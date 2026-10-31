//go:build amd64

package tsc

// Bodies in tsc_amd64.s.

// Ticks reads the timestamp counter.
func Ticks() uint64

// Pause executes one PAUSE instruction.
func Pause()

func invariant() bool

// Native reports whether Ticks is a hardware counter.
const Native = true
