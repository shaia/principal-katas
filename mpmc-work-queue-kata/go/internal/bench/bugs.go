package bench

import (
	"fmt"
	"os"
	"runtime/pprof"
	"slices"
	"time"

	"turnstile/internal/turnstile"
)

var bugs = map[string]turnstile.Bug{
	"none":          turnstile.BugNone,
	"publish-first": turnstile.BugPublishFirst,
	"no-full-check": turnstile.BugNoFullCheck,
	"late-announce": turnstile.BugLateAnnounce,
}

func BugNames() []string {
	var v []string
	for k := range bugs {
		v = append(v, k)
	}
	slices.Sort(v)
	return v
}

// RunBug is one row of the bug × tool table: the stress for one deliberate bug
// and, for the bugs that live in the sleep, the engineered race. It returns the
// exit code — 1 when anything detected the bug, a hang included. Built with
// -race, the detector adds its own verdict: exit code 66 when it reported.
func RunBug(name string) int {
	b, ok := bugs[name]
	if !ok {
		fmt.Printf("unknown bug %q; one of %v\n", name, BugNames())
		return 2
	}
	defer watchdogExit("the stress", 30*time.Second, 1)()

	s := Spec{Producers: 8, Consumers: 8, Events: 25_000 / scale}
	r := Run(turnstile.NewRing[ev, struct{}](64, turnstile.Options{Bug: b}), s)
	detected := !r.ExactlyOnce || r.Torn > 0 || r.Disorder > 0
	fmt.Printf("  %d x %d on a 64-slot ring, %d events each: accepted %d, consumed %d, torn %d, out of order %d\n",
		s.Producers, s.Consumers, s.Events, r.Accepted, r.Consumed, r.Torn, r.Disorder)

	if b == turnstile.BugNone || b == turnstile.BugLateAnnounce {
		lost := 0
		for _, spread := range []int{64, 256, 1024, 4096} {
			lost += wakeRace(b, 20_000/scale, spread).lost
		}
		fmt.Printf("  the engineered race, 4 x %d iterations: %d wakeups lost\n", 20_000/scale, lost)
		detected = detected || lost > 0
	}
	if detected {
		fmt.Println("detected")
		return 1
	}
	fmt.Println("not detected")
	return 0
}

// watchdogExit is Watchdog with the exit code a hang should report. Before it
// exits it writes every goroutine's stack to stderr, so a hang says where.
func watchdogExit(what string, limit time.Duration, code int) func() {
	done := make(chan struct{})
	go func() {
		select {
		case <-done:
		case <-time.After(limit):
			fmt.Printf("  FAIL: %s did not finish within %v — hung\n", what, limit)
			if code != 1 { // a deliberate bug's hang is its verdict, not a mystery
				pprof.Lookup("goroutine").WriteTo(os.Stderr, 2)
			}
			exit(code)
		}
	}()
	return func() { close(done) }
}
