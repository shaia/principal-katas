package weir

import (
	"context"
	"sync"
	"testing"
)

func TestChannelPipelineInvariants(t *testing.T) {
	sink := NewCountingSink()
	p := NewChannelPipeline(sink, WithProducers(16), WithCapacity(4096))
	if err := p.Start(context.Background()); err != nil {
		t.Fatal(err)
	}

	const producers, per = 8, 50_000
	var wg sync.WaitGroup
	for i := 0; i < producers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			pr, err := p.Producer()
			if err != nil {
				t.Error(err)
				return
			}
			defer pr.Close()
			for s := 0; s < per; s++ {
				pr.Push(Event{ProducerID: pr.ID(), Seq: uint32(s)})
			}
		}()
	}
	wg.Wait()
	if err := p.Close(); err != nil {
		t.Fatal(err)
	}

	st := p.Stats()
	offered := uint64(producers * per)
	if st.Pushed+st.Dropped != offered {
		t.Errorf("accounting: pushed=%d dropped=%d sum=%d want %d",
			st.Pushed, st.Dropped, st.Pushed+st.Dropped, offered)
	}
	if st.Consumed != st.Pushed {
		t.Errorf("drain lost events: consumed=%d pushed=%d", st.Consumed, st.Pushed)
	}
	if st.HighWater > 4096 {
		t.Errorf("memory not bounded: high_water=%d", st.HighWater)
	}
	t.Logf("chan: pushed=%d dropped=%d consumed=%d high_water=%d",
		st.Pushed, st.Dropped, st.Consumed, st.HighWater)
}

func TestChannelPipelineCloseIsIdempotent(t *testing.T) {
	p := NewChannelPipeline(NewCountingSink())
	if err := p.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	if err := p.Close(); err != nil {
		t.Fatal(err)
	}
	if err := p.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestChannelPipelineRegistryFull(t *testing.T) {
	p := NewChannelPipeline(NewCountingSink(), WithProducers(2), WithCapacity(8))
	defer p.Close()
	if err := p.Start(context.Background()); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if _, err := p.Producer(); err != nil {
			t.Fatalf("producer %d: %v", i, err)
		}
	}
	if _, err := p.Producer(); err == nil {
		t.Error("expected ErrRegistryFull")
	}
}
