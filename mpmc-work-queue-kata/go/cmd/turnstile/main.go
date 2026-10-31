// Command turnstile runs the Go track's measurements and asserts its
// invariants. The exit code is the verdict: 0 when every check held, 1 when one
// failed or a deliberate bug was detected, 3 when a run hung.
package main

import (
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"turnstile/internal/bench"
	"turnstile/internal/platform"
	"turnstile/internal/tsc"
)

func main() {
	phases := flag.String("phase", "", "comma-separated phases to run (default: all)")
	bug := flag.String("bug", "", "run the bug stress for one deliberate bug: "+strings.Join(bench.BugNames(), ", "))
	child := flag.String("child", "", "internal: run one experiment as a child process")
	flag.Parse()

	if err := platform.SetPowerThrottling(false); err != nil {
		fmt.Printf("  note: could not opt out of power throttling: %v\n", err)
	}
	if v := os.Getenv("TURNSTILE_NS_PER_TICK"); v != "" {
		ns, err := strconv.ParseFloat(v, 64)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		bench.Clk = tsc.FromNsPerTick(ns)
	} else {
		bench.Clk = tsc.Calibrate()
	}

	if *child != "" {
		bench.ChildMain(*child)
		return
	}
	if *bug != "" {
		os.Exit(bench.RunBug(*bug))
	}

	want := map[int]bool{}
	if *phases != "" {
		for _, f := range strings.Split(*phases, ",") {
			n, err := strconv.Atoi(strings.TrimSpace(f))
			if err != nil {
				fmt.Fprintf(os.Stderr, "bad phase %q\n", f)
				os.Exit(2)
			}
			want[n] = true
		}
	}
	start := time.Now()
	for _, p := range bench.Phases {
		if len(want) > 0 && !want[p.N] {
			continue
		}
		fmt.Printf("\n== phase %d — %s ==\n", p.N, p.Title)
		t0 := time.Now()
		disarm := bench.Watchdog(fmt.Sprintf("phase %d", p.N), 5*time.Minute)
		p.Run()
		disarm()
		fmt.Printf("  (phase %d: %.1f s)\n", p.N, time.Since(t0).Seconds())
	}
	fmt.Printf("\n%d checks failed; %.0f s in all\n", bench.Failures(), time.Since(start).Seconds())
	if bench.Failures() > 0 {
		os.Exit(1)
	}
	fmt.Println("all checks passed")
}
