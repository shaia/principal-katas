package bench

import (
	"context"
	"runtime"
	"sync/atomic"
	"testing"

	"turnstile/internal/turnstile"
)

// What single channel operations cost, and what a producer pays to learn that
// shutdown has begun: go test -run '^$' -bench . -count 3 ./internal/bench/.

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

func BenchmarkChannel(b *testing.B) {
	e := turnstile.MakeEvent(0, 0, 0)
	room := make(chan ev, 1024)
	full := make(chan ev, 1)
	full <- e
	empty := make(chan ev, 1)
	done := make(chan struct{})

	b.Run("send-receive", func(b *testing.B) {
		for b.Loop() {
			room <- e
			<-room
		}
	})
	b.Run("select-send-done-receive", func(b *testing.B) {
		for b.Loop() {
			select {
			case room <- e:
			case <-done:
			}
			<-room
		}
	})
	b.Run("nonblocking-send-full", func(b *testing.B) {
		for b.Loop() {
			select {
			case full <- e:
			default:
			}
		}
	})
	b.Run("nonblocking-send-full/parallel", func(b *testing.B) {
		parallel(b, func() {
			select {
			case full <- e:
			default:
			}
		})
	})
	b.Run("nonblocking-receive-empty", func(b *testing.B) {
		for b.Loop() {
			select {
			case <-empty:
			default:
			}
		}
	})
}

// Each check runs while shutdown has not begun, which is the case on every push
// but the last.
func BenchmarkShutdownCheck(b *testing.B) {
	var flag atomic.Bool
	done := make(chan struct{})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	ctxDone := ctx.Done()
	var stopped int
	for _, c := range []struct {
		name string
		op   func()
	}{
		{"atomic-bool", func() {
			if flag.Load() {
				stopped++
			}
		}},
		{"select-done-default", func() {
			select {
			case <-done:
				stopped++
			default:
			}
		}},
		{"ctx-err", func() {
			if ctx.Err() != nil {
				stopped++
			}
		}},
		{"select-ctx-done-default", func() {
			select {
			case <-ctx.Done():
				stopped++
			default:
			}
		}},
		{"select-ctx-done-hoisted", func() {
			select {
			case <-ctxDone:
				stopped++
			default:
			}
		}},
	} {
		b.Run(c.name, func(b *testing.B) {
			for b.Loop() {
				c.op()
			}
		})
		b.Run(c.name+"/parallel", func(b *testing.B) { parallel(b, c.op) })
	}
	if stopped != 0 {
		b.Fatal("a check reported shutdown while none had begun")
	}
}
