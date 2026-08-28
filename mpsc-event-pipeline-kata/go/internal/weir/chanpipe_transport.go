package weir

import "context"

// Transport adapter so ChannelPipeline can be measured under the identical
// harness, sink and pacer as the ring design. Nothing else may vary.

type chanTransportAdapter struct{ p *ChannelPipeline }

// AsTransport lets the A/B measure the sharded-channel design against the rings.
func (p *ChannelPipeline) AsTransport() Transport { return &chanTransportAdapter{p} }

func (a *chanTransportAdapter) Name() string  { return "chan/producer" }
func (a *chanTransportAdapter) Start(s Sink)  { _ = a.p.Start(context.Background()) }
func (a *chanTransportAdapter) Stop(StopMode) { _ = a.p.Close() }
func (a *chanTransportAdapter) Stats() Stats  { return a.p.Stats() }
func (a *chanTransportAdapter) Register() (Producer, bool) {
	pr, err := a.p.Producer()
	if err != nil {
		return nil, false
	}
	return &chanProducerAdapter{pr}, true
}

type chanProducerAdapter struct{ c *ChannelProducer }

func (a *chanProducerAdapter) Push(e Event) bool { return a.c.Push(e) }
func (a *chanProducerAdapter) ID() uint32        { return a.c.ID() }
func (a *chanProducerAdapter) Close()            { _ = a.c.Close() }
