package bench

import "math/bits"

// Histogram is a fixed-size latency histogram in counter ticks: log-linear
// buckets about 3 % wide over the whole 64-bit range. One per goroutine, so
// recording is a plain increment with no sharing and no allocation; merged
// after the run.
type Histogram struct {
	counts [buckets]uint64
	n, max uint64
}

// Values below 32 get a bucket each. Above that, each power of two is split
// into 16: index = 16·e + (v >> e), where e puts (v >> e) in [16, 32).
const buckets = 976

func index(v uint64) int {
	if v < 32 {
		return int(v)
	}
	e := bits.Len64(v) - 5
	return 16*e + int(v>>e)
}

func upper(i int) uint64 {
	if i < 32 {
		return uint64(i)
	}
	e := i/16 - 1
	m := uint64(i - 16*e)
	return (m+1)<<e - 1
}

func (h *Histogram) Record(v uint64) {
	h.counts[index(v)]++
	h.n++
	h.max = max(h.max, v)
}

func (h *Histogram) Merge(o *Histogram) {
	for i := range h.counts {
		h.counts[i] += o.counts[i]
	}
	h.n += o.n
	h.max = max(h.max, o.max)
}

func (h *Histogram) Count() uint64 { return h.n }
func (h *Histogram) Max() uint64   { return h.max }

// Quantile is the upper edge of the bucket holding the q-quantile. It can
// over-report by one bucket width and never under-reports, which is the right
// way round for a number compared against a budget.
func (h *Histogram) Quantile(q float64) uint64 {
	if h.n == 0 {
		return 0
	}
	rank := uint64(q*float64(h.n-1)) + 1
	var seen uint64
	for i, c := range h.counts {
		seen += c
		if seen >= rank {
			return min(upper(i), h.max)
		}
	}
	return h.max
}

// Above counts samples strictly above v, to bucket resolution.
func (h *Histogram) Above(v uint64) uint64 {
	var n uint64
	for i := index(v) + 1; i < buckets; i++ {
		n += h.counts[i]
	}
	return n
}
