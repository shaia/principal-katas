package phases

import (
	"fmt"
	"sync"

	"weir/internal/verify"
	"weir/internal/weir"
)

// Throughput is phase 2: offered vs sustained across producer counts, and what
// the notification mechanism costs a producer.
//
// The ns/push columns are the ones that carry the argument. In C++ they are
// 18-29 ns and essentially flat from 4 to 32 producers, and that flatness is the
// entire point of the design: adding producers adds independent rings, so
// per-producer cost does not care how many others exist. A shared tail would
// show the opposite curve.
//
// The Go prediction, stated before the numbers: the two columns should be much
// closer together than in C++. There, the bitmap costs ~15 ns/push because
// signal() opens with an explicit seq_cst fence. Here that fence does not exist
// — the publish store is already a lock-prefixed XCHGQ providing the same
// StoreLoad edge — so the bitmap costs one extra MOVQ load and a
// predicted-not-taken branch. The mechanism should be *relatively cheaper in Go
// than in C++*, having been paid for in advance on every push.
func Throughput(short bool) {
	fmt.Println()
	fmt.Println("== 2. throughput ==")

	perProducer := 400_000
	counts := []int{1, 4, 16, 32}
	if short {
		perProducer = 40_000
		counts = []int{1, 4, 8}
	}

	clock := weir.NewClock()

	// Warm-up, excluded: the first run pays for cold caches, first-touch page
	// faults and CPU frequency ramp.
	runThroughput(clock, weir.Bitmap, 4, 50_000)

	fmt.Println("  producers   |   offered  accepted  sustained |   ns/push   ns/push")
	fmt.Println("              |       M/s         %        M/s | no-signal   +bitmap")
	for _, n := range counts {
		noSignal := runThroughput(clock, weir.FullScan, n, perProducer)
		withBitmap := runThroughput(clock, weir.Bitmap, n, perProducer)

		fmt.Printf("  %-11d | %9.1f %8.1f%% %10.1f | %9.1f %9.1f\n",
			n, withBitmap.offeredRate, withBitmap.acceptedPct, withBitmap.sustainedRate,
			noSignal.nsPerPush, withBitmap.nsPerPush)

		verify.Check(withBitmap.accepted+withBitmap.dropped == uint64(n*perProducer),
			fmt.Sprintf("throughput/%d: every push accounted for", n))
	}
	fmt.Println()
	fmt.Println("  ns/push is the producer's cost per event with the consumer running. The")
	fmt.Println("  number to watch is whether it stays flat as producers are added: that")
	fmt.Println("  flatness is the design's whole claim. A shared tail climbs instead.")
}

func runThroughput(clock *weir.Clock, scan weir.ScanPolicy, producers, perProducer int) abResult {
	cfg := weir.DefaultConfig()
	cfg.Scan = scan
	p := weir.New(cfg, clock)
	p.Start(weir.NewCountingSink())

	start := clock.Ticks()
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
				h.Push(weir.Event{ProducerID: h.ID(), Seq: uint32(s)})
			}
		}()
	}
	wg.Wait()
	pushNs := clock.SinceNanos(start)
	p.Stop(weir.Drain)
	totalNs := clock.SinceNanos(start)

	st := p.Stats()
	offered := uint64(producers * perProducer)
	return abResult{
		offeredRate:   float64(offered) / (float64(pushNs) / 1e9) / 1e6,
		acceptedPct:   100 * float64(st.Pushed) / float64(offered),
		sustainedRate: float64(st.Consumed) / (float64(totalNs) / 1e9) / 1e6,
		nsPerPush:     float64(pushNs) * float64(producers) / float64(offered),
		accepted:      st.Pushed,
		dropped:       st.Dropped,
		consumed:      st.Consumed,
	}
}
