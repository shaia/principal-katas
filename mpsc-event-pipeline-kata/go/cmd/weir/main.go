// Command weir runs the verification phases. The exit code is the verdict:
// zero if every invariant held, otherwise the failure count.
//
// This mirrors cpp/bin/solution. Invariants that are fast and want the race
// detector live in `go test ./...` instead — that split is deliberate and
// explained in go/solution.md section 10.
package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"

	"weir/internal/phases"
	"weir/internal/verify"
	"weir/internal/weir"
)

func main() {
	short := flag.Bool("short", false, "reduced event counts, for a quick check")
	only := flag.String("phase", "", "run one phase: env, correctness, throughput, backpressure, scanab, baseline")
	flag.Parse()

	// Go 1.25 updates GOMAXPROCS dynamically; pin it so it cannot change
	// mid-measurement.
	runtime.GOMAXPROCS(runtime.NumCPU())

	fmt.Printf("GOMAXPROCS=%d, %d producer slots x %d events x %d B = %d KiB of ring storage\n",
		runtime.GOMAXPROCS(0), weir.MaxProducers, weir.RingCapacity, weir.EventSize,
		weir.MaxProducers*weir.RingCapacity*weir.EventSize/1024)

	clock := weir.NewClock()

	run := func(name string) bool { return *only == "" || *only == name }

	if run("env") {
		phases.Environment(clock)
	}
	if run("correctness") {
		phases.Correctness(*short)
	}
	if run("throughput") {
		phases.Throughput(*short)
	}
	if run("backpressure") {
		phases.Backpressure(*short)
	}
	if run("scanab") {
		phases.ScanAB(*short)
	}
	if run("baseline") {
		phases.BaselineAB(*short)
	}

	n := verify.Failures()
	if n == 0 {
		fmt.Println("all checks passed")
		os.Exit(0)
	}
	fmt.Printf("%d checks FAILED\n", n)
	if n > 125 {
		n = 125
	}
	os.Exit(n)
}
