// Package bench is the measuring side: the driver every phase is built from,
// the phases themselves, and the plumbing they share.
package bench

import (
	"fmt"
	"os"
	"runtime"
	"slices"
	"time"

	"turnstile/internal/platform"
	"turnstile/internal/tsc"
)

// Clk is the calibrated counter, set by Init before any phase runs.
var Clk *tsc.Clock

var failures int

// Check records a failure and prints it. The exit code is derived from
// Failures, so an invariant that breaks fails the run, not only the eye of
// whoever reads the output.
func Check(ok bool, format string, args ...any) {
	if !ok {
		fmt.Printf("  FAIL: "+format+"\n", args...)
		failures++
	}
}

func Failures() int { return failures }

// Median is per statistic: each column's median across repetitions, rather
// than one "median run" chosen by a single column.
func Median[T any](reps []T, get func(T) float64) float64 {
	v := make([]float64, len(reps))
	for i, r := range reps {
		v[i] = get(r)
	}
	slices.Sort(v)
	return v[len(v)/2]
}

// Ns formats a duration in nanoseconds to three significant figures.
func Ns(ns float64) string {
	switch {
	case ns < 1e3:
		return fmt.Sprintf("%.0f ns", ns)
	case ns < 1e4:
		return fmt.Sprintf("%.2f us", ns/1e3)
	case ns < 1e5:
		return fmt.Sprintf("%.1f us", ns/1e3)
	case ns < 1e6:
		return fmt.Sprintf("%.0f us", ns/1e3)
	case ns < 1e7:
		return fmt.Sprintf("%.2f ms", ns/1e6)
	case ns < 1e8:
		return fmt.Sprintf("%.1f ms", ns/1e6)
	case ns < 1e9:
		return fmt.Sprintf("%.0f ms", ns/1e6)
	}
	return fmt.Sprintf("%.2f s", ns/1e9)
}

func Rate(perSec float64) string { return fmt.Sprintf("%.2f M/s", perSec/1e6) }

// Q is a histogram quantile in nanoseconds.
func Q(h *Histogram, q float64) float64 { return Clk.Ns(h.Quantile(q)) }

// Watchdog ends the process if what it guards has not finished in time: a
// shutdown that deadlocks, or a deliberately broken queue that livelocks, must
// fail loudly rather than leave the run hanging with no verdict. The returned
// function disarms it.
func Watchdog(what string, limit time.Duration) func() { return watchdogExit(what, limit, 3) }

var exit = os.Exit

// Thermometer prints what one PAUSE costs right now on CPU 0. The instruction
// takes a fixed number of cycles, so its cost tracks the core's clock, which
// sustained load on this laptop pulls down. Pinned, because unpinned it reads
// whichever kind of core the goroutine landed on.
func Thermometer() {
	const n = 1 << 14
	done := make(chan float64)
	go func() {
		// Never unlocked: the runtime retires the pinned thread when this exits.
		runtime.LockOSThread()
		pinned := platform.PinThread(0) == nil
		t0 := tsc.Ticks()
		for i := 0; i < n; i++ {
			tsc.Pause()
		}
		ns := Clk.Ns(tsc.Ticks()-t0) / n
		if !pinned {
			ns = -ns
		}
		done <- ns
	}()
	ns := <-done
	where := "CPU 0"
	if ns < 0 {
		ns, where = -ns, "an unpinned thread"
	}
	fmt.Printf("  (clock check: one PAUSE costs %.1f ns on %s)\n", ns, where)
}
