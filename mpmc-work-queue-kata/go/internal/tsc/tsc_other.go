//go:build !amd64

package tsc

import "time"

// Elsewhere the counter is time.Now's monotonic reading, which is honest where
// that clock is fine-grained. Phase 0 prints Native beside its resolution.

var base = time.Now()

func Ticks() uint64 { return uint64(time.Since(base)) }

func Pause() {}

func invariant() bool { return true }

const Native = false
