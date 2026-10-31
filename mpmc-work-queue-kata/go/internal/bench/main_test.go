package bench

import (
	"os"
	"testing"

	"turnstile/internal/platform"
	"turnstile/internal/tsc"
)

// The driver reads the calibrated counter, and the benchmarks run many
// goroutines, which Windows throttles in a process not in the foreground
// unless it says otherwise.
func TestMain(m *testing.M) {
	platform.SetPowerThrottling(false)
	Clk = tsc.Calibrate()
	os.Exit(m.Run())
}
