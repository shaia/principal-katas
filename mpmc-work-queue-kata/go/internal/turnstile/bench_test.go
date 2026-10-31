package turnstile

import (
	"runtime"
	"sync"
	"testing"
)

// The single-goroutine and all-goroutines-at-once costs, as ordinary Go
// benchmarks: go test -run '^$' -bench . -count 3 ./internal/turnstile/.

// parallel runs op on one goroutine per P and reports, beside Go's ns/op (wall
// time over every goroutine's operations), what one operation costs the
// goroutine making it while all the others make theirs: ns/op/goroutine.
func parallel(b *testing.B, op func()) {
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			op()
		}
	})
	b.ReportMetric(float64(b.Elapsed().Nanoseconds())*float64(runtime.GOMAXPROCS(0))/float64(b.N), "ns/op/goroutine")
}

// A push and a pop, one goroutine, nothing contended.
func BenchmarkPushPop(b *testing.B) {
	q := New[LineEvent](1024)
	p, c := q.Producer(), q.Consumer()
	var e LineEvent
	for b.Loop() {
		p.TryPush(e)
		c.TryPop()
	}
}

// The same with the value built in place, under the defer that pays the slot
// if the build panics.
func BenchmarkPushFuncPop(b *testing.B) {
	q := New[LineEvent](1024)
	p, c := q.Producer(), q.Consumer()
	var e LineEvent
	for b.Loop() {
		p.TryPushFunc(func(v *LineEvent) { *v = e })
		c.TryPop()
	}
}

// A push's wake decision with nobody asleep — which at the brief's rate is
// nearly every push — for the ring's bitmap and for one shared token behind a
// count.
func BenchmarkWakeDecision(b *testing.B) {
	for _, c := range []struct {
		name   string
		shared bool
	}{{"bitmap", false}, {"shared", true}} {
		var w waiter
		w.init(c.shared)
		b.Run(c.name, func(b *testing.B) {
			for b.Loop() {
				w.notify()
			}
		})
		b.Run(c.name+"/parallel", func(b *testing.B) { parallel(b, func() { w.notify() }) })
	}
}

// The same decision made without a count in front of the token: a send on
// every push. Where the token rests between pushes decides the cost. Left in a
// full channel, each send fails at the lock-free full check; taken every time,
// each send succeeds and takes the lock — modelled by a channel that is never
// full, made with room for every send the benchmark will make (its elements
// have no size, so the room costs no memory). sync.Cond is the third choice,
// and needs its mutex to signal.
func BenchmarkWakeWithoutACount(b *testing.B) {
	resting := make(chan struct{}, 1)
	resting <- struct{}{}
	restingSend := func() {
		select {
		case resting <- struct{}{}:
		default:
		}
	}
	var mu sync.Mutex
	cond := sync.NewCond(&mu)
	signal := func() { mu.Lock(); cond.Signal(); mu.Unlock() }

	b.Run("token-resting", func(b *testing.B) {
		for b.Loop() {
			restingSend()
		}
	})
	b.Run("token-resting/parallel", func(b *testing.B) { parallel(b, restingSend) })
	b.Run("token-never-resting", func(b *testing.B) {
		// The classic loop, not b.Loop: the channel is sized from b.N, which
		// b.Loop does not fix in advance.
		never := make(chan struct{}, b.N)
		for range b.N {
			never <- struct{}{}
		}
	})
	b.Run("token-never-resting/parallel", func(b *testing.B) {
		never := make(chan struct{}, b.N)
		parallel(b, func() { never <- struct{}{} })
	})
	b.Run("cond-signal", func(b *testing.B) {
		for b.Loop() {
			signal()
		}
	})
	b.Run("cond-signal/parallel", func(b *testing.B) { parallel(b, signal) })
}
