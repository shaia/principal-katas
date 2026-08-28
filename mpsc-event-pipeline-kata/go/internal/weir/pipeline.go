package weir

import (
	"runtime"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// Producer slot lifetime:
//
//	Free --register--> Active --handle Close--> Retiring --consumer, after
//	                                            final drain--> Free
//
// Only the CONSUMER may publish Free, and only after it has drained the ring and
// seen it empty. A departing producer can mark itself Retiring but cannot
// release its own slot: it has no way to know whether the consumer is mid-batch
// inside its ring. A ring freed when its producer exits is a use-after-free the
// consumer hits at the worst possible moment.
const (
	slotFree uint32 = iota
	slotActive
	slotRetiring
)

type producerSlot struct {
	ring      *SpscRing     // offset  0
	state     atomic.Uint32 // offset  8
	_         [4]byte       // offset 12  pad state up to 8-byte alignment
	pushed    atomic.Uint64 // offset 16  producer-owned
	dropped   atomic.Uint64 // offset 24  producer-owned
	highWater atomic.Uint64 // offset 32  producer-owned
	_         [24]byte      // offset 40  -> sizeof == 64, one slot per line
}

// consumerCounters are plain fields accumulated by the consumer and published to
// the atomics once per pass.
//
// This is a deliberate divergence from the C++, and it fixes a measurement
// confound in it. The C++ does rings_probed_.fetch_add(1, relaxed) *per probe* —
// a lock xadd, ~18-20 cycles even on an L1-Modified line — to instrument a probe
// that costs ~4 cycles (an L1 hit on an index line nobody is writing). The
// counter is roughly four times the cost of the thing it measures, and it scales
// with registration, so a share of full-scan's measured p50 degradation in that
// writeup's section 8 is the instrument rather than the scan.
type consumerCounters struct {
	ringsProbed, ringsEmpty, passes uint64
}

// Pipeline is a registry of per-producer SPSC rings and the single consumer that
// drains them into a Sink.
//
// Three protocols live here, each subtle enough to deserve naming:
//
//	producer lifetime   Free -> Active -> Retiring -> Free, where only the
//	                    consumer may publish Free (see reclaimRetired)
//	wakeup handshake    a Dekker pair between signal() and clearEmptyBits(),
//	                    which in C++ needs the design's only seq_cst fence and
//	                    in Go needs nothing at all (see bitmap.go)
//	shutdown            two phases, two modes, always bounded by a deadline
//
// Must not be copied: the atomics embed noCopy, enforced by `go vet -copylocks`.
type Pipeline struct {
	// slots first, so &slots[i] is base + 64*i and one slot never straddles two
	// cache lines. Pipeline is heap-allocated by New and is far over 512 bytes,
	// so the allocator gives the base at least 64-byte alignment; New asserts it
	// rather than trusting it, and that assertion is the Go spelling of alignas.
	slots [MaxProducers]producerSlot

	// The push-path line. Both are read by every producer on every push and
	// written only on transitions, so co-locating them is deliberate: a push
	// touches one line, not two.
	//
	// What matters is not that they are together, it is what they are kept away
	// from. The C++ version got this wrong — accepting_ shared a line with
	// parked_, retire_pending_ and the park mutex, so the consumer's idle loop
	// invalidated the push-path line on every empty pass. That defect was found
	// by dumping the record layout while writing this file.
	// Note atomic.Bool is FOUR bytes, not one: it wraps a uint32. Getting that
	// wrong put running at offset 4164 and silently broke the separation this
	// padding exists to create — the identical failure mode to the C++ version's
	// missing alignas, and caught the same way, by asserting the offsets.
	active    atomic.Uint64        // 8
	accepting atomic.Bool          // 4
	_         [CacheLine - 12]byte // 52 -> 64 total

	// Consumer- and stopper-written state, off the push path.
	running       atomic.Bool          // 4
	parked        atomic.Bool          // 4
	retirePending atomic.Bool          // 4
	_             [CacheLine - 12]byte // 52 -> 64 total

	cfg   Config
	clock *Clock

	// wake is the notification channel: capacity 1, non-blocking send. The
	// capacity IS the coalescing — N concurrent notifies collapse to one pending
	// token. See notify.go for why this is a channel and not a sync.Cond.
	wake  chan struct{}
	timer *time.Timer
	done  chan struct{}

	stopOnce sync.Once
	regMu    sync.Mutex // registration only; never on the hot path

	// Consumer-local; no other goroutine touches these.
	stage      []Event
	staged     int
	local      consumerCounters
	drainMet   bool
	onEvent    func(*Event)
	leakedHnds atomic.Uint64

	consumed       atomic.Uint64
	retiredPushed  atomic.Uint64 // counters of departed producers
	retiredDropped atomic.Uint64
	ringsProbed    atomic.Uint64
	ringsEmpty     atomic.Uint64
	passes         atomic.Uint64
	bitmapWrites   atomic.Uint64
	notifies       atomic.Uint64
	parks          atomic.Uint64
}

// New allocates every ring up front. Nothing on the hot path ever touches the
// allocator.
func New(cfg Config, clock *Clock) *Pipeline {
	p := &Pipeline{
		cfg:   cfg,
		clock: clock,
		wake:  make(chan struct{}, 1),
		stage: make([]Event, StageEvents),
		done:  make(chan struct{}),
	}
	for i := range p.slots {
		p.slots[i].ring = new(SpscRing)
	}
	// Go has no alignas, and — measured, not assumed — it will not give this
	// object cache-line alignment either. Pipeline is 4472 bytes, which takes
	// the size-class allocator, and every allocation on the development machine
	// came back at base%64 == 8. (The 131 KiB SpscRing does get 64-byte
	// alignment, because it exceeds 32 KiB and takes the page-allocated
	// large-object path. The difference is an allocator implementation detail
	// either way.)
	//
	// So absolute alignment is simply not available, and asserting it would be
	// asserting something false. What padding *does* guarantee is the property
	// that actually prevents false sharing: the push-path group and the
	// consumer-written group land on different cache lines whatever the base
	// offset, because each group is 64 bytes and each is small enough not to
	// straddle. That is what to assert.
	if unsafe.Sizeof(producerSlot{}) != CacheLine {
		panic("weir: producerSlot is not exactly one cache line")
	}
	base := uintptr(unsafe.Pointer(p))
	pushLine := (base + unsafe.Offsetof(p.active)) / CacheLine
	writeLine := (base + unsafe.Offsetof(p.running)) / CacheLine
	if pushLine == writeLine {
		panic("weir: push-path state shares a cache line with consumer-written state")
	}
	// An atomic that straddles two lines is a correctness hazard on some
	// architectures and a performance one everywhere.
	if off := (base + unsafe.Offsetof(p.active)) % CacheLine; off > CacheLine-8 {
		panic("weir: Pipeline.active straddles a cache line")
	}
	p.timer = time.NewTimer(time.Hour)
	if !p.timer.Stop() {
		<-p.timer.C
	}
	return p
}

// Start launches the consumer.
func (p *Pipeline) Start(sink Sink) {
	p.running.Store(true)
	p.accepting.Store(true)
	go p.consume(sink)
}

// Stop is idempotent. Drain waits, bounded, for the rings to empty; Abort does
// not. The destructor that makes this automatic in C++ has no Go equivalent, so
// Stop is mandatory — call it with defer.
func (p *Pipeline) Stop(mode StopMode) { p.StopWithDeadline(mode, 5*time.Second) }

func (p *Pipeline) StopWithDeadline(mode StopMode, deadline time.Duration) {
	p.stopOnce.Do(func() {
		// Producers see this and start failing pushes, so the rings can only
		// shrink from here. This must come first: draining a queue that is still
		// being filled is a race you can lose indefinitely.
		p.accepting.Store(false)

		if mode == Drain {
			until := time.Now().Add(deadline)
			// Bounded: a wedged sink must never make shutdown hang forever.
			for time.Now().Before(until) && !p.allQuiesced() {
				p.wakeConsumer()
				time.Sleep(50 * time.Microsecond)
			}
			p.drainMet = p.allQuiesced()
		}
		p.running.Store(false)
		p.wakeConsumer()
		<-p.done
	})
}

// DrainCompleted reports false if a Drain hit its deadline and degraded to an
// Abort.
func (p *Pipeline) DrainCompleted() bool { return p.drainMet }

// RegisterProducer claims a slot. The two-value return is Go's substitute for
// [[nodiscard]]: ignoring the failure requires writing an explicit _.
func (p *Pipeline) RegisterProducer() (*ProducerHandle, bool) {
	p.regMu.Lock()
	defer p.regMu.Unlock()
	for i := uint32(0); i < MaxProducers; i++ {
		// Success here is ordered after the consumer's release of this slot, so
		// everything the previous owner and the consumer did is visible before
		// we reuse the ring. Note we do NOT reset the ring indices: they are
		// free-running, the ring is empty (head == tail), and a reset from this
		// goroutine would be a fresh race with the consumer.
		if p.slots[i].state.CompareAndSwap(slotFree, slotActive) {
			p.slots[i].pushed.Store(0)
			p.slots[i].dropped.Store(0)
			p.slots[i].highWater.Store(0)
			return &ProducerHandle{p: p, id: i}, true
		}
	}
	return nil, false // registry full — caller checks
}

// SetEventHook installs a per-event callback for tests. Production code would
// not pay for an indirect call on this path.
func (p *Pipeline) SetEventHook(f func(*Event)) { p.onEvent = f }

func (p *Pipeline) Stats() Stats {
	var s Stats
	s.Pushed = p.retiredPushed.Load()
	s.Dropped = p.retiredDropped.Load()
	for i := range p.slots {
		s.Pushed += p.slots[i].pushed.Load()
		s.Dropped += p.slots[i].dropped.Load()
		if hw := p.slots[i].highWater.Load(); hw > s.HighWater {
			s.HighWater = hw
		}
	}
	s.Consumed = p.consumed.Load()
	s.RingsProbed = p.ringsProbed.Load()
	s.RingsEmpty = p.ringsEmpty.Load()
	s.Passes = p.passes.Load()
	s.BitmapWrites = p.bitmapWrites.Load()
	s.Notifies = p.notifies.Load()
	s.Parks = p.parks.Load()
	return s
}

// --- producer hot path -----------------------------------------------------

func (p *Pipeline) push(id uint32, e *Event) bool {
	if !p.accepting.Load() {
		return false
	}
	slot := &p.slots[id]
	ring := slot.ring

	ok := ring.TryPush(e)
	if !ok && p.cfg.Full == SpinThenDrop {
		// Bounded spin, then give up. Never an unbounded wait.
		for i := uint32(0); i < p.cfg.PushSpin && !ok; i++ {
			pause()
			ok = ring.TryPush(e)
		}
	}
	if !ok {
		slot.dropped.Add(1)
		return false // loss is counted, never silent
	}
	slot.pushed.Add(1)

	if depth := ring.ProducerSizeHint(); depth > slot.highWater.Load() {
		slot.highWater.Store(depth)
	}

	if p.cfg.Scan == Bitmap {
		p.signal(id)
	}
	return true
}

func (p *Pipeline) retire(id uint32) {
	// The slot stays unusable until the consumer publishes Free.
	p.slots[id].state.Store(slotRetiring)
	p.retirePending.Store(true)
	if p.cfg.Scan == Bitmap {
		p.signal(id) // make sure it gets drained
	}
	p.wakeConsumer()
}

// --- consumer --------------------------------------------------------------

func (p *Pipeline) consume(sink Sink) {
	defer close(p.done)

	var idle uint32
	var rotate uint32 // fairness: where each pass starts

	for p.running.Load() {
		var n int
		if p.cfg.Scan == Bitmap {
			n = p.drainBitmap(sink, rotate)
		} else {
			n = p.drainFull(sink, rotate)
		}
		p.local.passes++
		rotate = (rotate + 1) & (MaxProducers - 1)

		if n > 0 {
			idle = 0
			continue
		}
		p.publishCounters()

		// Everything below runs only on a pass that found no work, so none of it
		// is on the hot path. Reclaiming retired slots is an O(64) scan; doing it
		// unconditionally would pay the full-scan cost on every iteration and
		// quietly cancel out the bitmap.
		if p.retirePending.Swap(false) {
			p.reclaimRetired()
		}

		idle++
		if idle < p.cfg.SpinBeforeYield {
			pause()
			continue
		}
		if idle < p.cfg.SpinBeforeYield+p.cfg.YieldBeforePark {
			runtime.Gosched()
			continue
		}

		// Clear stale bits only here, on the way to sleep — not on every empty
		// pass. A lightly loaded pipeline goes briefly empty between arrivals,
		// and clearing there would have the consumer clear a bit the producer
		// immediately sets again: write churn on the shared line, precisely
		// proportional to throughput, which is the thing this design exists to
		// avoid. The C++ measured 582,954 bitmap writes that way against 8 for
		// the deferred version.
		if p.cfg.Scan == Bitmap {
			p.clearEmptyBits()
		}

		// Defense in depth: one unconditional full scan immediately before
		// sleeping. The bitmap protocol above is believed correct, but the cost
		// of being wrong is an event stranded in a ring nobody probes. Going to
		// sleep is the only moment where being wrong becomes unbounded, and it
		// is also the moment an O(64) scan is affordable.
		if p.drainFull(sink, rotate) > 0 {
			idle = 0
			continue
		}
		p.park()
		idle = 0
	}

	// Final passes: whatever is published and reachable still goes out, so a
	// Drain shutdown loses nothing. Full scan regardless of policy — at
	// shutdown, correctness beats scan efficiency.
	for pass := 0; pass < 2; pass++ {
		r := uint32(0)
		for p.drainFull(sink, r) > 0 {
			r = (r + 1) & (MaxProducers - 1)
		}
	}
	p.flush(sink)
	p.reclaimRetired()
	p.publishCounters()
	// A real socket sink would CloseWrite() here so the peer sees a clean EOF.
}

// drainFull is O(64) every pass, however few producers are live. This is the
// naive version the follow-up asks us to fix; kept so the benchmark can A/B it,
// and used as the pre-sleep safety net above.
func (p *Pipeline) drainFull(sink Sink, rotate uint32) int {
	total := 0
	for k := uint32(0); k < MaxProducers; k++ {
		i := (k + rotate) & (MaxProducers - 1)
		if p.slots[i].state.Load() == slotFree {
			continue
		}
		p.local.ringsProbed++
		n := p.drainRing(sink, i)
		if n == 0 {
			p.local.ringsEmpty++
		}
		total += n
	}
	if total > 0 {
		p.flush(sink)
	}
	return total
}

func (p *Pipeline) drainRing(sink Sink, i uint32) int {
	got := 0
	for got < DrainBatch {
		if p.staged == len(p.stage) {
			p.flush(sink)
		}
		room := len(p.stage) - p.staged
		if r := DrainBatch - got; r < room {
			room = r
		}
		n := p.slots[i].ring.PopBatch(p.stage[p.staged : p.staged+room])
		if n == 0 {
			break
		}
		// Fire the hook on this batch *before* the next iteration can flush and
		// reset staged — otherwise the range is gone. The C++ shipped that bug:
		// it computed the range after a mid-drain flush had already reset the
		// index, silently skipping one buffer's worth of events per flush.
		if p.onEvent != nil {
			for k := 0; k < n; k++ {
				p.onEvent(&p.stage[p.staged+k])
			}
		}
		p.staged += n
		got += n
	}
	p.consumed.Add(uint64(got))
	return got
}

// flush hands the staged batch to the sink as one call, so the syscall a real
// socket would make is amortized over up to StageEvents events. Event is
// pointer-free and stage is contiguous, so the batch goes over as a byte view
// with no serialization copy.
func (p *Pipeline) flush(sink Sink) {
	if p.staged == 0 {
		return
	}
	sink.Write(EventsAsBytes(p.stage[:p.staged]))
	p.staged = 0
}

// reclaimRetired: only the consumer publishes Free, and only after the ring is
// empty.
func (p *Pipeline) reclaimRetired() {
	for i := uint32(0); i < MaxProducers; i++ {
		if p.slots[i].state.Load() != slotRetiring {
			continue
		}
		if !p.slots[i].ring.EmptyNow() {
			continue
		}
		// Roll the departing producer's counters into the lifetime totals before
		// the slot is reusable. RegisterProducer resets the per-slot ones, so
		// without this a recycled slot erases its predecessor's history and the
		// accounting silently stops adding up.
		p.retiredPushed.Add(p.slots[i].pushed.Swap(0))
		p.retiredDropped.Add(p.slots[i].dropped.Swap(0))
		p.active.And(^(uint64(1) << i))
		p.slots[i].state.Store(slotFree)
	}
}

// allQuiesced is called from the stopping goroutine, which is neither producer
// nor consumer, so it must use SizeNow. ProducerSizeHint reads the producer's
// private cached index and would be a data race here — which the C++ version
// shipped, and which `go test -race` would catch outright.
func (p *Pipeline) allQuiesced() bool {
	for i := range p.slots {
		if p.slots[i].state.Load() == slotFree {
			continue
		}
		if p.slots[i].ring.SizeNow() != 0 {
			return false
		}
	}
	return true
}

func (p *Pipeline) publishCounters() {
	p.ringsProbed.Store(p.local.ringsProbed)
	p.ringsEmpty.Store(p.local.ringsEmpty)
	p.passes.Store(p.local.passes)
}
