package weir

import "encoding/binary"

// Sink receives one batch as a contiguous byte view. In production it writes a
// socket.
//
// Implementations MUST NOT retain b past the call: it aliases the consumer's
// staging buffer, which is refilled immediately. This is the same contract
// std::span carries in the C++ version, stated here because Go's slices are
// retainable in a way spans are not.
//
// A plain interface, deliberately not a type parameter on Pipeline. Go stencils
// generics by GC shape, so every pointer-typed sink would share one
// instantiation and dispatch through a runtime dictionary anyway — an indirect
// call, exactly like an itab, but with fewer devirtualization opportunities and
// a viral type parameter on every type that holds a Pipeline. The call happens
// once per StageEvents (2048) events, so it is ~0.0005 calls per event either
// way. There is nothing to buy.
type Sink interface {
	Write(b []byte)
}

// ---------------------------------------------------------------------------
// The real one
// ---------------------------------------------------------------------------
//
// A socket sink has the same shape. A socket write is partial by nature, so the
// remainder has to be carried, and the connection must be non-blocking or the
// consumer stalls every ring behind one slow peer:
//
//	func (s *SocketSink) Write(b []byte) {
//	    s.pending = append(s.pending, b...)
//	    for len(s.pending) > 0 {
//	        n, err := s.conn.Write(s.pending)
//	        if n > 0 {
//	            s.pending = s.pending[n:]
//	            continue
//	        }
//	        if errors.Is(err, syscall.EWOULDBLOCK) || os.IsTimeout(err) {
//	            break // retry next pass
//	        }
//	        s.markFailed()
//	        return
//	    }
//	}
//
// When the peer stops draining, pending stops shrinking, the consumer falls
// behind, the rings fill, and the drop policy fires. That is the whole
// backpressure chain, and every link in it is bounded. A graceful drain then
// ends with conn.(*net.TCPConn).CloseWrite(), the Go spelling of
// shutdown(fd, SHUT_WR), so the peer sees a clean EOF rather than a reset.

// ---------------------------------------------------------------------------
// Benchmark sinks
// ---------------------------------------------------------------------------

// CountingSink measures the queue rather than the kernel: it counts bytes so
// accounting is verifiable, and touches the batch so the work cannot be elided.
//
// The sampling stride matters. A per-byte hash over a 64 KiB batch is a serial
// multiply chain tens of microseconds long — it would make the *sink* the
// bottleneck and every measurement would be reporting the checksum instead of
// the queue. The C++ version shipped exactly that bug and caught it by
// disbelieving its own throughput numbers. Sampling keeps this O(1)-ish.
type CountingSink struct {
	checksum uint64
	bytes    uint64
	batches  uint64
}

func NewCountingSink() *CountingSink {
	return &CountingSink{checksum: 0xcbf29ce484222325}
}

func (s *CountingSink) Write(b []byte) {
	h := s.checksum ^ uint64(len(b))
	words := len(b) / 8
	stride := words / 32
	if stride < 1 {
		stride = 1
	}
	for i := 0; i < words; i += stride {
		h = (h ^ binary.LittleEndian.Uint64(b[i*8:])) * 0x100000001b3
	}
	s.checksum = h
	s.bytes += uint64(len(b))
	s.batches++
}

func (s *CountingSink) Bytes() uint64    { return s.bytes }
func (s *CountingSink) Batches() uint64  { return s.batches }
func (s *CountingSink) Checksum() uint64 { return s.checksum }

// SlowSink is deliberately slow, to drive the rings into overflow and exercise
// the drop policy. It stands in for a socket whose peer has stopped reading.
//
// It spins on the TSC rather than calling time.Sleep, for the reason clock.go
// exists: a time.Sleep of 400 us on Windows is a park/unpark round trip whose
// actual duration this phase cannot control — measured, requests of 1, 10 and
// 100 us all return in ~510 us. A sink whose delay is not the delay you asked
// for is not a controlled variable.
type SlowSink struct {
	clock     *Clock
	delayTick uint64
	bytes     uint64
	batches   uint64
}

func NewSlowSink(c *Clock, delayNs int64) *SlowSink {
	return &SlowSink{clock: c, delayTick: c.NanosToTicks(delayNs)}
}

func (s *SlowSink) Write(b []byte) {
	s.bytes += uint64(len(b))
	s.batches++
	SpinUntil(s.clock.Ticks() + s.delayTick)
}

func (s *SlowSink) Bytes() uint64   { return s.bytes }
func (s *SlowSink) Batches() uint64 { return s.batches }

// Compile-time conformance, the Go spelling of the C++ static_assert(Sink<...>).
var (
	_ Sink = (*CountingSink)(nil)
	_ Sink = (*SlowSink)(nil)
)
