package weir

import (
	"time"
)

// Clock converts a raw hardware tick counter into nanoseconds.
//
// WHY THIS FILE EXISTS AT ALL, since it has no C++ counterpart:
//
// On Windows, Go's time.Now() does not read QueryPerformanceCounter. Its
// monotonic reading comes from KUSER_SHARED_DATA.InterruptTime, scaled by 100 ns
// (runtime/time_windows_amd64.s), so it advances only at the system clock
// interrupt — nominally every 15.625 ms, or ~1 ms if some process on the machine
// holds timeBeginPeriod(1). Measured on the development box: 237,201 calls to
// time.Now() across 5 ms produced *six* distinct values.
//
// The C++ answer's steady_clock is QPC, ~100 ns resolution, so its reported p50
// figures of 0.5-3.3 us are real measurements. A Go port that used time.Now()
// would report p50 = 0.0 us and a tail quantized to whole milliseconds — and
// would look faster than the C++, which is the worst kind of wrong: a flattering
// number with no bug to find.
//
// So: read the TSC directly, calibrate once, and make the calibration itself
// something phase 0 prints rather than something the reader has to trust.
type Clock struct {
	nanosPerTick float64
	origin       uint64
	readCostNs   float64
	invariant    bool
	native       bool
}

// NewClock calibrates against time.Now().
//
// time.Now() is coarse, which is the entire problem — but it is coarse in a way
// that cancels: both ends of the calibration window spin until the wall clock
// *ticks over*, so each endpoint is pinned to a clock edge rather than sampled
// somewhere inside a tick. The quantization error does not accumulate, it lands
// only in the sub-tick alignment of the two edges, and over a 300 ms window that
// is well under 0.1%. This needs no syscall and no build tags, which is worth
// more here than the last decimal place QPC would buy.
func NewClock() *Clock {
	const window = 300 * time.Millisecond

	// Start on a wall-clock edge.
	t0 := time.Now()
	var w0 time.Time
	for {
		w0 = time.Now()
		if w0.After(t0) {
			break
		}
	}
	c0 := ticks()

	// Wait out the window without burning a core: the endpoints are what must
	// be precise, not the middle.
	time.Sleep(window)

	// End on a wall-clock edge.
	t1 := time.Now()
	var w1 time.Time
	for {
		w1 = time.Now()
		if w1.After(t1) {
			break
		}
	}
	c1 := ticks()

	elapsedNs := float64(w1.Sub(w0).Nanoseconds())
	elapsedTicks := float64(c1 - c0)

	c := &Clock{
		nanosPerTick: elapsedNs / elapsedTicks,
		origin:       c1,
		invariant:    hasInvariantTSC(),
		native:       nativeTicks,
	}
	c.readCostNs = c.measureReadCost()
	return c
}

// measureReadCost is reported by phase 0 next to the latency tables. The C++
// writeup samples 1-in-4 because a clock read is ~25 ns and at multi-million
// events/s the instrument would otherwise become a meaningful share of the
// consumer's budget. Stating our own read cost is what makes that judgement
// checkable rather than inherited.
func (c *Clock) measureReadCost() float64 {
	const n = 200_000
	// Warm up, then measure. Sum the results so nothing can be optimized away.
	var sink uint64
	for i := 0; i < n/10; i++ {
		sink += ticks()
	}
	start := ticks()
	for i := 0; i < n; i++ {
		sink += ticks()
	}
	end := ticks()
	if sink == 0 { // never true; defeats dead-code elimination
		panic("unreachable")
	}
	return float64(end-start) * c.nanosPerTick / float64(n)
}

// Ticks reads the counter. On amd64 this is a single RDTSC.
func (c *Clock) Ticks() uint64 { return ticks() }

// Nanos returns nanoseconds since the clock was calibrated. This is the value
// that goes in Event.StampNs.
func (c *Clock) Nanos() int64 {
	return int64(float64(ticks()-c.origin) * c.nanosPerTick)
}

// TicksToNanos converts an absolute tick reading to the same nanosecond base as
// Nanos, so an *intended* send time computed in ticks can be stamped into an
// event and compared against arrival. Signed, because a tick before the origin
// is meaningful.
func (c *Clock) TicksToNanos(t uint64) int64 {
	return int64(float64(t-c.origin) * c.nanosPerTick)
}

// NanosToTicks is the inverse, used by the pacer to compute a spin deadline
// without converting on every poll.
func (c *Clock) NanosToTicks(ns int64) uint64 {
	return uint64(float64(ns) / c.nanosPerTick)
}

// SinceNanos is the elapsed nanoseconds since a tick reading.
func (c *Clock) SinceNanos(t0 uint64) int64 {
	return int64(float64(ticks()-t0) * c.nanosPerTick)
}

func (c *Clock) HzApprox() float64      { return 1e9 / c.nanosPerTick }
func (c *Clock) NanosPerTick() float64  { return c.nanosPerTick }
func (c *Clock) ReadCostNanos() float64 { return c.readCostNs }
func (c *Clock) Invariant() bool        { return c.invariant }

// Native reports whether Ticks() is a hardware counter. When false, every
// percentile below the wall clock's resolution is fiction and phase 0 says so.
func (c *Clock) Native() bool { return c.native }

// SpinUntil busy-waits until the counter reaches deadline.
//
// The pacer must spin rather than sleep. Measured on Windows, time.Sleep of
// 1 us, 10 us and 100 us all return in ~510 us; there is no way to ask for a
// shorter wait. And it must spin rather than Gosched: a yield deschedules the
// producer past its own deadline and injects exactly the jitter an open-loop
// pacer exists to expose.
func SpinUntil(deadline uint64) {
	for ticks() < deadline {
		pause()
	}
}

// MeasuredTimeNowResolution spins until time.Now() changes and reports the
// smallest delta it observed. Phase 0 prints this beside the TSC resolution,
// because the gap between them is the whole justification for this file.
func MeasuredTimeNowResolution(samples int) time.Duration {
	best := time.Duration(1<<63 - 1)
	for i := 0; i < samples; i++ {
		t0 := time.Now()
		for {
			t1 := time.Now()
			if t1.After(t0) {
				if d := t1.Sub(t0); d < best {
					best = d
				}
				break
			}
		}
	}
	return best
}
