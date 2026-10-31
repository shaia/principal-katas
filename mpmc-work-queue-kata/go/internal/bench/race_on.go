//go:build race

package bench

// The race detector runs the same code an order of magnitude slower, and
// changes the scheduling it is run under, so a race build divides the
// correctness phases' event counts and reports no latency at all.
const (
	RaceEnabled = true
	scale       = 10
)
