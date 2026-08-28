package weir

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func testConfig(scan ScanPolicy) Config {
	c := DefaultConfig()
	c.Scan = scan
	return c
}

func scale(t *testing.T, full int) int {
	if testing.Short() {
		return full / 10
	}
	return full
}

// TestCorrectnessUnderStress is phase 1. It asserts the four invariants the C++
// version asserts, under both scan policies, with the same labels.
//
// The ordering check allows gaps, but only where a drop was counted: a producer
// whose push failed has a hole in its sequence by design, and the point of the
// assertion is that events never arrive *out of order*, not that none are lost.
func TestCorrectnessUnderStress(t *testing.T) {
	for _, scan := range []ScanPolicy{FullScan, Bitmap} {
		t.Run(scan.String(), func(t *testing.T) {
			const producers = 16
			perProducer := scale(t, 200_000)

			clock := NewClock()
			p := New(testConfig(scan), clock)

			// The hook runs only on the consumer goroutine, so lastSeq needs no
			// synchronization: single writer by construction.
			lastSeq := make([]int64, MaxProducers)
			for i := range lastSeq {
				lastSeq[i] = -1
			}
			var violations, received int64
			p.SetEventHook(func(e *Event) {
				received++
				if int64(e.Seq) <= lastSeq[e.ProducerID] {
					violations++
				}
				lastSeq[e.ProducerID] = int64(e.Seq)
			})

			sink := NewCountingSink()
			p.Start(sink)

			var wg sync.WaitGroup
			for i := 0; i < producers; i++ {
				wg.Add(1)
				go func() {
					defer wg.Done()
					h, ok := p.RegisterProducer()
					if !ok {
						t.Error("registry full")
						return
					}
					defer h.Close()
					for s := 0; s < perProducer; s++ {
						h.Push(Event{ProducerID: h.ID(), Seq: uint32(s),
							StampNs: clock.Nanos()})
					}
				}()
			}
			wg.Wait()
			p.Stop(Drain)

			st := p.Stats()
			offered := uint64(producers * perProducer)

			if violations != 0 {
				t.Errorf("%s: per-producer order preserved: %d violations",
					scan, violations)
			}
			if st.Pushed+st.Dropped != offered {
				t.Errorf("%s: every push accounted for: pushed=%d dropped=%d sum=%d want %d",
					scan, st.Pushed, st.Dropped, st.Pushed+st.Dropped, offered)
			}
			if uint64(received) != st.Pushed {
				t.Errorf("%s: drain shutdown lost nothing: received=%d pushed=%d",
					scan, received, st.Pushed)
			}
			if !p.DrainCompleted() {
				t.Errorf("%s: drain finished within deadline", scan)
			}
			if st.HighWater > RingCapacity {
				t.Errorf("%s: ring depth stayed bounded: high_water=%d cap=%d",
					scan, st.HighWater, RingCapacity)
			}
			t.Logf("%s: pushed=%d dropped=%d received=%d bitmap_writes=%d parks=%d",
				scan, st.Pushed, st.Dropped, received, st.BitmapWrites, st.Parks)
		})
	}
}

// TestProducerLifetime is phase 1b: slots recycled across many waves, asserting
// a retired producer's events are never lost to slot reuse, and that the
// counter fold at reclaim does not erase a predecessor's history.
//
// This is the primary -race target. It is where the Retiring->Free handoff, the
// counter fold, and reuse of a ring's private cached indices by a *different*
// goroutine all collide.
func TestProducerLifetime(t *testing.T) {
	waves := scale(t, 40)
	if waves < 4 {
		waves = 4
	}
	const threads, per = 8, 500

	clock := NewClock()
	p := New(testConfig(Bitmap), clock)

	var received atomic.Int64
	p.SetEventHook(func(*Event) { received.Add(1) })
	p.Start(NewCountingSink())

	for w := 0; w < waves; w++ {
		var wg sync.WaitGroup
		for i := 0; i < threads; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				h, ok := p.RegisterProducer()
				if !ok {
					t.Error("registry full during churn")
					return
				}
				for s := 0; s < per; s++ {
					h.Push(Event{ProducerID: h.ID(), Seq: uint32(s)})
				}
				h.Close() // retire while the consumer may be mid-drain
			}()
		}
		wg.Wait()
	}
	p.Stop(Drain)

	st := p.Stats()
	want := uint64(waves * threads * per)
	if st.Pushed+st.Dropped != want {
		t.Errorf("churn: accounting survived slot recycling: pushed=%d dropped=%d want sum %d",
			st.Pushed, st.Dropped, want)
	}
	if uint64(received.Load()) != st.Pushed {
		t.Errorf("churn: retired producers' events all drained: received=%d pushed=%d",
			received.Load(), st.Pushed)
	}
	t.Logf("churn: %d waves x %d producers, pushed=%d received=%d",
		waves, threads, st.Pushed, received.Load())
}

// TestWakeupNeverStrands is the missed-wakeup stress: a producer that pushes one
// event into an otherwise idle pipeline, over and over, so the park/clear/signal
// path is exercised at exactly the transition boundary.
//
// This is the test for the bug class the Dekker handshake exists to prevent. In
// C++ it is a real hazard requiring an explicit seq_cst fence; here the claim is
// that Go's sequentially consistent atomics make it unwritable. A claim like
// that deserves a test that would fail if it were false.
func TestWakeupNeverStrands(t *testing.T) {
	rounds := scale(t, 3000)

	clock := NewClock()
	cfg := testConfig(Bitmap)
	cfg.SpinBeforeYield = 50 // park aggressively: we want the transition
	cfg.YieldBeforePark = 50
	p := New(cfg, clock)

	var received atomic.Int64
	p.SetEventHook(func(*Event) { received.Add(1) })
	p.Start(NewCountingSink())

	h, ok := p.RegisterProducer()
	if !ok {
		t.Fatal("registry full")
	}
	worst := time.Duration(0)
	for i := 0; i < rounds; i++ {
		// Wait until the consumer has actually gone to sleep before pushing.
		//
		// Without this the test is worthless: the producer pushes the next event
		// while the consumer is still spinning, the park path never runs, and
		// the race this test exists for is never attempted. A first version of
		// this test reported parks=0 and passed, which is exactly the kind of
		// green tick that means nothing.
		parksBefore := p.Stats().Parks
		deadline := time.Now().Add(2 * time.Second)
		for p.Stats().Parks == parksBefore {
			if time.Now().After(deadline) {
				t.Fatal("consumer never parked; the wakeup path is untested")
			}
			time.Sleep(50 * time.Microsecond)
		}

		start := time.Now()
		if !h.Push(Event{ProducerID: h.ID(), Seq: uint32(i)}) {
			t.Fatalf("push %d rejected on an idle pipeline", i)
		}
		want := int64(i + 1)
		for received.Load() < want {
			if d := time.Since(start); d > 2*time.Second {
				t.Fatalf("event %d stranded: published with the consumer asleep "+
					"and never drained after %v — this is the missed-wakeup bug", i, d)
			}
		}
		if d := time.Since(start); d > worst {
			worst = d
		}
	}
	h.Close()
	p.Stop(Drain)

	st := p.Stats()
	t.Logf("wakeup: %d rounds each crossing a park boundary, worst arrival %v, "+
		"parks=%d notifies=%d bitmap_writes=%d", rounds, worst, st.Parks,
		st.Notifies, st.BitmapWrites)
	if received.Load() != int64(rounds) {
		t.Errorf("received %d, want %d", received.Load(), rounds)
	}
	// Assert the park path was genuinely exercised, but not that *every* round
	// parked. Under -race (a 5-20x slowdown) or on a loaded machine the consumer
	// can still be spinning when the next push lands, and a strict
	// parks >= rounds turns a correct run into a red test. What matters is that
	// the transition happened many times and no event was stranded across it.
	if st.Parks < uint64(rounds)/2 {
		t.Errorf("only %d parks across %d rounds: the park/wake transition was "+
			"not exercised often enough for this test to mean anything",
			st.Parks, rounds)
	}
	// Every round is one park->wake cycle. Bits are cleared only on the way to
	// sleep, so writes should be bounded by a small multiple of the park count,
	// never by the event count.
	if st.BitmapWrites > 4*st.Parks+16 {
		t.Errorf("bitmap writes %d against %d parks: clearing is churning",
			st.BitmapWrites, st.Parks)
	}
}

// TestBitmapCoalescingHolds is the assertion the C++ version did not have and
// wished it did. Its first implementation cleared bits eagerly and produced
// 582,954 writes to the shared line where the fix produces one per producer. No
// test failed; the mechanism was quietly doing the opposite of its job.
//
// Go closes the ordering trap in this design but not this one — eager clearing
// is a design bug about write frequency, and no language feature prevents it.
// So assert it.
func TestBitmapCoalescingHolds(t *testing.T) {
	const producers = 8
	perProducer := scale(t, 100_000)

	clock := NewClock()
	p := New(testConfig(Bitmap), clock)
	p.Start(NewCountingSink())

	var wg sync.WaitGroup
	for i := 0; i < producers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			h, _ := p.RegisterProducer()
			defer h.Close()
			for s := 0; s < perProducer; s++ {
				h.Push(Event{ProducerID: h.ID(), Seq: uint32(s)})
			}
		}()
	}
	wg.Wait()
	p.Stop(Drain)

	st := p.Stats()
	events := uint64(producers * perProducer)
	// A streaming producer should set its bit once and never touch the line
	// again. Allow generous headroom for idle transitions, but nothing remotely
	// proportional to throughput.
	limit := events / 1000
	if limit < 200 {
		limit = 200
	}
	t.Logf("coalescing: %d events, %d bitmap writes (%.6f per event), %d parks",
		events, st.BitmapWrites, float64(st.BitmapWrites)/float64(events), st.Parks)
	if st.BitmapWrites > limit {
		t.Errorf("bitmap writes %d exceed %d: coalescing is not holding, and the "+
			"mechanism is reintroducing the contention it exists to remove",
			st.BitmapWrites, limit)
	}
}

func TestShutdownModes(t *testing.T) {
	t.Run("abort-is-prompt", func(t *testing.T) {
		clock := NewClock()
		p := New(testConfig(Bitmap), clock)
		p.Start(NewCountingSink())

		stop := make(chan struct{})
		var wg sync.WaitGroup
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				h, _ := p.RegisterProducer()
				defer h.Close()
				for s := 0; ; s++ {
					select {
					case <-stop:
						return
					default:
					}
					h.Push(Event{ProducerID: h.ID(), Seq: uint32(s)})
				}
			}()
		}
		time.Sleep(50 * time.Millisecond)
		start := time.Now()
		p.Stop(Abort)
		elapsed := time.Since(start)
		close(stop)
		wg.Wait()

		t.Logf("abort shutdown returned in %v", elapsed)
		if elapsed > time.Second {
			t.Errorf("abort took %v, want well under a second", elapsed)
		}
	})

	t.Run("stop-is-idempotent", func(t *testing.T) {
		clock := NewClock()
		p := New(testConfig(Bitmap), clock)
		p.Start(NewCountingSink())
		p.Stop(Drain)
		p.Stop(Drain) // must not panic, hang, or double-close
		p.Stop(Abort)
	})

	t.Run("drain-loses-nothing", func(t *testing.T) {
		clock := NewClock()
		p := New(testConfig(Bitmap), clock)
		var received atomic.Int64
		p.SetEventHook(func(*Event) { received.Add(1) })
		p.Start(NewCountingSink())

		h, _ := p.RegisterProducer()
		const n = 5000
		sent := 0
		for i := 0; i < n; i++ {
			if h.Push(Event{ProducerID: h.ID(), Seq: uint32(i)}) {
				sent++
			}
		}
		h.Close()
		p.Stop(Drain)

		if int(received.Load()) != sent {
			t.Errorf("drain lost events: received=%d pushed=%d", received.Load(), sent)
		}
		if !p.DrainCompleted() {
			t.Error("drain did not complete within its deadline")
		}
	})
}

// TestBackpressureIsBoundedAndCounted is phase 3: against a deliberately slow
// sink, drops must occur rather than blocking, memory must stay bounded, and no
// producer may block indefinitely.
func TestBackpressureIsBoundedAndCounted(t *testing.T) {
	const producers = 8
	perProducer := scale(t, 100_000)

	clock := NewClock()
	p := New(testConfig(Bitmap), clock)
	p.Start(NewSlowSink(clock, 400_000)) // 400 us per batch

	var worstPush atomic.Int64
	var wg sync.WaitGroup
	for i := 0; i < producers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			h, _ := p.RegisterProducer()
			defer h.Close()
			for s := 0; s < perProducer; s++ {
				t0 := clock.Ticks()
				h.Push(Event{ProducerID: h.ID(), Seq: uint32(s)})
				if d := clock.SinceNanos(t0); d > worstPush.Load() {
					worstPush.Store(d)
				}
			}
		}()
	}
	wg.Wait()
	p.Stop(Abort)

	st := p.Stats()
	offered := uint64(producers * perProducer)
	t.Logf("slow sink: pushed=%d dropped=%d (%.1f%%) high_water=%d worst_push=%.1f us",
		st.Pushed, st.Dropped, 100*float64(st.Dropped)/float64(offered),
		st.HighWater, float64(worstPush.Load())/1000)

	if st.Dropped == 0 {
		t.Error("no drops against a slow sink: producers must have blocked")
	}
	if st.Pushed+st.Dropped != offered {
		t.Errorf("accounting: pushed+dropped=%d want %d", st.Pushed+st.Dropped, offered)
	}
	if st.HighWater > RingCapacity {
		t.Errorf("memory not bounded: high_water=%d cap=%d", st.HighWater, RingCapacity)
	}
	// "Must not block indefinitely" — a bound in the tens of milliseconds is
	// dominated by OS preemption, not by the queue.
	if worstPush.Load() > 100_000_000 {
		t.Errorf("worst push took %v: a producer blocked indefinitely",
			time.Duration(worstPush.Load()))
	}
}
