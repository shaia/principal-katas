package weir

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

// ErrRegistryFull is returned when every producer slot is taken.
var ErrRegistryFull = errors.New("weir: producer registry full")

// ChannelPipeline is the idiomatic Go answer: one buffered channel per producer,
// drained by a single consumer that batches into a Sink.
//
// WHY THIS SHAPE, FROM GO'S OWN PRINCIPLES.
//
// The reflexive Go design is one shared buffered channel, and it is wrong here
// for a reason Go itself explains. A buffered channel is a runtime.hchan:
//
//	type hchan struct {
//	    qcount, dataqsiz uint
//	    buf              unsafe.Pointer
//	    sendx, recvx     uint
//	    recvq, sendq     waitq
//	    lock             mutex
//	}
//
// Every successful send takes that lock. With one channel and N producers the
// lock is contended, and measured on a 32-core machine it degrades from 36 ns
// per send at one producer to 1552 ns at thirty-two — a 42x collapse, because
// losers spin, then osyield, then sleep on a kernel semaphore.
//
// Go's answer to a contended resource is not to hand-roll a lock-free structure.
// It is to **shard** so nothing is contended. One channel per producer means
// each hchan.lock has exactly one sender and one receiver, so it is uncontended
// by construction, and an uncontended lock in Go is a single CAS.
//
// That gets you, for free and in about a hundred lines of ordinary Go:
//
//   - per-producer ordering, which is all the specification asks for
//   - bounded memory, because the buffer is a capacity you pass to make
//   - a non-blocking send, which is the idiomatic spelling of DropNewest and
//     the only channel discipline satisfying "producers must not block
//     indefinitely"
//   - no unsafe, no manual cache-line padding, no memory-model reasoning, and
//     nothing the race detector needs to be pointed at
//
// The consumer drains each channel with a non-blocking receive loop rather than
// a select over N cases, because select cannot take a dynamic number of cases
// without reflect.Select, which allocates and is far slower than the loop it
// would replace.
//
// See Pipeline for the version that replaces each channel with a hand-rolled
// SPSC ring. It is roughly four times faster per push and considerably more
// code; solution.md section 11 has the measurements and the argument for when
// that trade is worth taking.
type ChannelPipeline struct {
	cfg   config
	sink  Sink
	slots []chanSlot

	cancel  context.CancelFunc
	done    chan struct{}
	started atomic.Bool
	closed  atomic.Bool

	mu       sync.Mutex // guards registration only; never on the send path
	nextSlot int

	consumed atomic.Uint64
	passes   atomic.Uint64
	probes   atomic.Uint64
}

type chanSlot struct {
	ch     chan Event
	inUse  atomic.Bool
	closed atomic.Bool

	// Producer-owned: single writer, so relaxed counters are exact.
	pushed    atomic.Uint64
	dropped   atomic.Uint64
	highWater atomic.Uint64
}

// config is populated by Options. Unexported so the zero value cannot escape
// into user code half-built.
type config struct {
	producers   int
	capacity    int
	batch       int
	idleBackoff time.Duration
}

func defaults() config {
	return config{
		producers:   MaxProducers,
		capacity:    RingCapacity,
		batch:       StageEvents,
		idleBackoff: 50 * time.Microsecond,
	}
}

// An Option configures a pipeline. Functional options rather than an exported
// struct: the set of knobs is expected to grow, and this way adding one is not a
// breaking change to anyone's composite literal.
type Option func(*config)

// WithProducers sets how many producer slots exist. Memory is
// producers x capacity x sizeof(Event).
func WithProducers(n int) Option { return func(c *config) { c.producers = n } }

// WithCapacity sets each producer's buffer depth, in events.
func WithCapacity(n int) Option { return func(c *config) { c.capacity = n } }

// WithBatch sets how many events the consumer accumulates before one Sink call.
func WithBatch(n int) Option { return func(c *config) { c.batch = n } }

// WithIdleBackoff sets how long the consumer waits after a pass that found
// nothing. Latency against wakeups; the default is a reasonable middle.
func WithIdleBackoff(d time.Duration) Option { return func(c *config) { c.idleBackoff = d } }

// NewChannelPipeline returns a pipeline that has not started. Call Start.
func NewChannelPipeline(sink Sink, opts ...Option) *ChannelPipeline {
	cfg := defaults()
	for _, o := range opts {
		o(&cfg)
	}
	p := &ChannelPipeline{
		cfg:   cfg,
		sink:  sink,
		slots: make([]chanSlot, cfg.producers),
		done:  make(chan struct{}),
	}
	for i := range p.slots {
		p.slots[i].ch = make(chan Event, cfg.capacity)
	}
	return p
}

// Start launches the consumer. It returns an error if called twice.
//
// The context is the shutdown signal: cancel it, or call Close, and the consumer
// drains what is already buffered and returns. Using a context rather than a
// bespoke stop() is what lets this compose with the rest of a Go program —
// signal handling, request scopes, errgroup — instead of needing its own
// lifecycle wired in by hand.
func (p *ChannelPipeline) Start(ctx context.Context) error {
	if !p.started.CompareAndSwap(false, true) {
		return errors.New("weir: pipeline already started")
	}
	ctx, p.cancel = context.WithCancel(ctx)
	go p.consume(ctx)
	return nil
}

// Close stops accepting, drains, and waits for the consumer. It is idempotent
// and safe to defer.
func (p *ChannelPipeline) Close() error {
	if !p.closed.CompareAndSwap(false, true) {
		return nil
	}
	if p.cancel != nil {
		p.cancel()
	}
	select {
	case <-p.done:
	case <-time.After(5 * time.Second):
		return errors.New("weir: consumer did not stop within 5s")
	}
	return nil
}

// Producer claims a slot. The returned Producer is NOT safe for concurrent use:
// exactly one goroutine may call Push on it. Close it when done, ideally with
// defer.
func (p *ChannelPipeline) Producer() (*ChannelProducer, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	for i := 0; i < len(p.slots); i++ {
		idx := (p.nextSlot + i) % len(p.slots)
		if p.slots[idx].inUse.CompareAndSwap(false, true) {
			p.nextSlot = idx + 1
			s := &p.slots[idx]
			s.pushed.Store(0)
			s.dropped.Store(0)
			s.highWater.Store(0)
			return &ChannelProducer{p: p, slot: s, id: uint32(idx)}, nil
		}
	}
	return nil, fmt.Errorf("%w (%d slots)", ErrRegistryFull, len(p.slots))
}

// Stats reports the pipeline's counters.
func (p *ChannelPipeline) Stats() Stats {
	var s Stats
	for i := range p.slots {
		s.Pushed += p.slots[i].pushed.Load()
		s.Dropped += p.slots[i].dropped.Load()
		if hw := p.slots[i].highWater.Load(); hw > s.HighWater {
			s.HighWater = hw
		}
	}
	s.Consumed = p.consumed.Load()
	s.Passes = p.passes.Load()
	s.RingsProbed = p.probes.Load()
	return s
}

func (p *ChannelPipeline) consume(ctx context.Context) {
	defer close(p.done)
	batch := make([]Event, 0, p.cfg.batch)

	drain := func() int {
		total := 0
		for i := range p.slots {
			s := &p.slots[i]
			if !s.inUse.Load() && len(s.ch) == 0 {
				continue
			}
			p.probes.Add(1)
			// Non-blocking batch receive. A select over N channels would need
			// reflect.Select for a dynamic N, which allocates a []SelectCase per
			// call and is an order of magnitude slower than this loop.
			for len(batch) < cap(batch) {
				select {
				case e := <-s.ch:
					batch = append(batch, e)
					total++
				default:
					goto next
				}
			}
		next:
			if len(batch) == cap(batch) {
				p.flush(&batch)
			}
		}
		p.flush(&batch)
		return total
	}

	for {
		n := drain()
		p.passes.Add(1)
		if n > 0 {
			continue
		}
		select {
		case <-ctx.Done():
			// Final drain: whatever is buffered still goes out, so a clean
			// shutdown loses nothing.
			for drain() > 0 {
			}
			return
		case <-time.After(p.cfg.idleBackoff):
		}
	}
}

func (p *ChannelPipeline) flush(batch *[]Event) {
	if len(*batch) == 0 {
		return
	}
	p.consumed.Add(uint64(len(*batch)))
	p.sink.Write(EventsAsBytes(*batch))
	*batch = (*batch)[:0]
}

// ChannelProducer is one producer's handle. Not safe for concurrent use.
type ChannelProducer struct {
	p    *ChannelPipeline
	slot *chanSlot
	id   uint32
}

// ID reports the producer's slot index.
func (c *ChannelProducer) ID() uint32 { return c.id }

// Push submits an event, reporting whether it was accepted.
//
// A non-blocking send is the idiomatic Go spelling of DropNewest, and it is the
// only channel discipline that satisfies "producers must not block
// indefinitely". The reflexive `ch <- e` blocks when the buffer is full and
// violates that requirement outright.
func (c *ChannelProducer) Push(e Event) bool {
	if c.p.closed.Load() {
		return false
	}
	select {
	case c.slot.ch <- e:
		c.slot.pushed.Add(1)
		if d := uint64(len(c.slot.ch)); d > c.slot.highWater.Load() {
			c.slot.highWater.Store(d)
		}
		return true
	default:
		c.slot.dropped.Add(1) // loss is counted, never silent
		return false
	}
}

// Close releases the slot for reuse.
func (c *ChannelProducer) Close() error {
	if c.slot.closed.CompareAndSwap(false, true) {
		// The consumer will drain whatever is left; the slot becomes reusable
		// once it is empty, which the next Producer() call observes.
		go func(s *chanSlot) {
			for len(s.ch) > 0 {
				time.Sleep(50 * time.Microsecond)
			}
			s.closed.Store(false)
			s.inUse.Store(false)
		}(c.slot)
	}
	return nil
}
