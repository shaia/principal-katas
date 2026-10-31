package turnstile

import (
	"context"
	"errors"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"weak"

	"turnstile/internal/platform"
)

// Benchmarks here run many goroutines at once, and Windows throttles a process
// that is not in the foreground unless it says otherwise.
func TestMain(m *testing.M) {
	platform.SetPowerThrottling(false)
	os.Exit(m.Run())
}

// mode is how stress's producers push.
type mode int

const (
	try    mode = iota // TryPush, yielding on ErrFull
	ticket             // Push
	cas                // PushContext
)

// stress pushes `events` from each producer, or pushes until refused as closed
// when closeAfter is set, and checks what the consumers took: every accepted
// event exactly once, intact, and in order per producer from each consumer's
// view.
func stress(t *testing.T, q *Ring[Event], producers, consumers, events int, m mode, closeAfter time.Duration) {
	t.Helper()
	accepted := make([]Tally, producers)
	consumed := make([]Tally, consumers)
	closedSeen := make([]bool, producers)
	var torn, disorder atomic.Uint64
	var pw, cw sync.WaitGroup
	pw.Add(producers)
	for i := range producers {
		go func() {
			defer pw.Done()
			p := q.Producer()
			for seq := 0; closeAfter > 0 || seq < events; seq++ {
				e := MakeEvent(uint32(i), uint32(seq), 0)
				var err error
				switch m {
				case try:
					for err = p.TryPush(e); err == ErrFull; err = p.TryPush(e) {
						runtime.Gosched()
					}
				case ticket:
					err = p.Push(e)
				case cas:
					err = p.PushContext(context.Background(), e)
				}
				if errors.Is(err, ErrClosed) {
					closedSeen[i] = true
					return
				}
				if err != nil {
					t.Errorf("producer %d: push %d: %v", i, seq, err)
					return
				}
				accepted[i].Add(uint32(i), uint32(seq))
			}
		}()
	}
	cw.Add(consumers)
	for i := range consumers {
		go func() {
			defer cw.Done()
			c := q.Consumer()
			last := make([]int64, producers)
			for j := range last {
				last[j] = -1
			}
			for {
				e, err := c.Pop()
				if errors.Is(err, ErrClosed) {
					return
				}
				if !Intact(&e) || int(e.Producer) >= producers {
					torn.Add(1)
					continue
				}
				if int64(e.Seq) <= last[e.Producer] {
					disorder.Add(1)
				}
				last[e.Producer] = int64(e.Seq)
				consumed[i].Add(e.Producer, e.Seq)
			}
		}()
	}
	if closeAfter > 0 {
		time.Sleep(closeAfter)
		q.Close()
		pw.Wait()
	} else {
		pw.Wait()
		q.Close()
	}
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
		for i, seen := range closedSeen {
			if !seen {
				t.Errorf("producer %d was never refused as closed", i)
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

var modes = []struct {
	name string
	m    mode
}{{"TryPush", try}, {"Push", ticket}, {"PushContext", cas}}

// Two and four slots: every few events a lap wraps, with slots still reserved.
func TestLapWrap(t *testing.T) {
	for _, n := range []int{2, 4} {
		for _, m := range modes {
			t.Run(m.name, func(t *testing.T) {
				stress(t, New[Event](n), 4, 4, events(20000), m.m, 0)
			})
		}
	}
}

func TestFullFigures(t *testing.T) {
	stress(t, New[Event](1<<10), 8, 8, events(50000), ticket, 0)
}

// With no consumer, exactly capacity events go in and none is overwritten.
func TestNeverOverwrites(t *testing.T) {
	q := New[Event](64)
	p := q.Producer()
	n := 0
	for p.TryPush(MakeEvent(0, uint32(n), 0)) == nil {
		n++
	}
	if n != 64 {
		t.Errorf("accepted %d into 64 slots", n)
	}
	if err := p.TryPush(Event{}); !errors.Is(err, ErrFull) {
		t.Errorf("a push into a full ring: %v, want ErrFull", err)
	}
}

// A close racing pushes strands nothing: every producer is refused, and every
// event accepted before the refusal is consumed.
func TestCloseVersusReserve(t *testing.T) {
	for _, m := range modes {
		t.Run(m.name, func(t *testing.T) {
			for round := range 20 {
				stress(t, New[Event](64), 8, 4, 0, m.m, time.Duration(round%4+1)*time.Millisecond)
			}
		})
	}
}

// Three producers pass the ticket pre-check, then the ring closes with nothing
// in it and its consumers leave, then all three take their tickets — on a
// two-slot ring, so the third ticket maps to the first one's slot. A ticket
// taken after the close must not wait for a slot no consumer will free again.
func TestTicketAfterCloseDoesNotWaitForASlot(t *testing.T) {
	q := New[Event](2)
	q.Close()
	if _, err := q.Consumer().Pop(); !errors.Is(err, ErrClosed) {
		t.Fatalf("an empty closed ring: Pop returned %v, want ErrClosed", err)
	}
	p := q.Producer()
	for i := range 3 {
		done := make(chan error)
		go func() { done <- p.pushTicket(MakeEvent(0, uint32(i), 0)) }()
		select {
		case err := <-done:
			if !errors.Is(err, ErrClosed) {
				t.Fatalf("ticket %d after the close: %v, want ErrClosed", i, err)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("ticket %d after the close is still waiting for its slot", i)
		}
	}
}

// More producers than slots, all blocking with tickets, closed early and often.
func TestTicketsAfterCloseNeverStrand(t *testing.T) {
	for round := range 200 {
		done := make(chan struct{})
		go func() {
			defer close(done)
			stress(t, New[Event](2), 64, 2, 0, ticket,
				time.Duration(round%5)*100*time.Microsecond+50*time.Microsecond)
		}()
		select {
		case <-done:
		case <-time.After(10 * time.Second):
			t.Fatalf("round %d: a producer is still waiting for its slot 10 s after the close", round)
		}
	}
}

func TestPushContextHonoursItsDeadline(t *testing.T) {
	q := New[Event](2)
	p := q.Producer()
	p.TryPush(Event{})
	p.TryPush(Event{})
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if err := p.PushContext(ctx, Event{}); !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("a push into a full ring with a deadline: %v, want DeadlineExceeded", err)
	}
}

func TestPopContextHonoursItsDeadline(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := New[Event](2).Consumer().PopContext(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Errorf("a pop from an empty ring with a deadline: %v, want DeadlineExceeded", err)
	}
}

// The limit is checked when the handle is made, not when it first sleeps.
func TestSixtyFifthConsumerIsRefused(t *testing.T) {
	q := New[Event](2)
	for range 64 {
		q.Consumer()
	}
	defer func() {
		if recover() == nil {
			t.Error("a 65th consumer was accepted")
		}
	}()
	q.Consumer()
}

// A fill that panics inside the window still pays its slot, so the consumers
// behind it are not stopped.
func TestPanicInWindowPaysTombstone(t *testing.T) {
	q := New[Event](8)
	p, c := q.Producer(), q.Consumer()
	func() {
		defer func() {
			if recover() == nil {
				t.Fatal("the fill's panic did not reach the caller")
			}
		}()
		p.TryPushFunc(func(*Event) { panic("constructor failed") })
	}()
	for i := range 3 {
		p.TryPush(MakeEvent(0, uint32(i), 0))
	}
	for i := range 3 {
		e, ok := c.TryPop()
		if !ok || e.Seq != uint32(i) {
			t.Fatalf("pop %d: %v, seq %d — the tombstone was not skipped", i, ok, e.Seq)
		}
	}
	if q.Len() != 0 {
		t.Errorf("Len %d after draining", q.Len())
	}
}

type referent [1 << 10]byte

// A consumed slot must not keep its referent alive. The channel clears the slot
// it received from (typedmemclr in runtime.chanrecv); the ring has to do it by
// hand, and with Variant.Retain it does not.
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
	for _, retain := range []bool{false, true} {
		q := NewVariant[*referent](64, Variant{Retain: retain})
		p, c := q.Producer(), q.Consumer()
		n := alive(func(r *referent) { p.TryPush(r) }, func() *referent { r, _ := c.TryPop(); return r })
		want := 0
		if retain {
			want = 32
		}
		if n != want {
			t.Errorf("ring, retain=%v: %d of 32 referents alive after consumption, want %d", retain, n, want)
		}
		runtime.KeepAlive(q)
	}
	ch := make(chan *referent, 64)
	if n := alive(func(r *referent) { ch <- r }, func() *referent { return <-ch }); n != 0 {
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
	stress(t, NewVariant[Event](4, Variant{Bug: BugPublishFirst}), 2, 2, 2000, ticket, 0)
}
