//go:build amd64 && !purego

#include "textflag.h"

// func rdtsc() uint64
//
// Plain RDTSC, not RDTSCP or LFENCE;RDTSC. It is not ordered against
// surrounding loads, so a read can drift by a few tens of cycles relative to
// program order. At the microsecond scale this benchmark measures that is
// irrelevant, and the serialising variants cost 3-5x more — which matters,
// because the C++ writeup already worries about a 25 ns clock read becoming a
// meaningful share of the consumer's budget.
TEXT ·rdtsc(SB), NOSPLIT|NOFRAME, $0-8
	RDTSC
	SHLQ $32, DX
	ORQ  DX, AX
	MOVQ AX, ret+0(FP)
	RET

// func pause()
//
// The x86 PAUSE instruction. Hints "this is a spin-wait loop": reduces power,
// yields the pipeline to a hyperthread sibling, and avoids the memory-order
// violation pipeline flush on loop exit. Go exposes no equivalent, so the only
// way to get it is here.
TEXT ·pause(SB), NOSPLIT|NOFRAME, $0-0
	PAUSE
	RET

// func hasInvariantTSC() bool
//
// CPUID.80000007H:EDX[8] — "TSC Invariant": the TSC runs at a constant rate
// regardless of core frequency and C-state transitions. Present on every CPU
// since Nehalem, and a precondition for Windows using the TSC as its QPC
// source. Without it, calibrating once and extrapolating is wrong, and we must
// refuse to report sub-microsecond percentiles.
//
// Guarded by a max-extended-leaf check: CPUID.80000000H must report at least
// 0x80000007, or leaf 7 returns whatever the highest supported leaf returns
// rather than an error.
TEXT ·hasInvariantTSC(SB), NOSPLIT, $0-1
	MOVL $0x80000000, AX
	XORL CX, CX
	CPUID
	CMPL AX, $0x80000007
	JB   no

	MOVL $0x80000007, AX
	XORL CX, CX
	CPUID
	BTL  $8, DX
	JNC  no

	MOVB $1, ret+0(FP)
	RET

no:
	MOVB $0, ret+0(FP)
	RET
