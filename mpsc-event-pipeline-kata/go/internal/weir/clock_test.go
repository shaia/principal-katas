package weir

import (
	"testing"
	"time"
)

// TestClockCalibration is the test that justifies clock.go existing. If
// time.Now() turns out to be fine-grained on some machine, the log output says
// so and the TSC path is merely redundant there; on Windows it is not.
func TestClockCalibration(t *testing.T) {
	c := NewClock()
	t.Logf("native=%v invariant=%v", c.Native(), c.Invariant())
	t.Logf("calibrated: %.4f ns/tick (%.3f GHz)", c.NanosPerTick(), c.HzApprox()/1e9)
	t.Logf("TSC read cost: %.2f ns", c.ReadCostNanos())
	t.Logf("time.Now() smallest observable tick: %v", MeasuredTimeNowResolution(200))

	if !c.Native() {
		t.Skip("no hardware tick counter on this platform; percentiles are clock-limited")
	}
	if !c.Invariant() {
		t.Error("TSC is not invariant: calibrating once and extrapolating is unsound here")
	}

	// Calibration accuracy against a known sleep.
	for _, d := range []time.Duration{50 * time.Millisecond, 200 * time.Millisecond} {
		t0 := c.Ticks()
		w0 := time.Now()
		time.Sleep(d)
		gotTSC := c.SinceNanos(t0)
		gotWall := time.Since(w0).Nanoseconds()
		errPct := (float64(gotTSC) - float64(gotWall)) / float64(gotWall) * 100
		t.Logf("sleep %-6v: tsc=%.3f ms wall=%.3f ms delta=%+.4f%%",
			d, float64(gotTSC)/1e6, float64(gotWall)/1e6, errPct)
		if errPct > 1 || errPct < -1 {
			t.Errorf("calibration off by %.3f%%, want within 1%%", errPct)
		}
	}

	// Resolution: the entire point. time.Now() manages ~6 distinct values in
	// 5 ms on Windows; the TSC should manage thousands in 1 ms.
	seen := map[uint64]struct{}{}
	end := c.Ticks() + uint64(1e6/c.NanosPerTick())
	for c.Ticks() < end {
		seen[c.Ticks()] = struct{}{}
	}
	t.Logf("distinct TSC values in 1 ms: %d", len(seen))
	if len(seen) < 1000 {
		t.Errorf("TSC resolution too coarse: %d distinct values in 1 ms", len(seen))
	}
}

// TestSpinUntilIsAccurate checks the pacer's primitive at the periods the
// benchmark actually uses. time.Sleep cannot do any of these on Windows.
func TestSpinUntilIsAccurate(t *testing.T) {
	c := NewClock()
	if !c.Native() {
		t.Skip("clock-limited platform")
	}
	for _, periodNs := range []int64{1_000, 10_000, 100_000} {
		var worst, total int64
		const reps = 200
		for i := 0; i < reps; i++ {
			start := c.Ticks()
			deadline := start + c.NanosToTicks(periodNs)
			SpinUntil(deadline)
			late := c.SinceNanos(start) - periodNs
			if late > worst {
				worst = late
			}
			total += late
		}
		t.Logf("period %6d ns: mean lateness %+5d ns, worst %+7d ns",
			periodNs, total/reps, worst)
		if total/reps > periodNs {
			t.Errorf("mean lateness %d ns exceeds the %d ns period", total/reps, periodNs)
		}
	}
}
