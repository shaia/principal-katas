//go:build windows

// Package platform is the little the benchmark needs from the OS that Go's
// standard library does not offer.
package platform

import (
	"syscall"
	"unsafe"
)

var (
	kernel32              = syscall.NewLazyDLL("kernel32.dll")
	setProcessInformation = kernel32.NewProc("SetProcessInformation")
	setThreadAffinityMask = kernel32.NewProc("SetThreadAffinityMask")
	getCurrentThread      = kernel32.NewProc("GetCurrentThread")
	getProcessTimes       = kernel32.NewProc("GetProcessTimes")
)

// SetPowerThrottling opts the process into or out of EcoQoS. Windows throttles
// a console process that is not in the foreground — threads steered to the
// efficiency cores at lower clocks — which on this laptop cut a saturated
// multi-threaded run to a quarter of its speed while single-threaded numbers
// did not move. A benchmark has to say it wants full speed.
func SetPowerThrottling(throttled bool) error {
	state := struct{ Version, ControlMask, StateMask uint32 }{Version: 1, ControlMask: 1}
	if throttled {
		state.StateMask = 1
	}
	const processPowerThrottling = 4
	p, _ := syscall.GetCurrentProcess()
	if r, _, err := setProcessInformation.Call(uintptr(p), processPowerThrottling,
		uintptr(unsafe.Pointer(&state)), unsafe.Sizeof(state)); r == 0 {
		return err
	}
	return nil
}

// PinThread restricts the calling OS thread to one logical CPU. The caller
// must have locked its goroutine to the thread and must exit without unlocking
// it, so that the runtime retires the pinned thread instead of reusing it.
func PinThread(cpu int) error {
	h, _, _ := getCurrentThread.Call()
	if r, _, err := setThreadAffinityMask.Call(h, uintptr(1)<<cpu); r == 0 {
		return err
	}
	return nil
}

// ProcessCPU is the CPU time this process has used, user and kernel, in seconds.
func ProcessCPU() float64 {
	var create, exit, kernel, user syscall.Filetime
	p, _ := syscall.GetCurrentProcess()
	getProcessTimes.Call(uintptr(p), uintptr(unsafe.Pointer(&create)), uintptr(unsafe.Pointer(&exit)),
		uintptr(unsafe.Pointer(&kernel)), uintptr(unsafe.Pointer(&user)))
	ticks := func(f syscall.Filetime) float64 { return float64(uint64(f.HighDateTime)<<32 | uint64(f.LowDateTime)) }
	return (ticks(kernel) + ticks(user)) * 100e-9
}
