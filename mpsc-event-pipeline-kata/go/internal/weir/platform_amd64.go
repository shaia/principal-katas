//go:build amd64 && !purego

package weir

// Declarations for platform_amd64.s. No bodies: the assembler supplies them.

//go:noescape
func rdtsc() uint64

//go:noescape
func pause()

//go:noescape
func hasInvariantTSC() bool

// nativeTicks reports whether ticks() is a real hardware counter rather than a
// fallback onto time.Now(). Phase 0 prints this, and the percentile reporting
// refuses sub-microsecond claims when it is false.
const nativeTicks = true

func ticks() uint64 { return rdtsc() }
