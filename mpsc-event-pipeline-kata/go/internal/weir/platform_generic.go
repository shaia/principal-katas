//go:build !amd64 || purego

package weir

import (
	"runtime"
	"time"
)

// Fallbacks for non-amd64 or -tags purego.
//
// ticks() degrades to time.Now(), which on Windows reads
// KUSER_SHARED_DATA.InterruptTime and advances roughly a thousand times a
// second — three orders of magnitude coarser than the latencies this design
// operates at. Phase 0 prints a banner when nativeTicks is false, and the
// percentile report is suppressed rather than shown misleadingly precise.
const nativeTicks = false

func ticks() uint64 { return uint64(time.Now().UnixNano()) }

// No PAUSE available. Gosched is not the same thing — it deschedules rather
// than hinting — but an empty loop body would be worse.
func pause() { runtime.Gosched() }

func hasInvariantTSC() bool { return false }
