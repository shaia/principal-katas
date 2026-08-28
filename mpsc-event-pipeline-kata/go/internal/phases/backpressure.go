package phases

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"weir/internal/verify"
	"weir/internal/weir"
)

// Backpressure is phase 3: overload against a deliberately slow sink, and
// shutdown.
//
// "Producers must not block indefinitely" and "bounded memory usage" are the
// same requirement seen from two sides, and together they force the answer: the
// queue is bounded, so overload must have an explicit policy, and every policy
// must terminate. An unbounded queue is not a solution to overload, it is a
// deferral of it — it converts a latency problem into a memory problem, and the
// memory problem arrives later, larger, and as an OOM kill.
//
// The two numbers that matter are high_water (proving memory stayed bounded)
// and worst_push (proving no producer blocked indefinitely). The drop percentage
// is the system behaving as designed, not failing.
func Backpressure(short bool) {
	fmt.Println()
	fmt.Println("== 3. backpressure and shutdown ==")

	const producers = 8
	perProducer := 100_000
	if short {
		perProducer = 20_000
	}

	clock := weir.NewClock()
	p := weir.New(weir.DefaultConfig(), clock)
	p.Start(weir.NewSlowSink(clock, 400_000)) // 400 us per batch

	var worstPush atomic.Int64
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
				t0 := clock.Ticks()
				h.Push(weir.Event{ProducerID: h.ID(), Seq: uint32(s)})
				if d := clock.SinceNanos(t0); d > worstPush.Load() {
					worstPush.Store(d)
				}
			}
		}()
	}
	wg.Wait()

	st := p.Stats()
	offered := uint64(producers * perProducer)
	fmt.Printf("  slow sink: pushed=%d dropped=%d (%.1f%%) high_water=%d worst_push=%.1f us\n",
		st.Pushed, st.Dropped, 100*float64(st.Dropped)/float64(offered),
		st.HighWater, float64(worstPush.Load())/1000)

	verify.Check(st.Dropped > 0,
		"backpressure: drops occurred rather than producers blocking")
	verify.Check(st.Pushed+st.Dropped == offered,
		"backpressure: every push accounted for")
	verify.Check(st.HighWater <= weir.RingCapacity,
		"backpressure: memory stayed bounded")
	verify.Check(worstPush.Load() < 100_000_000,
		"backpressure: no producer blocked indefinitely")

	// Abort must be prompt even with a wedged sink.
	start := time.Now()
	p.Stop(weir.Abort)
	elapsed := time.Since(start)
	fmt.Printf("  abort shutdown returned in %.1f ms\n", float64(elapsed.Microseconds())/1000)
	verify.Check(elapsed < 2*time.Second, "backpressure: abort returned promptly")
}
