// internal/fakes/mqtt.go — Captures published MQTT messages for test assertions.

package fakes

import "sync"

type CapturedMessage struct {
	Topic   string
	Payload []byte
	QoS     byte
}

type FakePublisher struct {
	mu       sync.Mutex
	Messages []CapturedMessage
}

func (p *FakePublisher) Publish(topic string, qos byte, retained bool, payload []byte) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.Messages = append(p.Messages, CapturedMessage{
		Topic:   topic,
		Payload: append([]byte{}, payload...),
		QoS:     qos,
	})
	return nil
}

func (p *FakePublisher) LastMessage() *CapturedMessage {
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(p.Messages) == 0 {
		return nil
	}
	return &p.Messages[len(p.Messages)-1]
}
