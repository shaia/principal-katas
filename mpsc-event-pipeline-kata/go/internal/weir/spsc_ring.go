package weir

import "sync/atomic"

// SpscRing is a bounded single-producer / single-consumer ring buffer.
//
// This is where the memory ordering lives — and the Go story differs from the
// C++ one in one decisive way: Go does not have acquire/release. Every
// sync/atomic operation is sequentially consistent (go.dev/ref/mem: "All the
// atomic operations executed in a program behave as though executed in some
// sequentially consistent order"). There is no weaker ordering to ask for.
//
// On amd64 that asymmetry is stark, and it cuts both ways:
//
//	tail.Load()   ->  MOVQ                 free, exactly like the C++ acquire load
//	tail.Store()  ->  XCHGQ AX, (CX)       lock-prefixed, ~20 cycles, drains the
//	                                       store buffer — where C++'s release
//	                                       store is a plain MOV costing nothing
//
// So the publish path — the single hottest operation in the entire design, once
// per event — is *more expensive in Go than in C++*, and there is no way around
// it in the language. Even the runtime's own internal StoreRel64 is `JMP
// Store64` on amd64: the cheap release store does not exist to be borrowed.
//
// What that buys is in bitmap.go, and it is not nothing: the explicit
// atomic_thread_fence(seq_cst) that the C++ wakeup handshake requires — the
// subtlest line in that design, the one whose misplacement was a real shipped
// bug — disappears entirely, because this store already provides the StoreLoad
// edge it existed to create. Go removes the choice, charges for it per event,
// and hands back the hardest part of the design for free.
//
// Indices are free-running and never wrapped; only slot lookup masks. That makes
// full and empty unambiguous without sacrificing a slot: size == tail - head.
//
// Must not be copied after first use — atomic.Uint64 embeds noCopy, so
// `go vet -copylocks` enforces it. Note that `go test` does NOT run copylocks by
// default, so CI must run `go vet ./...` separately or the check never fires.
//
// Capacity is a package constant rather than a type parameter because Go has no
// non-type generic parameters, and because a compile-time constant is what lets
// the compiler drop the bounds check on slots[t&ringMask].
type SpscRing struct {
	// Producer line. The producer is the sole writer of both fields.
	tail       atomic.Uint64        // offset  0
	cachedHead uint64               // offset  8  producer-private, non-atomic on purpose
	_          [CacheLine - 16]byte // offset 16

	// Consumer line.
	head       atomic.Uint64        // offset 64
	cachedTail uint64               // offset 72  consumer-private
	_          [CacheLine - 16]byte // offset 80

	slots [RingCapacity]Event // offset 128
}

// --- producer side ---------------------------------------------------------

// TryPush copies value into the ring, returning false if it is full.
//
// Exactly one goroutine may call this for a given ring. That is the S in SPSC,
// and it is a much easier rule to break in Go than in C++, because
// "goroutine per unit of work" is the default idiom. The saving grace is that
// breaking it is a plain-memory data race on cachedHead, so `go test -race`
// reports it — where in C++ the same mistake is silent. See
// TestSPSCContractViolationIsDetected.
func (r *SpscRing) TryPush(value *Event) bool {
	t := r.tail.Load() // sole writer of tail; a plain MOVQ on amd64
	if t-r.cachedHead == RingCapacity {
		// Believed full. Only now pay for the consumer's cache line. Under Go's
		// sequentially consistent atomics this load is ordered after everything
		// the consumer did before its head store, so a slot it has finished
		// reading is safe for us to overwrite.
		r.cachedHead = r.head.Load()
		if t-r.cachedHead == RingCapacity {
			return false
		}
	}
	r.slots[t&ringMask] = *value
	// The publish. XCHGQ on amd64: it makes the slot write above visible to
	// anyone who loads tail, and — see signal() in bitmap.go — it is also the
	// StoreLoad barrier that the C++ has to buy separately with an explicit
	// atomic_thread_fence(seq_cst).
	r.tail.Store(t + 1)
	return true
}

// ProducerSizeHint is the producer's own view of occupancy: cheap and slightly
// stale, since cachedHead may lag, which is fine for the high-water statistic.
//
// Producer-only. It reads the private cached index, so calling it from any other
// goroutine is a data race — and that is not a theoretical objection: the C++
// version shipped exactly that bug, reading the producer's cached index from the
// stopping thread. Use SizeNow from anywhere else.
func (r *SpscRing) ProducerSizeHint() uint64 {
	return r.tail.Load() - r.cachedHead
}

// --- consumer side ---------------------------------------------------------

// PopBatch copies out at most len(out) items and frees their slots with a single
// store, so a 512-event drain costs one cross-core write rather than 512.
func (r *SpscRing) PopBatch(out []Event) int {
	h := r.head.Load() // sole writer of head
	if h == r.cachedTail {
		r.cachedTail = r.tail.Load()
		if h == r.cachedTail {
			return 0
		}
	}
	n := uint64(len(out))
	if avail := r.cachedTail - h; avail < n {
		n = avail
	}
	for i := uint64(0); i < n; i++ {
		out[i] = r.slots[(h+i)&ringMask]
	}
	// The pairing people forget.
	//
	// This is not merely a liveness hint telling the producer there is room. It
	// is what prevents the producer from overwriting a slot the consumer is
	// still reading: without it the slot reads above may be reordered after the
	// index update, the producer sees free space, and it writes into a slot
	// mid-read. That is a torn read, and it is silent.
	//
	// In Go the store is sequentially consistent, so the reordering is forbidden
	// by the language rather than by an annotation someone has to remember to
	// write. This is the one place where Go's lack of choice is unambiguously a
	// gain: it is the pairing the C++ writeup singles out as the one that gets
	// forgotten, and here it cannot be.
	r.head.Store(h + n)
	return int(n)
}

// EmptyNow is the consumer's authoritative emptiness check. It refreshes the
// cached index, which is what makes it the right call to use immediately after
// clearing an active-bitmap bit.
func (r *SpscRing) EmptyNow() bool {
	r.cachedTail = r.tail.Load()
	return r.head.Load() == r.cachedTail
}

// --- observer side ---------------------------------------------------------

// SizeNow is safe from a goroutine that is neither the producer nor the
// consumer: it reads only the two atomics, never the cached copies, which are
// private to their owning goroutine. The stopping goroutine must use this.
//
// Cached state is fast precisely because it is unshared, and that is what makes
// it unavailable to observers.
func (r *SpscRing) SizeNow() uint64 {
	t := r.tail.Load()
	h := r.head.Load()
	return t - h
}
