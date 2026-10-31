//go:build !windows

package platform

import (
	"errors"
	"syscall"
)

func SetPowerThrottling(bool) error { return nil }

func PinThread(int) error { return errors.ErrUnsupported }

func ProcessCPU() float64 {
	var ru syscall.Rusage
	if syscall.Getrusage(syscall.RUSAGE_SELF, &ru) != nil {
		return 0
	}
	sec := func(t syscall.Timeval) float64 { return float64(t.Sec) + float64(t.Usec)/1e6 }
	return sec(ru.Utime) + sec(ru.Stime)
}
