package weir

// notify never blocks.
//
// The capacity-1 channel plus a non-blocking send IS the coalescing: N
// concurrent notifies collapse to at most one pending token. That makes the
// prev == 0 gate in signal() belt-and-braces — it now saves the channel
// operation rather than being the sole mechanism — but it is still worth having
// on a path that runs per push.
func (p *Pipeline) notify() {
	select {
	case p.wake <- struct{}{}:
		p.notifies.Add(1)
	default:
		// A wakeup is already pending; nothing to do.
	}
}

func (p *Pipeline) wakeConsumer() { p.notify() }

// park sleeps until a producer signals or the backstop fires.
//
// WHY THIS IS A CHANNEL AND NOT A sync.Cond.
//
// sync.Cond is the direct port of std::condition_variable and it is the wrong
// choice, for a structural reason rather than a stylistic one: sync.Cond has no
// timed wait. It offers Wait, Signal and Broadcast, and nothing else.
//
// The C++ design's third layer of defence is wait_for(lk, park_timeout) — a
// bounded worst case, which that writeup explicitly says is worth more than
// confidence that the wakeup path is perfect. Reproducing that with sync.Cond
// requires a second goroutine broadcasting on a ticker: an extra goroutine, a
// Broadcast storm, and a wakeup path more complex than the one it is insuring.
// A channel gives the timeout in a select.
//
// So the idiomatic Go answer here is also the forced one, which is a pleasant
// coincidence and worth naming rather than presenting the channel as taste.
//
// The timer is created once and reused. time.After allocates a *Timer and a
// chan Time on every call, and at a 200 us backstop on an idle pipeline that is
// thousands of allocations per second of pure garbage — on the one path this
// design goes out of its way not to allocate on.
func (p *Pipeline) park() {
	p.parked.Store(true)

	// Re-check after arming, or a producer that signalled between our last scan
	// and this store would find parked == false, skip the notify, and leave us
	// asleep on an already non-empty ring. Under sequentially consistent atomics
	// this is the same Dekker argument as signal(), and it needs no annotation.
	if p.active.Load() == 0 && p.running.Load() {
		p.parks.Add(1)
		p.timer.Reset(p.cfg.ParkTimeout)
		select {
		case <-p.wake:
			// Go 1.23+: after Stop returns, a receive from t.C blocks rather
			// than delivering a stale value. Do NOT drain here — the older
			// `if !t.Stop() { <-t.C }` idiom deadlocks on modern Go.
			p.timer.Stop()
		case <-p.timer.C:
			// The backstop fired. A bounded worst case beats trusting the
			// wakeup path to be perfect.
		}
	}
	p.parked.Store(false)
}
