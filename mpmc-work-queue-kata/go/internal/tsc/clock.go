// Package tsc is the instrument: the processor's timestamp counter, calibrated
// once against the wall clock.
//
// It exists because time.Now cannot measure this kata on Windows. Its monotonic
// reading is KUSER_SHARED_DATA.InterruptTime (runtime/time_windows_amd64.s),
// which advances only at the clock interrupt — every 15.6 ms by default, about
// once a millisecond while some process holds a finer timer period. The brief's
// budget is 2 µs. A p99 taken with time.Now would be a column of zeros with a
// few whole milliseconds in it, and it would look better than the truth.
package tsc

import (
	"math"
	"time"
)

type Clock struct {
	nsPerTick float64
	readNs    float64
}

// Calibrate measures the counter against the wall clock over 300 ms. Both ends
// spin until time.Now changes, so each is pinned to a clock edge rather than
// sampled somewhere inside a tick, and the coarse clock's quantization does not
// enter the ratio.
func Calibrate() *Clock {
	const window = 300 * time.Millisecond
	w0, c0 := edge()
	time.Sleep(window)
	w1, c1 := edge()
	c := &Clock{nsPerTick: float64(w1.Sub(w0).Nanoseconds()) / float64(c1-c0)}
	c.readNs = c.readCost()
	return c
}

// FromNsPerTick rebuilds a calibration measured elsewhere — by the parent of a
// child process, so each child does not spend 300 ms measuring the same
// invariant counter again.
func FromNsPerTick(ns float64) *Clock {
	c := &Clock{nsPerTick: ns}
	c.readNs = c.readCost()
	return c
}

func (c *Clock) NsPerTick() float64 { return c.nsPerTick }

// Agreement measures a second, independent window of d against the calibration
// and returns the relative error.
func (c *Clock) Agreement(d time.Duration) float64 {
	w0, c0 := edge()
	time.Sleep(d)
	w1, c1 := edge()
	wall := float64(w1.Sub(w0).Nanoseconds())
	return (c.Ns(c1-c0) - wall) / wall
}

// edge waits for time.Now to change and returns the new reading together with
// the counter at that moment.
func edge() (time.Time, uint64) {
	t0 := time.Now()
	for {
		t := time.Now()
		if t.After(t0) {
			return t, Ticks()
		}
	}
}

func (c *Clock) readCost() float64 {
	const n = 1 << 18
	var sink uint64
	start := Ticks()
	for i := 0; i < n; i++ {
		sink += Ticks()
	}
	end := Ticks()
	if sink == 0 {
		panic("unreachable: keeps the loop")
	}
	return c.Ns(end-start) / n
}

func (c *Clock) Ns(ticks uint64) float64 { return float64(ticks) * c.nsPerTick }

func (c *Clock) Ticks(ns float64) uint64 { return uint64(math.Round(ns / c.nsPerTick)) }

func (c *Clock) Duration(ticks uint64) time.Duration { return time.Duration(c.Ns(ticks)) }

func (c *Clock) GHz() float64 { return 1 / c.nsPerTick }

// ReadCost is what one Ticks call costs, the floor under every latency reported.
func (c *Clock) ReadCost() float64 { return c.readNs }

// Invariant reports whether the counter runs at a constant rate. Without it a
// single calibration would not hold, and phase 0 fails the run.
func Invariant() bool { return invariant() }

// NowTick is time.Now's smallest observable step: the shortest of `samples`
// waits for its reading to change.
func NowTick(samples int) time.Duration {
	best := time.Duration(math.MaxInt64)
	for i := 0; i < samples; i++ {
		t0 := time.Now()
		for {
			t := time.Now()
			if t.After(t0) {
				best = min(best, t.Sub(t0))
				break
			}
		}
	}
	return best
}

// NowDistinct counts the distinct readings time.Now returns while it is called
// in a tight loop for d, measured by the counter rather than by itself.
func (c *Clock) NowDistinct(d time.Duration) (calls, distinct int) {
	end := Ticks() + c.Ticks(float64(d.Nanoseconds()))
	var last time.Time
	for Ticks() < end {
		t := time.Now()
		calls++
		if !t.Equal(last) {
			distinct++
			last = t
		}
	}
	return calls, distinct
}

// Burn spins for the given number of ticks: the consumer's work per event.
func Burn(ticks uint64) {
	end := Ticks() + ticks
	for Ticks() < end {
	}
}
