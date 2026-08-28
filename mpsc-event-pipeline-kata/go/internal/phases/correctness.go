package phases

import (
	"fmt"
	"sync"
	"sync/atomic"

	"weir/internal/verify"
	"weir/internal/weir"
)

// Correctness is phase 1: the same four invariants the C++ asserts, under both
// scan policies, with the same sentence-shaped labels.
//
// The ordering assertion allows gaps but only where a drop was counted. A
// producer whose push failed has a hole in its sequence by design; the claim is
// that events never arrive out of order, not that none are lost.
func Correctness(short bool) {
	fmt.Println("== 1. correctness under stress ==")

	const producers = 16
	perProducer := 200_000
	if short {
		perProducer = 20_000
	}

	for _, scan := range []weir.ScanPolicy{weir.FullScan, weir.Bitmap} {
		clock := weir.NewClock()
		cfg := weir.DefaultConfig()
		cfg.Scan = scan
		p := weir.New(cfg, clock)

		// The hook runs only on the consumer goroutine, so this needs no
		// synchronization: single writer by construction.
		lastSeq := make([]int64, weir.MaxProducers)
		for i := range lastSeq {
			lastSeq[i] = -1
		}
		var violations, received int64
		p.SetEventHook(func(e *weir.Event) {
			received++
			if int64(e.Seq) <= lastSeq[e.ProducerID] {
				violations++
			}
			lastSeq[e.ProducerID] = int64(e.Seq)
		})

		p.Start(weir.NewCountingSink())
		var wg sync.WaitGroup
		for i := 0; i < producers; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				h, ok := p.RegisterProducer()
				if !ok {
					return
				}
				defer h.Close()
				for s := 0; s < perProducer; s++ {
					h.Push(weir.Event{ProducerID: h.ID(), Seq: uint32(s), StampNs: clock.Nanos()})
				}
			}()
		}
		wg.Wait()
		p.Stop(weir.Drain)

		st := p.Stats()
		offered := uint64(producers * perProducer)
		name := scan.String()

		fmt.Printf("  [%s] pushed=%d dropped=%d received=%d order_violations=%d\n",
			name, st.Pushed, st.Dropped, received, violations)

		verify.Check(violations == 0, name+": per-producer order preserved")
		verify.Check(st.Pushed+st.Dropped == offered,
			name+": every push accounted for (accepted + dropped)")
		verify.Check(uint64(received) == st.Pushed,
			name+": drain shutdown lost nothing (received == pushed)")
		verify.Check(p.DrainCompleted(), name+": drain finished within deadline")
		verify.Check(st.HighWater <= weir.RingCapacity, name+": ring depth stayed bounded")
	}

	producerLifetime(short)
}

// producerLifetime is phase 1b: slots recycled through many waves, asserting a
// retired producer's events are never lost to slot reuse.
//
// The counter fold at reclaim is the subtle part, and it caused a real bug in
// the C++ version: per-slot counters are reset at registration, so without
// folding a departing producer's totals into lifetime counters first, a recycled
// slot erases its predecessor's history and the accounting silently stops
// adding up.
func producerLifetime(short bool) {
	waves, threads, per := 40, 8, 500
	if short {
		waves = 8
	}

	clock := weir.NewClock()
	p := weir.New(weir.DefaultConfig(), clock)
	var received atomic.Int64
	p.SetEventHook(func(*weir.Event) { received.Add(1) })
	p.Start(weir.NewCountingSink())

	for w := 0; w < waves; w++ {
		var wg sync.WaitGroup
		for i := 0; i < threads; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				h, ok := p.RegisterProducer()
				if !ok {
					return
				}
				for s := 0; s < per; s++ {
					h.Push(weir.Event{ProducerID: h.ID(), Seq: uint32(s)})
				}
				h.Close() // retire while the consumer may be mid-drain
			}()
		}
		wg.Wait()
	}
	p.Stop(weir.Drain)

	st := p.Stats()
	want := uint64(waves * threads * per)
	fmt.Printf("  churn: %d waves x %d producers, pushed=%d received=%d\n",
		waves, threads, st.Pushed, received.Load())
	verify.Check(st.Pushed+st.Dropped == want,
		"churn: accounting survived slot recycling")
	verify.Check(uint64(received.Load()) == st.Pushed,
		"churn: every event from a retired producer was drained")
}
