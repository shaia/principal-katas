package turnstile

import (
	"strings"
	"testing"
	"time"
)

// The reason the data channel is not the shutdown signal: closing it while a
// sender is still active turns the close-versus-send race into a crash.
func TestSendOnClosedChannelPanics(t *testing.T) {
	q := NewChan[Event](8, SendFast)
	q.ProducersDone()
	defer func() {
		r := recover()
		err, ok := r.(error)
		if !ok || !strings.Contains(err.Error(), "send on closed channel") {
			t.Fatalf("recovered %v, want the runtime's send-on-closed panic", r)
		}
	}()
	var st OpStats
	q.TryPush(MakeEvent(0, 0, 0), &st)
}

// 32 senders, a separate shutdown signal, the data channel closed only after
// every sender has returned: no panic, nothing stranded.
func TestShutdownProtocol(t *testing.T) {
	for _, s := range []Send{SendFast, SendSelect} {
		for round := 0; round < 10; round++ {
			stress(t, NewChan[Event](256, s), 32, 4, 0, block, 2*time.Millisecond)
		}
	}
}
