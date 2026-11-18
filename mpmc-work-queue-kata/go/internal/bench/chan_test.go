package bench

import (
	"strings"
	"testing"
	"time"

	"turnstile/internal/turnstile"
)

// The reason the data channel is not the shutdown signal: closing it while a
// sender is still active turns the close-versus-send race into a crash.
func TestSendOnClosedChannelPanics(t *testing.T) {
	q := chanWith(sendFast)()
	p := q.Producer()
	q.ProducersDone()
	defer func() {
		err, ok := recover().(error)
		if !ok || !strings.Contains(err.Error(), "send on closed channel") {
			t.Fatalf("recovered %v, want the runtime's send-on-closed panic", err)
		}
	}()
	p.TryPush(turnstile.MakeEvent(0, 0, 0))
}

// 32 senders, a separate shutdown signal, and the data channel closed only
// after every sender has returned: no panic, nothing stranded, every producer
// refused once.
func TestShutdownProtocol(t *testing.T) {
	for _, s := range []send{sendFast, sendSelect} {
		for range 10 {
			r := Run(chanWith(s)(), Spec{Producers: 32, Consumers: 4, CloseAfter: 2 * time.Millisecond})
			if !r.ExactlyOnce || r.Torn != 0 {
				t.Errorf("send %d: accepted %d, consumed %d, torn %d", s, r.Accepted, r.Consumed, r.Torn)
			}
			if r.RefusedClosed != 32 {
				t.Errorf("send %d: %d of 32 producers refused as closed", s, r.RefusedClosed)
			}
		}
	}
}
