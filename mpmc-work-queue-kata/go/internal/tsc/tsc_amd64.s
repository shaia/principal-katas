//go:build amd64

#include "textflag.h"

// func Ticks() uint64
//
// Plain RDTSC: not ordered against the loads and stores around it, so a reading
// can drift a few tens of cycles from program order. That is far below what
// this kata measures, and the serialising forms cost several times more.
TEXT ·Ticks(SB), NOSPLIT|NOFRAME, $0-8
	RDTSC
	SHLQ $32, DX
	ORQ  DX, AX
	MOVQ AX, ret+0(FP)
	RET

// func Pause()
//
// The spin-wait hint. Go has no spelling for it outside the runtime, which uses
// it in procyield; a spin loop in user code needs its own.
TEXT ·Pause(SB), NOSPLIT|NOFRAME, $0-0
	PAUSE
	RET

// func invariant() bool
//
// CPUID.80000007H:EDX[8]: the counter ticks at a constant rate whatever the
// core's clock and sleep state, so one calibration holds for the whole run. The
// maximum extended leaf is checked first, because an unsupported leaf returns
// the highest supported one's data rather than an error.
TEXT ·invariant(SB), NOSPLIT, $0-1
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
