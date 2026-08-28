package weir

import (
	"os"
	"sync"
	"testing"
)

// TestSPSCContractViolationIsDetected deliberately breaks the single-producer
// rule so you can see what -race catches. It is env-gated because it is
// *supposed* to fail under -race.
//
//	WEIR_SHOW_RACE=1 CGO_ENABLED=1 CC=gcc go test -race -run ContractViolation ./internal/weir/
//
// Why this exists: a clean -race run only means something if the tool is
// actually engaged on this code. This is the control. It is also the concrete
// demonstration of the Go port's headline claim — the C++ version documents the
// SPSC contract in a comment and cannot check it, because ThreadSanitizer is
// unavailable for x86_64-pc-windows-msvc. Here the contract is enforced.
//
// The race is on cachedHead, which is a plain uint64 written by whichever
// goroutine calls TryPush. Two producers on one handle is a much easier mistake
// to make in Go than in C++, because "spawn a goroutine per unit of work" is the
// default idiom and nothing in the type system objects.
func TestSPSCContractViolationIsDetected(t *testing.T) {
	if os.Getenv("WEIR_SHOW_RACE") == "" {
		t.Skip("set WEIR_SHOW_RACE=1 to demonstrate what -race catches here")
	}
	r := new(SpscRing)
	var wg sync.WaitGroup
	for g := 0; g < 2; g++ { // two producers on one ring: the violation
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 50_000; i++ {
				e := Event{ProducerID: uint32(g), Seq: uint32(i)}
				r.TryPush(&e)
			}
		}(g)
	}
	out := make([]Event, DrainBatch)
	for i := 0; i < 50_000; i++ {
		r.PopBatch(out)
	}
	wg.Wait()
}
