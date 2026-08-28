// Package verify is the benchmark harness: assertions, percentiles, and the
// open-loop load generator. It mirrors cpp/bench/harness.{hpp,cpp}.
package verify

import (
	"fmt"
	"sort"
	"sync"
)

var (
	mu       sync.Mutex
	failures int
)

// Check records a failure and prints it. The process exit code is derived from
// Failures(), so an invariant that breaks fails the run, not just the eye of
// whoever is reading the output.
func Check(cond bool, what string) {
	if cond {
		return
	}
	mu.Lock()
	failures++
	mu.Unlock()
	fmt.Printf("  FAIL: %s\n", what)
}

func Failures() int {
	mu.Lock()
	defer mu.Unlock()
	return failures
}

func Reset() {
	mu.Lock()
	failures = 0
	mu.Unlock()
}

// Percentiles are in microseconds. Means are deliberately absent: the entire
// claim in this kata is about tails, and a mean averages away exactly the events
// under discussion.
type Percentiles struct {
	P50, P99, P999, P9999, Max float64
}

// ComputePercentiles sorts in place and returns microseconds. Samples are
// collected into a preallocated buffer and only sorted afterwards; nothing
// allocates or locks inside the measured path.
func ComputePercentiles(v []int64) Percentiles {
	if len(v) == 0 {
		return Percentiles{}
	}
	sort.Slice(v, func(i, j int) bool { return v[i] < v[j] })
	at := func(q float64) float64 {
		i := int(q * float64(len(v)-1))
		return float64(v[i]) / 1000
	}
	return Percentiles{
		P50:   at(0.50),
		P99:   at(0.99),
		P999:  at(0.999),
		P9999: at(0.9999),
		Max:   float64(v[len(v)-1]) / 1000,
	}
}

// MedianOf takes the median across repetitions per statistic, rather than
// picking one "median run". Choosing a whole run by its p99.9 lets one noisy
// percentile drag unrelated columns along with it.
func MedianOf[T any](reps []T, get func(T) float64) float64 {
	if len(reps) == 0 {
		return 0
	}
	v := make([]float64, 0, len(reps))
	for _, r := range reps {
		v = append(v, get(r))
	}
	sort.Float64s(v)
	return v[len(v)/2]
}
