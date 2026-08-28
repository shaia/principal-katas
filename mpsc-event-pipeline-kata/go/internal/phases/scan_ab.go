package phases

import (
	"fmt"
	"sync"

	"weir/internal/verify"
	"weir/internal/weir"
)

// ScanAB is phase 4: full scan vs active bitmap.
//
// The follow-up asks how you would *benchmark* the optimization, which is the
// right question — and the answer has to survive the result not being what you
// hoped. The C++ version's honest conclusion was that the bitmap is a
// median-latency and CPU-efficiency win, not a tail-latency one, and that
// p99/p99.9 do not separate the policies at all on that hardware.
//
// The methodology, carried over unchanged because it is the most transferable
// part of the C++ answer:
//
//   - A/B in one binary. ScanPolicy is a runtime switch, so both policies run
//     the same code, same build, same machine, alternating.
//   - Isolate one variable. Active producers are pinned at 8 (the premise) and
//     *registration* is swept 8 -> 64. Sweeping the active count instead would
//     change CPU contention and scan width simultaneously and confound them.
//   - Open-loop load at a fixed rate, latency measured from the *intended* send
//     time. Non-negotiable: a closed loop lets a stalled consumer throttle its
//     own producers, so the queue never backs up and the stall never appears.
//     Coordinated omission is the single most common way a latency benchmark
//     lies, and it always lies in the flattering direction.
//   - Percentiles, never means.
//   - The measurement must not perturb the measured: samples go to a
//     preallocated buffer with no allocation or locking on the path.
//   - Repetitions with per-statistic medians.
//   - Report mechanism counters beside latency, so a tail that moves for an
//     unrelated reason cannot be credited to this change.
//
// One deliberate divergence from the C++, which fixes a confound in it: the C++
// increments rings_probed_ with an atomic fetch_add *per probe*. That is a lock
// xadd, ~18-20 cycles, instrumenting a probe that costs ~4 cycles (an L1 hit on
// an index line nobody is writing). The counter is roughly four times the cost
// of the thing it measures and it scales with registration, so part of that
// writeup's measured full-scan degradation is the instrument. Here the consumer
// accumulates into plain local fields and publishes once per pass.
func ScanAB(short bool) {
	fmt.Println()
	fmt.Println("== 4. follow-up: full scan vs active bitmap ==")
	fmt.Println()
	fmt.Println("  Held fixed: 8 active producers at 100000 events/s each. Varying: how")
	fmt.Println("  many idle producers are also registered, i.e. how many empty rings the")
	fmt.Println("  full-scan consumer must probe on every pass.")
	fmt.Println()
	fmt.Println("  Prediction, before the numbers: probes/pass tracks registration for full")
	fmt.Println("  scan and stays flat for the bitmap, so the mechanism demonstrably works.")
	fmt.Println("  Whether that reaches the tail is a different question, and the honest")
	fmt.Println("  answer may well be no — probing an idle ring is an L1 hit, not a")
	fmt.Println("  coherence miss, and 64 L1 hits do not move a p99.9 that has OS")
	fmt.Println("  preemption in it.")
	fmt.Println()

	const active = 8
	ratePerSec := 100_000
	durationMs := int64(400)
	reps := 5
	registrations := []int{8, 16, 32, 64}
	if short {
		durationMs = 120
		reps = 3
		registrations = []int{8, 64}
	}

	clock := weir.NewClock()

	fmt.Println("  registered scan   |  p50 us   p99 us    p99.9 | probes/pass   bmp-wr  drop%")
	for _, reg := range registrations {
		for _, scan := range []weir.ScanPolicy{weir.FullScan, weir.Bitmap} {
			var runs []scanRun
			for r := 0; r < reps; r++ {
				runs = append(runs, runScan(clock, scan, active, reg, ratePerSec, durationMs))
			}
			p50 := verify.MedianOf(runs, func(r scanRun) float64 { return r.pct.P50 })
			p99 := verify.MedianOf(runs, func(r scanRun) float64 { return r.pct.P99 })
			p999 := verify.MedianOf(runs, func(r scanRun) float64 { return r.pct.P999 })
			probes := verify.MedianOf(runs, func(r scanRun) float64 { return r.probesPerPass })
			bmp := verify.MedianOf(runs, func(r scanRun) float64 { return float64(r.bitmapWrites) })
			drop := verify.MedianOf(runs, func(r scanRun) float64 { return r.dropPct })

			fmt.Printf("  %-10d %-6s | %7.1f %8.1f %8.1f | %11.1f %8.0f %5.1f%%\n",
				reg, scan.String(), p50, p99, p999, probes, bmp, drop)

			if scan == weir.Bitmap {
				verify.Check(probes < float64(reg)*0.75+2,
					fmt.Sprintf("scan-ab/%d: bitmap probes fewer rings than registration", reg))
				verify.Check(bmp < 1000,
					fmt.Sprintf("scan-ab/%d: bitmap writes stayed coalesced", reg))
			} else {
				verify.Check(probes > float64(reg)*0.75,
					fmt.Sprintf("scan-ab/%d: full scan probes every registered ring", reg))
			}
		}
	}
	fmt.Println()
	fmt.Println("  'probes/pass' is rings inspected per consumer pass — the scan width this")
	fmt.Println("  optimization exists to shrink. 'bmp-wr' counts writes to the shared")
	fmt.Println("  bitmap line over the whole run: coalescing is working only if it stays")
	fmt.Println("  negligible next to the event count. A tail that moves while probes/pass")
	fmt.Println("  stays flat means the win came from somewhere else.")
}

type scanRun struct {
	pct           verify.Percentiles
	probesPerPass float64
	bitmapWrites  uint64
	dropPct       float64
}

func runScan(clock *weir.Clock, scan weir.ScanPolicy, active, registered, ratePerSec int, durationMs int64) scanRun {
	cfg := weir.DefaultConfig()
	cfg.Scan = scan
	p := weir.New(cfg, clock)

	// Latency is measured on the consumer, from the event's *intended* send
	// time, into a preallocated buffer. Sampling 1-in-4 because a clock read is
	// not free and at these rates the instrument would otherwise become a
	// meaningful share of the consumer's budget.
	const sampleEvery = 4
	expected := int(int64(active) * int64(ratePerSec) * durationMs / 1000 / sampleEvery)
	samples := make([]int64, 0, expected+1024)
	seen := 0
	p.SetEventHook(func(e *weir.Event) {
		seen++
		if seen%sampleEvery != 0 || len(samples) == cap(samples) {
			return
		}
		samples = append(samples, clock.Nanos()-e.StampNs)
	})

	p.Start(weir.NewCountingSink())

	// Register the idle producers first so they occupy slots and must be
	// probed, but never push.
	var idle []*weir.ProducerHandle
	for i := active; i < registered; i++ {
		if h, ok := p.RegisterProducer(); ok {
			idle = append(idle, h)
		}
	}

	periodTicks := clock.NanosToTicks(int64(1e9) / int64(ratePerSec))
	totalEvents := int(int64(ratePerSec) * durationMs / 1000)

	var wg sync.WaitGroup
	for i := 0; i < active; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			h, ok := p.RegisterProducer()
			if !ok {
				return
			}
			defer h.Close()

			// Open loop: the schedule is fixed from a single origin. The
			// intended time is origin + n*period, never now + period — a late
			// send must not push the schedule back, because that is exactly
			// coordinated omission.
			origin := clock.Ticks()
			for s := 0; s < totalEvents; s++ {
				intended := origin + uint64(s)*periodTicks
				weir.SpinUntil(intended)
				h.Push(weir.Event{
					ProducerID: h.ID(),
					Seq:        uint32(s),
					StampNs:    clock.TicksToNanos(intended), // intended, not actual
				})
			}
		}()
	}
	wg.Wait()
	p.Stop(weir.Drain)
	for _, h := range idle {
		h.Close()
	}

	st := p.Stats()
	offered := float64(active * totalEvents)
	probesPerPass := 0.0
	if st.Passes > 0 {
		probesPerPass = float64(st.RingsProbed) / float64(st.Passes)
	}
	return scanRun{
		pct:           verify.ComputePercentiles(samples),
		probesPerPass: probesPerPass,
		bitmapWrites:  st.BitmapWrites,
		dropPct:       100 * float64(st.Dropped) / offered,
	}
}
