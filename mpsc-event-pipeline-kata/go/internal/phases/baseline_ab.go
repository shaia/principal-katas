package phases

import (
	"fmt"
	"sync"

	"weir/internal/verify"
	"weir/internal/weir"
)

// BaselineAB is the Go-specific phase with no C++ counterpart: per-producer SPSC
// rings against the two designs section 1 rejects — one shared buffered channel,
// and an explicit mutex plus ring.
//
// This matters more in Go than the equivalent would in C++, because in Go the
// rejected design is the *idiomatic* one. `ch := make(chan Event, 8192)` is what
// every Go engineer writes first, and it is a runtime.hchan: a mutex plus a
// circular buffer, with every field on the same two cache lines.
//
// THE MEASUREMENT TRAP THIS PHASE EXISTS TO AVOID:
//
// The rings drop under overload by policy. A channel with a non-blocking send
// also drops. But a *blocking* channel send, or a benchmark that only counts
// pushes returning true, silently compares "events the ring accepted" against
// "events the channel was forced to accept" — and the ring wins enormously by
// counting its drops as work. A prototype of this benchmark reported the rings
// at 775 M/s and the channel at 3.8 M/s at 16 producers, and a large part of
// that gap was drops being counted as pushes.
//
// So every row reports offered, accepted and sustained separately, and the
// sustained column — events the consumer actually got through the sink — is the
// only one worth comparing.
func BaselineAB(short bool) {
	fmt.Println()
	fmt.Println("== 5. idiomatic baseline: rings vs chan vs mutex ==")
	fmt.Println()
	fmt.Println("  In Go the rejected design is the idiomatic one: a buffered chan is an")
	fmt.Println("  hchan — a mutex plus a circular buffer — so this is the comparison")
	fmt.Println("  section 9 of the C++ answer asks for, in a language where the simple")
	fmt.Println("  answer is also the one you would reach for by default.")
	fmt.Println()
	fmt.Println("  'accepted' is the share of offered events the transport took; 'sustained'")
	fmt.Println("  is what the consumer actually pushed through the sink. Comparing anything")
	fmt.Println("  but sustained lets a design that drops freely look fast.")
	fmt.Println()

	perProducer := 400_000
	counts := []int{1, 4, 8, 16, 32}
	if short {
		perProducer = 40_000
		counts = []int{1, 4, 8}
	}

	clock := weir.NewClock()

	fmt.Println("  producers  transport |    offered   accepted   sustained |   ns/push")
	fmt.Println("                       |        M/s          %         M/s |")
	for _, n := range counts {
		for _, name := range []string{"rings", "chan/producer", "chan", "mutex"} {
			r := runTransport(clock, name, n, perProducer)
			fmt.Printf("  %-9d  %-9s | %10.1f %9.1f%% %11.1f | %9.1f\n",
				n, name, r.offeredRate, r.acceptedPct, r.sustainedRate, r.nsPerPush)

			verify.Check(r.accepted+r.dropped == uint64(n*perProducer),
				fmt.Sprintf("baseline %s/%d: every push accounted for", name, n))
			verify.Check(r.consumed <= r.accepted,
				fmt.Sprintf("baseline %s/%d: consumed never exceeds accepted", name, n))
		}
	}
	fmt.Println()
}

type abResult struct {
	offeredRate, acceptedPct, sustainedRate, nsPerPush float64
	accepted, dropped, consumed                        uint64
}

func runTransport(clock *weir.Clock, name string, producers, perProducer int) abResult {
	var t weir.Transport
	switch name {
	case "rings":
		cfg := weir.DefaultConfig()
		t = weir.New(cfg, clock)
	case "chan/producer":
		// The idiomatic Go design: one buffered channel per producer, so every
		// hchan.lock has exactly one sender and one receiver and is therefore
		// uncontended by construction. Same memory as the rings.
		t = weir.NewChannelPipeline(weir.NewCountingSink(),
			weir.WithProducers(weir.MaxProducers), weir.WithCapacity(weir.RingCapacity)).AsTransport()
	case "chan":
		// Memory-equivalent to the rings: 64 slots x 4096 events. Giving the
		// channel less would be comparing a smaller buffer, not a different
		// design.
		t = weir.NewChanTransport(weir.MaxProducers * weir.RingCapacity)
	case "mutex":
		t = weir.NewMutexTransport(weir.MaxProducers * weir.RingCapacity)
	}

	sink := weir.NewCountingSink()
	t.Start(sink)

	start := clock.Ticks()
	var wg sync.WaitGroup
	for i := 0; i < producers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			h, ok := t.Register()
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
	t.Stop(weir.Drain)
	totalNs := clock.SinceNanos(start)

	st := t.Stats()
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
