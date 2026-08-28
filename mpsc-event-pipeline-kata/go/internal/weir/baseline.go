package weir

import (
	"sync"
	"sync/atomic"
)

// Transport is the contract the three-way A/B runs against, so the rings, a
// channel and a mutex can be measured through identical harness, sink and pacer
// code with nothing else varying.
type Transport interface {
	Name() string
	Register() (Producer, bool)
	Start(Sink)
	Stop(StopMode)
	Stats() Stats
}

// Producer is the handle side of Transport.
type Producer interface {
	Push(Event) bool
	ID() uint32
	Close()
}

// Pipeline and ProducerHandle satisfy them directly.
func (p *Pipeline) Name() string { return "rings" }
func (p *Pipeline) Register() (Producer, bool) {
	h, ok := p.RegisterProducer()
	if !ok {
		return nil, false
	}
	return h, true
}

var (
	_ Transport = (*Pipeline)(nil)
	_ Producer  = (*ProducerHandle)(nil)
)

// ---------------------------------------------------------------------------
// The idiomatic baseline
// ---------------------------------------------------------------------------

// ChanTransport is one buffered channel shared by every producer.
//
// THIS IS THE DESIGN SECTION 1 REJECTS, AND IT IS WHAT EVERY GO ENGINEER WRITES
// FIRST. That coincidence is the most interesting thing about porting this kata
// to Go, and it deserves to be measured rather than argued.
//
// A buffered chan is a runtime.hchan:
//
//	type hchan struct {
//	    qcount, dataqsiz uint
//	    buf              unsafe.Pointer
//	    sendx, recvx     uint
//	    recvq, sendq     waitq
//	    lock             mutex        // <- here
//	}
//
// A mutex plus a circular buffer. Every successful send takes lock(&c.lock);
// the lock-free fast path in chansend covers only the *failed* non-blocking
// case. All of those fields live in the same two cache lines, so every
// producer's send ping-pongs the same lines between cores — precisely the
// single shared writable cache line the per-producer ring design exists to
// eliminate. On Windows that mutex is lock_sema.go: spin, osyield, then sleep on
// a kernel semaphore, so losers park in the kernel exactly as section 1
// describes for a mutex.
//
// One thing it does NOT lose: a channel preserves per-producer ordering just
// fine, so the specification's decisive clause does not discriminate between the
// two designs. What it loses is capacity isolation and per-producer accounting
// — see the writeup.
//
// The overload policy is a non-blocking send, which is the idiomatic Go spelling
// of DropNewest and the only channel discipline that satisfies "producers must
// not block indefinitely". A blocking send, the reflexive Go answer, violates a
// hard requirement outright.
type ChanTransport struct {
	ch       chan Event
	capacity int
	accept   atomic.Bool
	done     chan struct{}
	stopOnce sync.Once
	nextID   atomic.Uint32

	pushed   atomic.Uint64
	dropped  atomic.Uint64
	consumed atomic.Uint64
	onEvent  func(*Event)
}

func NewChanTransport(capacity int) *ChanTransport {
	return &ChanTransport{
		ch:       make(chan Event, capacity),
		capacity: capacity,
		done:     make(chan struct{}),
	}
}

func (c *ChanTransport) Name() string { return "chan" }

func (c *ChanTransport) SetEventHook(f func(*Event)) { c.onEvent = f }

func (c *ChanTransport) Register() (Producer, bool) {
	return &chanProducer{t: c, id: c.nextID.Add(1) - 1}, true
}

func (c *ChanTransport) Start(sink Sink) {
	c.accept.Store(true)
	go func() {
		defer close(c.done)
		batch := make([]Event, 0, StageEvents)
		for {
			// Block for the first event, then take whatever else is ready
			// without spinning. This is the shape a real Go consumer has, and
			// it is genuinely simpler than the ring consumer's idle ladder —
			// a point in the channel's favour that the writeup should concede.
			e, ok := <-c.ch
			if !ok {
				break
			}
			batch = append(batch, e)
		fill:
			for len(batch) < StageEvents {
				select {
				case e, ok := <-c.ch:
					if !ok {
						break fill
					}
					batch = append(batch, e)
				default:
					break fill
				}
			}
			if c.onEvent != nil {
				for i := range batch {
					c.onEvent(&batch[i])
				}
			}
			c.consumed.Add(uint64(len(batch)))
			sink.Write(EventsAsBytes(batch))
			batch = batch[:0]
		}
	}()
}

func (c *ChanTransport) Stop(StopMode) {
	c.stopOnce.Do(func() {
		c.accept.Store(false)
		close(c.ch)
		<-c.done
	})
}

func (c *ChanTransport) Stats() Stats {
	return Stats{
		Pushed:   c.pushed.Load(),
		Dropped:  c.dropped.Load(),
		Consumed: c.consumed.Load(),
	}
}

type chanProducer struct {
	t  *ChanTransport
	id uint32
}

func (p *chanProducer) ID() uint32 { return p.id }
func (p *chanProducer) Close()     {}

func (p *chanProducer) Push(e Event) bool {
	if !p.t.accept.Load() {
		return false
	}
	select {
	case p.t.ch <- e:
		p.t.pushed.Add(1)
		return true
	default:
		p.t.dropped.Add(1)
		return false
	}
}

// ---------------------------------------------------------------------------
// The explicit spelling of what a channel is
// ---------------------------------------------------------------------------

// MutexTransport is sync.Mutex plus a preallocated ring: what ChanTransport is
// implicitly, written out so the comparison is not confounded by the runtime's
// direct-handoff optimisation (chansend copies straight into a waiting
// receiver's stack, which the rings cannot do and a plain mutex does not either).
type MutexTransport struct {
	mu       sync.Mutex
	buf      []Event
	head     uint64
	tail     uint64
	capacity uint64

	accept   atomic.Bool
	running  atomic.Bool
	done     chan struct{}
	stopOnce sync.Once
	nextID   atomic.Uint32

	pushed   atomic.Uint64
	dropped  atomic.Uint64
	consumed atomic.Uint64
	onEvent  func(*Event)
}

func NewMutexTransport(capacity int) *MutexTransport {
	return &MutexTransport{
		buf:      make([]Event, capacity),
		capacity: uint64(capacity),
		done:     make(chan struct{}),
	}
}

func (m *MutexTransport) Name() string                { return "mutex" }
func (m *MutexTransport) SetEventHook(f func(*Event)) { m.onEvent = f }

func (m *MutexTransport) Register() (Producer, bool) {
	return &mutexProducer{t: m, id: m.nextID.Add(1) - 1}, true
}

func (m *MutexTransport) Start(sink Sink) {
	m.accept.Store(true)
	m.running.Store(true)
	go func() {
		defer close(m.done)
		batch := make([]Event, StageEvents)
		for m.running.Load() {
			m.mu.Lock()
			n := uint64(len(batch))
			if avail := m.tail - m.head; avail < n {
				n = avail
			}
			for i := uint64(0); i < n; i++ {
				batch[i] = m.buf[(m.head+i)%m.capacity]
			}
			m.head += n
			m.mu.Unlock()

			if n == 0 {
				pause()
				continue
			}
			if m.onEvent != nil {
				for i := uint64(0); i < n; i++ {
					m.onEvent(&batch[i])
				}
			}
			m.consumed.Add(n)
			sink.Write(EventsAsBytes(batch[:n]))
		}
	}()
}

func (m *MutexTransport) Stop(StopMode) {
	m.stopOnce.Do(func() {
		m.accept.Store(false)
		m.running.Store(false)
		<-m.done
	})
}

func (m *MutexTransport) Stats() Stats {
	return Stats{
		Pushed:   m.pushed.Load(),
		Dropped:  m.dropped.Load(),
		Consumed: m.consumed.Load(),
	}
}

type mutexProducer struct {
	t  *MutexTransport
	id uint32
}

func (p *mutexProducer) ID() uint32 { return p.id }
func (p *mutexProducer) Close()     {}

func (p *mutexProducer) Push(e Event) bool {
	t := p.t
	if !t.accept.Load() {
		return false
	}
	t.mu.Lock()
	if t.tail-t.head == t.capacity {
		t.mu.Unlock()
		t.dropped.Add(1)
		return false
	}
	t.buf[t.tail%t.capacity] = e
	t.tail++
	t.mu.Unlock()
	t.pushed.Add(1)
	return true
}

var (
	_ Transport = (*ChanTransport)(nil)
	_ Transport = (*MutexTransport)(nil)
)
