package weir

import "sync/atomic"

// ProducerHandle is a ticket for one producer slot.
//
// NOT SAFE FOR CONCURRENT USE. Exactly one goroutine may call Push on a given
// handle — that is the S in SPSC. This is a much easier rule to break in Go than
// in C++, because "spawn a goroutine per unit of work" is the default idiom and
// nothing in the type system objects. The saving grace is that breaking it is a
// plain-memory data race on the ring's cachedHead, so `go test -race` reports
// it, where in C++ the same mistake is silent.
//
// NO RAII, AND THAT IS A REAL LOSS.
//
// The C++ version is move-only RAII: the destructor retires the slot, so
// retirement cannot be forgotten, and a pipeline dropped without an explicit
// stop is still safe. Go has neither destructors nor scope-bound cleanup, so
// this is Close() plus defer, and forgetting it leaks a slot out of 64.
//
// runtime.AddCleanup and runtime.SetFinalizer are NOT the substitute, for four
// separate reasons:
//
//  1. Timing. They run "some time after ptr is no longer reachable", on another
//     goroutine, with no ordering guarantee and no guarantee they run before
//     exit. The Free->Active->Retiring->Free protocol exists precisely to make
//     the handoff deterministic; a slot released eventually, against a budget of
//     64, is a resource leak with a latency attached — and RegisterProducer
//     failing because a dead producer's slot has not been reaped yet is a nastier
//     failure than forgetting Close.
//  2. A C++ destructor is a *scope* guarantee. A Go cleanup is a *reachability*
//     hint. Substituting one for the other is a category error.
//  3. AddCleanup never runs if ptr is reachable from the cleanup or its
//     argument, so the natural spelling — a closure over the handle — silently
//     never fires.
//  4. It moves retirement onto an arbitrary goroutine, adding a third
//     participant to a protocol whose entire value is having exactly two.
//
// Cleanups are the right tool for *diagnosing* the leak, though, which is what
// leakedHandles below is for.
type ProducerHandle struct {
	p      *Pipeline
	id     uint32
	closed atomic.Bool
}

func (h *ProducerHandle) ID() uint32 { return h.id }

func (h *ProducerHandle) Valid() bool { return h != nil && h.p != nil }

// Push copies e into this producer's ring. Returns false if the event was
// dropped — by policy when the ring is full, or because the pipeline has stopped
// accepting. A false return is always counted; loss is never silent.
func (h *ProducerHandle) Push(e Event) bool {
	if h == nil || h.p == nil {
		return false
	}
	return h.p.push(h.id, &e)
}

// Close retires the slot. Idempotent, so a defer plus an explicit call is fine.
//
// The slot is not reusable until the consumer has drained the ring and published
// Free — this only marks the intent.
func (h *ProducerHandle) Close() {
	if h == nil || h.p == nil {
		return
	}
	if h.closed.CompareAndSwap(false, true) {
		h.p.retire(h.id)
	}
}
