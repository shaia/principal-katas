package weir

import (
	"reflect"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"unsafe"
)

func TestEventLayout(t *testing.T) {
	if got := unsafe.Sizeof(Event{}); got != 32 {
		t.Fatalf("sizeof(Event) = %d, want 32", got)
	}
	for _, c := range []struct {
		name string
		got  uintptr
		want uintptr
	}{
		{"ProducerID", unsafe.Offsetof(Event{}.ProducerID), 0},
		{"Seq", unsafe.Offsetof(Event{}.Seq), 4},
		{"StampNs", unsafe.Offsetof(Event{}.StampNs), 8},
		{"Payload", unsafe.Offsetof(Event{}.Payload), 16},
	} {
		if c.got != c.want {
			t.Errorf("offsetof(Event.%s) = %d, want %d", c.name, c.got, c.want)
		}
	}
}

// TestEventIsPointerFree is the guard on the design's headline GC property. A
// single pointer field here silently moves 8 MiB of ring storage onto the mark
// path; measured, that is 2.7x the GC work and 26.8 us of added stop-the-world.
// The obvious way to break it is to reach for time.Time, which embeds
// loc *Location.
func TestEventIsPointerFree(t *testing.T) {
	var walk func(reflect.Type, string)
	walk = func(ty reflect.Type, path string) {
		switch ty.Kind() {
		case reflect.Pointer, reflect.UnsafePointer, reflect.Map, reflect.Chan,
			reflect.Func, reflect.Slice, reflect.String, reflect.Interface:
			t.Errorf("Event contains a pointer at %s (%s): rings would become "+
				"scannable heap", path, ty)
		case reflect.Array:
			walk(ty.Elem(), path+"[]")
		case reflect.Struct:
			for i := 0; i < ty.NumField(); i++ {
				f := ty.Field(i)
				walk(f.Type, path+"."+f.Name)
			}
		}
	}
	walk(reflect.TypeOf(Event{}), "Event")
}

// TestRingLayout asserts the cache-line separation that C++ gets from
// alignas(64). Go has no alignas, so padding plus this test is the substitute —
// and a test is what catches the failure mode the C++ actually hit, where one
// alignas was missing and four hot fields silently shared a line.
func TestRingLayout(t *testing.T) {
	var r SpscRing
	checks := []struct {
		name string
		got  uintptr
		want uintptr
	}{
		{"tail", unsafe.Offsetof(r.tail), 0},
		{"cachedHead", unsafe.Offsetof(r.cachedHead), 8},
		{"head", unsafe.Offsetof(r.head), 64},
		{"cachedTail", unsafe.Offsetof(r.cachedTail), 72},
		{"slots", unsafe.Offsetof(r.slots), 128},
	}
	for _, c := range checks {
		if c.got != c.want {
			t.Errorf("offsetof(SpscRing.%s) = %d, want %d", c.name, c.got, c.want)
		}
	}
	if got, want := unsafe.Sizeof(r), uintptr(128+RingCapacity*32); got != want {
		t.Errorf("sizeof(SpscRing) = %d, want %d", got, want)
	}

	// tail and head must not share a line, or every push invalidates the line
	// the consumer writes and vice versa: a ~1 ns store becomes a ~100 ns
	// coherence miss. This is the single most common way a hand-rolled
	// lock-free queue benchmarks slower than a mutex.
	tl := unsafe.Offsetof(r.tail) / CacheLine
	hl := unsafe.Offsetof(r.head) / CacheLine
	if tl == hl {
		t.Errorf("tail and head share cache line %d", tl)
	}

	// Absolute alignment is an allocator property, not a language guarantee:
	// objects >32 KiB are page-allocated. Assert it rather than assume it.
	for i := 0; i < 32; i++ {
		p := new(SpscRing)
		if a := uintptr(unsafe.Pointer(&p.tail)) % CacheLine; a != 0 {
			t.Fatalf("heap-allocated ring is %d bytes off a cache line", a)
		}
	}
}

func TestRingSingleThreadedSemantics(t *testing.T) {
	r := new(SpscRing)
	if !r.EmptyNow() {
		t.Fatal("fresh ring is not empty")
	}
	if n := r.PopBatch(make([]Event, 8)); n != 0 {
		t.Fatalf("pop from empty ring returned %d", n)
	}

	// Fill exactly to capacity, then confirm the next push fails.
	for i := 0; i < RingCapacity; i++ {
		e := Event{ProducerID: 1, Seq: uint32(i)}
		if !r.TryPush(&e) {
			t.Fatalf("push %d failed before capacity", i)
		}
	}
	e := Event{ProducerID: 1, Seq: RingCapacity}
	if r.TryPush(&e) {
		t.Fatal("push succeeded past capacity: memory is not bounded")
	}
	if got := r.SizeNow(); got != RingCapacity {
		t.Fatalf("SizeNow = %d, want %d", got, RingCapacity)
	}

	// Drain and check order and content.
	out := make([]Event, 512)
	seen := 0
	for {
		n := r.PopBatch(out)
		if n == 0 {
			break
		}
		for i := 0; i < n; i++ {
			if out[i].Seq != uint32(seen) {
				t.Fatalf("out of order: got seq %d, want %d", out[i].Seq, seen)
			}
			seen++
		}
	}
	if seen != RingCapacity {
		t.Fatalf("drained %d events, want %d", seen, RingCapacity)
	}
	if !r.EmptyNow() {
		t.Fatal("ring not empty after full drain")
	}
}

// TestRingWrapsFarPastCapacity exercises the free-running indices. Wrapping the
// *index* rather than only the lookup would make full and empty ambiguous.
func TestRingWrapsFarPastCapacity(t *testing.T) {
	r := new(SpscRing)
	out := make([]Event, 64)
	const rounds = RingCapacity * 20
	next := uint32(0)
	for i := 0; i < rounds; i++ {
		e := Event{ProducerID: 7, Seq: uint32(i)}
		if !r.TryPush(&e) {
			t.Fatalf("unexpected full at %d", i)
		}
		if i%3 == 0 {
			n := r.PopBatch(out)
			for k := 0; k < n; k++ {
				if out[k].Seq != next {
					t.Fatalf("order violation at %d: got %d want %d", i, out[k].Seq, next)
				}
				next++
			}
		}
	}
}

// TestRingConcurrent is the -race target for the ring itself: exactly one
// producer goroutine and one consumer goroutine, which is the contract.
func TestRingConcurrent(t *testing.T) {
	r := new(SpscRing)
	const total = 200_000
	var wg sync.WaitGroup
	wg.Add(2)

	go func() { // producer
		defer wg.Done()
		for i := 0; i < total; i++ {
			e := Event{ProducerID: 3, Seq: uint32(i)}
			for !r.TryPush(&e) {
				runtime.Gosched()
			}
		}
	}()

	got := 0
	var violations int
	go func() { // consumer
		defer wg.Done()
		out := make([]Event, DrainBatch)
		for got < total {
			n := r.PopBatch(out)
			for i := 0; i < n; i++ {
				if out[i].Seq != uint32(got+i) {
					violations++
				}
			}
			got += n
			if n == 0 {
				runtime.Gosched()
			}
		}
	}()

	wg.Wait()
	if got != total {
		t.Errorf("consumed %d, want %d", got, total)
	}
	if violations != 0 {
		t.Errorf("%d ordering violations", violations)
	}
}

// TestPushIsAllocationFree proves the "minimal allocation" requirement rather
// than asserting it. Note the gotcha: AllocsPerRun sets GOMAXPROCS=1 for its
// duration, so this must run against a ring with no consumer goroutine.
func TestPushIsAllocationFree(t *testing.T) {
	r := new(SpscRing)
	e := Event{ProducerID: 1}
	if n := testing.AllocsPerRun(10_000, func() {
		if !r.TryPush(&e) {
			// Drain by fiat; we are measuring the push, not the protocol.
			r.head.Store(r.tail.Load())
			r.cachedHead = r.tail.Load()
		}
	}); n != 0 {
		t.Errorf("TryPush allocates %v times per call, want 0", n)
	}
}

func BenchmarkRingPush(b *testing.B) {
	r := new(SpscRing)
	e := Event{ProducerID: 1}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if !r.TryPush(&e) {
			r.head.Store(r.tail.Load())
			r.cachedHead = r.tail.Load()
		}
	}
}

// BenchmarkPublishStore is the number section 2 of the writeup is built on: what
// Go's sequentially consistent store costs against the plain store C++ gets to
// use for the same publish.
func BenchmarkPublishStore(b *testing.B) {
	b.Run("atomic-seqcst-XCHGQ", func(b *testing.B) {
		var v atomic.Uint64
		for i := 0; i < b.N; i++ {
			v.Store(uint64(i))
		}
	})
	b.Run("plain-store-MOVQ", func(b *testing.B) {
		var v uint64
		for i := 0; i < b.N; i++ {
			v = uint64(i)
		}
		_ = v
	})
	b.Run("atomic-load-MOVQ", func(b *testing.B) {
		var v atomic.Uint64
		var sink uint64
		for i := 0; i < b.N; i++ {
			sink += v.Load()
		}
		_ = sink
	})
}
