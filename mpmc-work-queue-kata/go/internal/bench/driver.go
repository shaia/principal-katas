package bench

import (
	"errors"
	"math"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"turnstile/internal/tsc"
	"turnstile/internal/turnstile"
)

// Queue is what the driver runs. Each goroutine takes its own handle, as the
// ring's API asks; a channel's handles are thin.
type Queue interface {
	Producer() Producer
	Consumer() Consumer
	Close()
	// ProducersDone is called once every producer has returned from its last
	// push: the moment a channel's data channel can be closed.
	ProducersDone()
	Len() int
}

type Producer interface {
	TryPush(e turnstile.Event) error // nil, turnstile.ErrFull or turnstile.ErrClosed
	Push(e turnstile.Event) error    // the arm's blocking push: nil or turnstile.ErrClosed
	Stats() turnstile.Stats
}

type Consumer interface {
	Pop() (turnstile.Event, error) // turnstile.ErrClosed once drained
	Stats() turnstile.Stats
}

// Mode is the producers' full policy. Mixed: even producers reject, odd ones
// block — both reservation styles on one tail word at once.
type Mode uint8

const (
	Block Mode = iota
	Reject
	Mixed
)

// The brief's numbers.
const (
	Capacity  = 1 << 16
	Producers = 32
	Consumers = 16
	BriefRate = 5e6
	BudgetNs  = 2000
	WorkNs    = 1000 // what a consumer spends on each event in the paced runs
)

type Spec struct {
	Producers, Consumers int

	// Saturated when Rate == 0: each producer pushes Events as fast as it can.
	Events uint64

	// Paced when Rate > 0: Rate events/s in aggregate for Seconds. Producers
	// sleep between bursts and push whatever has come due — so 32 of them do
	// not hold 32 Ps doing nothing.
	Rate, Seconds float64

	// What a paced producer asks to sleep between bursts; 200 µs when zero,
	// which Go's timer turns into about half a millisecond here. The burst a
	// producer pushes on waking is whatever came due while it slept, so this
	// sets the arrival process the queue sees as much as Rate does.
	Nap time.Duration

	WorkNs  float64
	Mode    Mode
	Latency bool

	// Close this long after the start, with producers still running.
	CloseAfter time.Duration
}

type Result struct {
	Offered, Accepted, Rejected, RefusedClosed uint64
	Consumed, Torn, Disorder                   uint64
	ExactlyOnce                                bool
	// Accepted pushes that began after Close had returned: the shutdown
	// signal lost a race to the send.
	AfterClose uint64

	TGo, TClose, TEnd, FirstFull uint64 // counter readings; FirstFull is 0 if nobody saw full
	BacklogAtClose               uint64
	Seconds, Rate                float64

	Push  Histogram // one push call
	Ready Histogram // from when the event could first have been pushed to accepted
	E2E   Histogram // push start to pop, across cores: phase 0 checks the counter skew

	ProducerOps, ConsumerOps turnstile.Stats
	Bursts                   uint64
	ProducersUnfinished      int  // producers that ended on something other than their quota
	ProducersRefused         int  // producers refused on full at least once
	ClosedEarly              bool // closed at CloseAfter, so producers were meant to be cut off

	// Over the measured window, from release to the last consumer's exit: what
	// the collector and the allocator did while the queue was running.
	GCs     uint32
	Mallocs uint64
}

type producerState struct {
	_                                [64]byte
	accepted                         turnstile.Tally
	offered, rejected, refusedClosed uint64
	afterClose, firstFull, tDone     uint64
	bursts                           uint64
	finished                         bool
	push, ready                      Histogram
	ops                              turnstile.Stats
}

type consumerState struct {
	_            [64]byte
	consumed     turnstile.Tally
	torn, disord uint64
	tLast        uint64
	lastSeq      [Producers * 4]int64
	e2e          Histogram
	ops          turnstile.Stats
}

func (s *producerState) account(err error, id, seq uint32, now uint64) {
	s.offered++
	switch {
	case err == nil:
		s.accepted.Add(id, seq)
	case errors.Is(err, turnstile.ErrClosed):
		s.refusedClosed++
	default:
		s.rejected++
		if s.firstFull == 0 {
			s.firstFull = now
		}
	}
}

// Run releases P producers and C consumers on q together, closes q when the
// producers finish (or at CloseAfter), and merges what each counted.
func Run(q Queue, spec Spec) Result {
	P, C := spec.Producers, spec.Consumers
	if spec.Nap == 0 {
		spec.Nap = 200 * time.Microsecond
	}
	ps := make([]producerState, P)
	cs := make([]consumerState, max(C, 1))
	if P > len(cs[0].lastSeq) {
		Check(false, "a run has at most %d producers, the ones the order check tracks (asked for %d)",
			len(cs[0].lastSeq), P)
		return Result{}
	}

	var ready atomic.Int32
	var gate atomic.Bool
	waitGo := func() {
		ready.Add(1)
		for !gate.Load() {
			runtime.Gosched()
		}
	}

	var work uint64
	if spec.WorkNs > 0 {
		work = Clk.Ticks(spec.WorkNs)
	}
	paced := spec.Rate > 0
	var interval, duration uint64
	if paced {
		interval = Clk.Ticks(1e9 * float64(P) / spec.Rate)
		duration = Clk.Ticks(spec.Seconds * 1e9)
	}
	var tGo, closeTick atomic.Uint64

	var producers, consumers sync.WaitGroup
	producers.Add(P)
	for id := 0; id < P; id++ {
		go func(id int) {
			defer producers.Done()
			s := &ps[id]
			pid := uint32(id)
			mode := spec.Mode
			if mode == Mixed {
				mode = Reject
				if id%2 == 1 {
					mode = Block
				}
			}
			p := q.Producer()
			defer func() { s.ops = p.Stats() }()
			waitGo()
			start := tGo.Load()

			// pushTimed reports whether the producer may go on: false once the
			// queue has refused it as closed.
			pushTimed := func(seq uint32, readyAt uint64) bool {
				t0 := tsc.Ticks()
				e := turnstile.MakeEvent(pid, seq, t0)
				var err error
				if mode == Reject {
					err = p.TryPush(e)
				} else {
					err = p.Push(e)
				}
				t1 := tsc.Ticks()
				if spec.Latency {
					s.push.Record(t1 - t0)
					s.ready.Record(t1 - min(readyAt, t0))
				}
				if err == nil && spec.CloseAfter > 0 {
					if ct := closeTick.Load(); ct != 0 && t0 > ct {
						s.afterClose++
					}
				}
				s.account(err, pid, seq, t1)
				return !errors.Is(err, turnstile.ErrClosed)
			}

			if !paced {
				quota := spec.Events
				if quota == 0 {
					quota = math.MaxUint32
				}
				for i := uint64(0); i < quota; i++ {
					if !pushTimed(uint32(i), tsc.Ticks()) {
						break
					}
				}
				s.finished = spec.Events != 0 && s.offered == spec.Events
			} else {
				// Staggered starts, so 32 producers do not arrive in lockstep.
				next := start + interval*uint64(id)/uint64(P)
				end := start + duration
				woke := start
				var seq uint32
				open := true
				for open && next < end {
					now := tsc.Ticks()
					if now < next {
						time.Sleep(spec.Nap)
						woke = tsc.Ticks()
						continue
					}
					s.bursts++
					for next <= now && next < end {
						open = pushTimed(seq, max(next, woke))
						seq++
						next += interval
						now = tsc.Ticks()
						if !open {
							break
						}
					}
				}
				s.finished = open
			}
			s.tDone = tsc.Ticks()
		}(id)
	}

	consumers.Add(C)
	for id := 0; id < C; id++ {
		go func(id int) {
			defer consumers.Done()
			s := &cs[id]
			for i := range s.lastSeq {
				s.lastSeq[i] = -1
			}
			c := q.Consumer()
			defer func() { s.ops = c.Stats() }()
			waitGo()
			for {
				ev, err := c.Pop()
				if errors.Is(err, turnstile.ErrClosed) {
					return
				}
				t := tsc.Ticks()
				s.tLast = t
				if !turnstile.Intact(&ev) || int(ev.Producer) >= len(s.lastSeq) {
					s.torn++
					continue
				}
				if spec.Latency {
					s.e2e.Record(t - ev.Stamp)
				}
				s.consumed.Add(ev.Producer, ev.Seq)
				// One consumer claims positions in increasing order and a producer's
				// events occupy increasing positions, so from any one consumer's view
				// each producer's sequence only goes up.
				if int64(ev.Seq) <= s.lastSeq[ev.Producer] {
					s.disord++
				}
				s.lastSeq[ev.Producer] = int64(ev.Seq)
				if work != 0 {
					tsc.Burn(work)
				}
			}
		}(id)
	}

	for int(ready.Load()) < P+C {
		runtime.Gosched()
	}
	// Collect now, with every goroutine waiting at the gate, so the run starts
	// with nothing owed to the collector; then count what the run itself does.
	runtime.GC()
	var m0, m1 runtime.MemStats
	runtime.ReadMemStats(&m0)
	tGo.Store(tsc.Ticks())
	gate.Store(true)

	var out Result
	out.TGo = tGo.Load()
	finish := func() {
		out.BacklogAtClose = uint64(q.Len())
		out.TClose = tsc.Ticks()
		q.Close()
		closeTick.Store(tsc.Ticks())
	}
	if spec.CloseAfter > 0 {
		time.Sleep(spec.CloseAfter)
		finish()
		producers.Wait()
	} else {
		producers.Wait()
		finish()
	}
	// Every producer has returned from its last push, so the data channel, if
	// there is one, can be closed without a send racing the close.
	q.ProducersDone()
	consumers.Wait()
	runtime.ReadMemStats(&m1)
	out.GCs = m1.NumGC - m0.NumGC
	out.Mallocs = m1.Mallocs - m0.Mallocs

	var accepted, consumed turnstile.Tally
	for i := range ps {
		s := &ps[i]
		accepted.Merge(s.accepted)
		out.Offered += s.offered
		out.Rejected += s.rejected
		out.RefusedClosed += s.refusedClosed
		out.AfterClose += s.afterClose
		out.Push.Merge(&s.push)
		out.Ready.Merge(&s.ready)
		out.ProducerOps.Add(s.ops)
		out.Bursts += s.bursts
		if !s.finished {
			out.ProducersUnfinished++
		}
		if s.rejected > 0 {
			out.ProducersRefused++
		}
		if s.firstFull != 0 && (out.FirstFull == 0 || s.firstFull < out.FirstFull) {
			out.FirstFull = s.firstFull
		}
	}
	for i := 0; i < C; i++ {
		s := &cs[i]
		consumed.Merge(s.consumed)
		out.Torn += s.torn
		out.Disorder += s.disord
		out.E2E.Merge(&s.e2e)
		out.ConsumerOps.Add(s.ops)
		out.TEnd = max(out.TEnd, s.tLast)
	}
	out.Accepted, out.Consumed = accepted.N, consumed.N
	out.ExactlyOnce = C > 0 && accepted == consumed && out.Torn == 0
	out.ClosedEarly = spec.CloseAfter > 0
	if out.TEnd > out.TGo {
		out.Seconds = Clk.Ns(out.TEnd-out.TGo) / 1e9
		out.Rate = float64(out.Consumed) / out.Seconds
	}
	return out
}

// CheckRun asserts the invariants every run must satisfy, under the labels the
// C++ track uses.
//
// The first is the harness checking its own accounting — every push ends one of
// three ways — and cannot catch a queue bug. The rest can.
func CheckRun(r *Result, label string) {
	Check(r.Offered == r.Accepted+r.Rejected+r.RefusedClosed, "%s: offered == accepted + rejected + refused", label)
	Check(r.ClosedEarly || r.ProducersUnfinished == 0, "%s: with no early close, every producer finished (%d did not)",
		label, r.ProducersUnfinished)
	Check(r.Torn == 0, "%s: no event read torn or unpublished (%d)", label, r.Torn)
	Check(r.ExactlyOnce, "%s: every accepted event consumed exactly once (accepted %d, consumed %d)",
		label, r.Accepted, r.Consumed)
	Check(r.Disorder == 0, "%s: per consumer, each producer's events arrive in order (%d out of order)",
		label, r.Disorder)
}
