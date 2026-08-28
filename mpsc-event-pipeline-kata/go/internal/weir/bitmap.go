package weir

import "math/bits"

// signal tells the consumer this ring is non-empty — as rarely as possible.
//
// THE FENCE IS GONE, AND ITS ABSENCE IS THE POINT.
//
// In C++ this function opens with atomic_thread_fence(seq_cst), and that
// writeup spends a page on why it cannot be moved below the check. x86 is TSO,
// which permits exactly one reordering — StoreLoad — so an unfenced load of the
// bitmap may execute before the tail publish drains the store buffer. The
// producer then reads a set bit, skips the fetch_or, and returns, while the
// consumer concurrently sees the ring empty, clears the bit, re-checks, still
// sees empty, and parks. The event is now published with its bit clear, nobody
// will probe that ring, and it sits there until this producer happens to push
// again — which it may never do. That is unbounded latency on a published
// event: rare, load-dependent, and invisible in testing. The C++ author shipped
// the "obvious optimization" — check the bit first, fence only on the slow path
// — before catching it, and says plainly that this class of bug is found by
// reasoning or not at all.
//
// Go forecloses it. Every sync/atomic operation is sequentially consistent, so
// there is a single total order S containing the producer's tail.Store (A1) and
// active.Load (A2), and the consumer's active.And (B1) and tail.Load inside
// EmptyNow (B2), with A1 <S A2 and B1 <S B2 by program order.
//
// Suppose the bad outcome: A2 reads the bit set, so the producer returns; and B2
// reads the ring empty, so the consumer parks. A2 reading the bit set means
// A2 <S B1, since B1 is the only operation that clears it. Then
// A1 <S A2 <S B1 <S B2, so B2 must observe A1 and the ring is not empty.
// Contradiction. The protocol is correct with no annotation at all.
//
// On amd64 this is not a coincidence: the tail publish compiles to XCHGQ, a
// lock-prefixed instruction that drains the store buffer, so the hardware edge
// is the same one the C++ fence buys. The difference is that in Go the argument
// is a language-level one, and the tempting optimization is not expressible:
// there is no relaxed load to write. The only wrong thing available here is a
// plain non-atomic read of active, which is a data race by definition and which
// -race reports.
//
// The cost of that guarantee is charged in spsc_ring.go, on every push, whether
// or not the bitmap is enabled.
func (p *Pipeline) signal(id uint32) {
	bit := uint64(1) << id

	// Fast path: the bit is already set almost always, because a streaming
	// producer sets it once and then never touches the line again. That is the
	// coalescing, and it is what makes a shared word acceptable here — a line
	// that is *read* by 64 cores sits Shared in 64 caches simultaneously and
	// costs each an L1 hit. Cost appears only on writes.
	//
	// It must still be an atomic load: a plain read racing the consumer's And is
	// undefined behaviour in Go, not merely stale. It costs nothing — a plain
	// MOVQ on amd64, same as the C++ relaxed load.
	if p.active.Load()&bit != 0 {
		return
	}

	prev := p.active.Or(bit) // writes happen only on an empty->non-empty edge
	p.bitmapWrites.Add(1)

	// Coalesced notify: only the producer that took the whole bitmap from zero
	// owes a wakeup, so a burst across 8 producers costs one notify rather than
	// eight. Notifying on every push would be a syscall per event, which is
	// categorically worse than the polling it replaces.
	//
	// Same Dekker shape one level up, and free for the same reason: prev == 0
	// means our Or precedes the consumer's active.Load inside park(), so if we
	// read parked == false the consumer had not yet armed, and its subsequent
	// load must observe our Or and it will not park.
	if prev == 0 && p.parked.Load() {
		p.notify()
	}
}

// clearEmptyBits is the consumer's half of the Dekker pair.
//
// A set bit means "maybe non-empty". The asymmetry is the whole design: a false
// positive costs one wasted probe, a false negative loses a wakeup. So bits stay
// set while there is work, and a busy producer's bit is written once and left
// alone — which is what keeps the line read-mostly.
//
// Called ONLY on the path to parking, never after each drain. This is the one
// trap Go does NOT close: it is not an ordering bug, it is a design bug about
// write frequency on a shared line, and no language feature prevents it. The
// C++ version's first implementation cleared eagerly and produced 582,954 writes
// to the shared line where this produces one per producer. No test failed. The
// only thing that made it visible was having instrumented the quantity the
// design claims to optimize.
func (p *Pipeline) clearEmptyBits() {
	m := p.active.Load()
	for m != 0 {
		i := uint32(bits.TrailingZeros64(m))
		m &= m - 1
		bit := uint64(1) << i
		slot := &p.slots[i]
		if !slot.ring.EmptyNow() {
			continue
		}

		// Clear first, THEN look again.
		//
		// In C++ the comment here has to point out that fetch_and is an RMW and
		// therefore already a full barrier, so no explicit fence is needed on
		// this side. In Go there is nothing to point out: And and EmptyNow's
		// Load are both in the single total order, in program order, and that is
		// all the protocol needs.
		//
		// If a producer published before our clear, the re-check sees its event;
		// if it publishes after, it sees the cleared bit and sets it itself.
		p.active.And(^bit)
		p.bitmapWrites.Add(1)
		if !slot.ring.EmptyNow() || slot.state.Load() == slotRetiring {
			p.active.Or(bit)
			p.bitmapWrites.Add(1)
		}
	}
}

// drainBitmap is O(active) instead of O(64).
//
// bits.RotateLeft64(x, -rotate) is math/bits' spelling of std::rotr. The
// rotation moves the scan's starting bit each pass so producer 0 is not drained
// first every time: TrailingZeros64 scans from the least significant bit, and
// under sustained overload a fixed LSB-first order starves the high indices.
// Combined with the per-ring DrainBatch cap, that is the whole fairness story.
func (p *Pipeline) drainBitmap(sink Sink, rotate uint32) int {
	m := bits.RotateLeft64(p.active.Load(), -int(rotate))
	total := 0
	for m != 0 {
		off := uint32(bits.TrailingZeros64(m))
		m &= m - 1
		i := (off + rotate) & (MaxProducers - 1)

		p.local.ringsProbed++
		n := p.drainRing(sink, i)
		if n == 0 {
			p.local.ringsEmpty++
		}
		total += n
	}
	if total > 0 {
		p.flush(sink)
	}
	return total
}
