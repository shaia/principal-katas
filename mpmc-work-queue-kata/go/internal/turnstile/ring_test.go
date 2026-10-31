package turnstile

import (
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"weak"
)

type queue interface {
	TryPush(e Event, st *OpStats) Push
	Push(e Event, until time.Time, st *OpStats) Push
	TryPop(e *Event, st *OpStats) Pop
	Pop(e *Event, until time.Time, st *OpStats) Pop
	Close()
	ProducersDone()
	Occupancy() uint64
}

type mode int

const (
	block mode = iota
	reject
)

// stress pushes `events` from each producer, or pushes until refused as closed
// when closeAfter is set, and checks what the consumers took: every accepted
// event exactly once, intact, and in order per producer from each consumer's view.
func stress(t *testing.T, q queue, producers, consumers, events int, m mode, closeAfter time.Duration) {
	t.Helper()
	accepted := make([]Tally, producers)
	consumed := make([]Tally, consumers)
	closedSeen := make([]bool, producers)
	var torn, disorder atomic.Uint64
	var pw, cw sync.WaitGroup
	pw.Add(producers)
	for p := 0; p < producers; p++ {
		go func(p int) {
			defer pw.Done()
			var st OpStats
			for i := 0; closeAfter > 0 || i < events; i++ {
				e := MakeEvent(uint32(p), uint32(i), 0)
				var r Push
				if m == reject {
					for r = q.TryPush(e, &st); r == PushFull; r = q.TryPush(e, &st) {
						runtime.Gosched()
					}
				} else {
					r = q.Push(e, time.Now().Add(time.Minute), &st)
				}
				if r == PushClosed {
					closedSeen[p] = true
					return
				}
				if r != PushOK {
					t.Errorf("producer %d: push %d ended %d", p, i, r)
					return
				}
				accepted[p].Add(uint32(p), uint32(i))
			}
		}(p)
	}
	cw.Add(consumers)
	for c := 0; c < consumers; c++ {
		go func(c int) {
			defer cw.Done()
			var st OpStats
			last := make([]int64, producers)
			for i := range last {
				last[i] = -1
			}
			var e Event
			for {
				switch q.Pop(&e, time.Now().Add(time.Minute), &st) {
				case PopDrained:
					return
				case PopOK:
					if !Intact(&e) || int(e.Producer) >= producers {
						torn.Add(1)
						continue
					}
					if int64(e.Seq) <= last[e.Producer] {
						disorder.Add(1)
					}
					last[e.Producer] = int64(e.Seq)
					consumed[c].Add(e.Producer, e.Seq)
				}
			}
		}(c)
	}
	if closeAfter > 0 {
		time.Sleep(closeAfter)
		q.Close()
		pw.Wait()
	} else {
		pw.Wait()
		q.Close()
	}
	q.ProducersDone()
	cw.Wait()

	var a, c Tally
	for _, x := range accepted {
		a.Merge(x)
	}
	for _, x := range consumed {
		c.Merge(x)
	}
	if a != c {
		t.Errorf("accepted %d, consumed %d: not the same events", a.N, c.N)
	}
	if n := torn.Load(); n != 0 {
		t.Errorf("%d events read torn or unpublished", n)
	}
	if n := disorder.Load(); n != 0 {
		t.Errorf("%d events out of order for their producer", n)
	}
	if closeAfter > 0 {
		for p, seen := range closedSeen {
			if !seen {
				t.Errorf("producer %d was never refused as closed", p)
			}
		}
	}
}

// The race detector runs code an order of magnitude slower; the races it looks
// for need interleavings, not volume.
func events(n int) int {
	if raceEnabled {
		return n / 10
	}
	return n
}

// Two and four slots: every few events a lap wraps, with slots still reserved.
func TestLapWrap(t *testing.T) {
	for _, n := range []int{2, 4} {
		for _, c := range []struct {
			name string
			opt  Options
			m    mode
		}{
			{"cas-block", Options{}, block},
			{"ticket-block", Options{Reserve: ReserveTicket}, block},
			{"reject", Options{}, reject},
		} {
			t.Run(c.name, func(t *testing.T) {
				stress(t, NewRing[Event, struct{}](n, c.opt), 4, 4, events(20000), c.m, 0)
			})
		}
	}
}

func TestFullFigures(t *testing.T) {
	stress(t, NewRing[Event, LinePad](1<<10, Options{Reserve: ReserveTicket}), 8, 8, events(50000), block, 0)
	stress(t, NewChan[Event](1<<10, SendFast), 8, 8, events(50000), block, 0)
}

// With no consumer, exactly capacity events go in and none is overwritten.
func TestNeverOverwrites(t *testing.T) {
	for name, q := range map[string]queue{
		"ring": NewRing[Event, LinePad](64, Options{}),
		"chan": NewChan[Event](64, SendFast),
	} {
		var st OpStats
		n := 0
		for q.TryPush(MakeEvent(0, uint32(n), 0), &st) == PushOK {
			n++
		}
		if n != 64 {
			t.Errorf("%s: accepted %d into 64 slots", name, n)
		}
	}
}

// A close racing pushes strands nothing: every producer is refused, and every
// event accepted before the refusal is consumed.
func TestCloseVersusReserve(t *testing.T) {
	for _, c := range []struct {
		name string
		q    func() queue
		m    mode
	}{
		{"ring-cas-block", func() queue { return NewRing[Event, struct{}](64, Options{}) }, block},
		{"ring-ticket-block", func() queue { return NewRing[Event, struct{}](64, Options{Reserve: ReserveTicket}) }, block},
		{"ring-reject", func() queue { return NewRing[Event, struct{}](64, Options{}) }, reject},
		{"chan-fast", func() queue { return NewChan[Event](64, SendFast) }, block},
		{"chan-select", func() queue { return NewChan[Event](64, SendSelect) }, block},
	} {
		t.Run(c.name, func(t *testing.T) {
			for round := 0; round < 20; round++ {
				stress(t, c.q(), 8, 4, 0, c.m, time.Duration(round%4+1)*time.Millisecond)
			}
		})
	}
}

// Three producers pass the ticket pre-check, then the queue closes with nothing
// in it and its consumers leave, then all three take their tickets — on a
// two-slot ring, so the third ticket maps to the first one's slot. A ticket
// taken after the close must not wait for a slot no consumer will free again.
func TestTicketAfterCloseDoesNotWaitForASlot(t *testing.T) {
	r := NewRing[Event, struct{}](2, Options{Reserve: ReserveTicket})
	r.Close()
	var st OpStats
	if got := r.Pop(new(Event), time.Now().Add(time.Second), &st); got != PopDrained {
		t.Fatalf("an empty closed ring: Pop returned %d, want drained", got)
	}
	for i := 0; i < 3; i++ {
		done := make(chan Push)
		go func() { done <- r.pushTicket(MakeEvent(0, uint32(i), 0), &st) }()
		select {
		case got := <-done:
			if got != PushClosed {
				t.Fatalf("ticket %d after the close: %d, want closed", i, got)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("ticket %d after the close is still waiting for its slot", i)
		}
	}
}

// More producers than slots, all blocking with tickets, closed early and often.
func TestTicketsAfterCloseNeverStrand(t *testing.T) {
	for round := 0; round < 200; round++ {
		done := make(chan struct{})
		go func() {
			defer close(done)
			stress(t, NewRing[Event, struct{}](2, Options{Reserve: ReserveTicket}), 64, 2, 0, block,
				time.Duration(round%5)*100*time.Microsecond+50*time.Microsecond)
		}()
		select {
		case <-done:
		case <-time.After(10 * time.Second):
			t.Fatalf("round %d: a producer is still waiting for its slot 10 s after the close", round)
		}
	}
}

// A fill that panics inside the window still pays its slot, so the consumers
// behind it are not stopped.
func TestPanicInWindowPaysTombstone(t *testing.T) {
	r := NewRing[Event, LinePad](8, Options{})
	var st OpStats
	func() {
		defer func() {
			if recover() == nil {
				t.Fatal("the fill's panic did not reach the caller")
			}
		}()
		r.TryPushFunc(func(*Event) { panic("constructor failed") }, &st)
	}()
	for i := 0; i < 3; i++ {
		r.TryPush(MakeEvent(0, uint32(i), 0), &st)
	}
	for i := 0; i < 3; i++ {
		var e Event
		if got := r.TryPop(&e, &st); got != PopOK || e.Seq != uint32(i) {
			t.Fatalf("pop %d: %d, seq %d — the tombstone was not skipped", i, got, e.Seq)
		}
	}
	if r.Occupancy() != 0 {
		t.Errorf("occupancy %d after draining", r.Occupancy())
	}
}

type referent [1 << 10]byte

// A consumed slot must not keep its referent alive. The channel clears the slot
// it received from (typedmemclr in runtime.chanrecv); the ring has to do it by
// hand, and with Retain it does not.
func TestConsumedSlotReleasesReferent(t *testing.T) {
	alive := func(push func(*referent), pop func() *referent) int {
		ws := make([]weak.Pointer[referent], 32)
		for i := range ws {
			p := new(referent)
			ws[i] = weak.Make(p)
			push(p)
		}
		for range ws {
			if pop() == nil {
				t.Fatal("pop came back empty")
			}
		}
		runtime.GC()
		n := 0
		for _, w := range ws {
			if w.Value() != nil {
				n++
			}
		}
		return n
	}
	var st OpStats
	for _, retain := range []bool{false, true} {
		r := NewRing[*referent, struct{}](64, Options{Retain: retain})
		n := alive(func(p *referent) { r.TryPush(p, &st) },
			func() *referent { var p *referent; r.TryPop(&p, &st); return p })
		if want := map[bool]int{false: 0, true: 32}[retain]; n != want {
			t.Errorf("ring, retain=%v: %d of 32 referents alive after consumption, want %d", retain, n, want)
		}
		runtime.KeepAlive(r)
	}
	ch := make(chan *referent, 64)
	if n := alive(func(p *referent) { ch <- p }, func() *referent { return <-ch }); n != 0 {
		t.Errorf("channel: %d of 32 referents alive after consumption", n)
	}
	runtime.KeepAlive(ch)
}

// The control for a clean -race run: publishing before the payload write is a
// data race on the payload, and the detector must say so. Opt in, because a
// test that exists to fail cannot run by default:
//
//	TURNSTILE_SHOW_RACE=1 go test -race -run RaceDetectorSeesPublishFirst ./internal/turnstile/
func TestRaceDetectorSeesPublishFirst(t *testing.T) {
	if os.Getenv("TURNSTILE_SHOW_RACE") == "" {
		t.Skip("set TURNSTILE_SHOW_RACE=1, under -race, to watch the detector report the race")
	}
	stress(t, NewRing[Event, struct{}](4, Options{Bug: BugPublishFirst}), 2, 2, 2000, block, 0)
}
