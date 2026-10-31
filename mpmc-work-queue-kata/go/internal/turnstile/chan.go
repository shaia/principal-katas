package turnstile

import (
	"sync/atomic"
	"time"
)

// ChanQueue is a buffered channel behind the same interface as Ring.
//
// The channel is already a bounded MPMC queue: runtime.hchan is a ring under
// one runtime mutex, with a queue of parked senders and one of parked
// receivers. Reservation, copy and publication happen inside that one critical
// section, so "reserved but not published" is never a state any goroutine can
// see.
//
// Its one hazard for this brief is shutdown. A send on a closed channel panics,
// so with 32 senders the data channel cannot be closed as the shutdown signal:
// Close closes a separate one, and ProducersDone closes the data channel only
// once whoever counted the producers out knows the last send has returned.
type ChanQueue[T any] struct {
	ch      chan T
	done    chan struct{}
	closing atomic.Bool
	send    Send
}

// Send is what a blocking push does on the way into the channel.
type Send uint8

const (
	// SendFast tries a non-blocking send, and only when the channel is full
	// selects over the send, the shutdown signal and the deadline. A
	// non-blocking send to a full channel returns without taking the lock
	// (runtime/chan.go, chansend), so the fast path's only lock is the send's.
	SendFast Send = iota
	// SendSelect selects over the send and the shutdown signal on every push:
	// the textbook spelling. selectgo locks every channel in the select, so
	// the shutdown channel's lock is taken by every push too.
	SendSelect
	// SendPlain is a bare send: no shutdown signal and no deadline. It never
	// refuses, so a producer using it stops only when its own quota does.
	SendPlain
)

func NewChan[T any](capacity int, send Send) *ChanQueue[T] {
	return &ChanQueue[T]{ch: make(chan T, capacity), done: make(chan struct{}), send: send}
}

// TryPush is the reject policy: select with a default. The closed check is a
// load of a line that only Close writes.
func (q *ChanQueue[T]) TryPush(v T, st *OpStats) Push {
	if q.closing.Load() {
		return PushClosed
	}
	select {
	case q.ch <- v:
		return PushOK
	default:
		return PushFull
	}
}

func (q *ChanQueue[T]) Push(v T, until time.Time, st *OpStats) Push {
	switch q.send {
	case SendPlain:
		q.ch <- v
		return PushOK
	case SendSelect:
		select {
		case q.ch <- v:
			return PushOK
		case <-q.done:
			return PushClosed
		}
	}
	if q.closing.Load() {
		return PushClosed
	}
	select {
	case q.ch <- v:
		return PushOK
	default:
	}
	now := time.Now()
	if !now.Before(until) {
		return PushTimeout
	}
	st.Parks++
	if st.timer == nil {
		st.timer = time.NewTimer(until.Sub(now))
	} else {
		st.timer.Reset(until.Sub(now))
	}
	select {
	case q.ch <- v:
		st.timer.Stop()
		return PushOK
	case <-q.done:
		st.timer.Stop()
		return PushClosed
	case <-st.timer.C:
		return PushTimeout
	}
}

func (q *ChanQueue[T]) TryPop(out *T, st *OpStats) Pop {
	select {
	case v, ok := <-q.ch:
		if !ok {
			return PopDrained
		}
		*out = v
		return PopOK
	default:
		return PopEmpty
	}
}

// Pop waits without a deadline. The bare receive is the channel's own park,
// and a deadline would cost a timer on every wait.
func (q *ChanQueue[T]) Pop(out *T, until time.Time, st *OpStats) Pop {
	v, ok := <-q.ch
	if !ok {
		return PopDrained
	}
	*out = v
	return PopOK
}

// Close is the shutdown signal. It does not touch the data channel.
func (q *ChanQueue[T]) Close() {
	if q.closing.CompareAndSwap(false, true) {
		close(q.done)
	}
}

// ProducersDone closes the data channel, after which receivers drain what it
// holds and then see it closed. Call it only after every producer has returned
// from its last push.
func (q *ChanQueue[T]) ProducersDone() { close(q.ch) }

func (q *ChanQueue[T]) Closed() bool { return q.closing.Load() }

func (q *ChanQueue[T]) Occupancy() uint64 { return uint64(len(q.ch)) }
