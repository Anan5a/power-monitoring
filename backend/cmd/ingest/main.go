// cmd/ingest/main.go — Ingest worker entry point.
// Connects to MQTT, subscribes to telemetry/#, runs the pipeline,
// and flushes telemetry to ClickHouse in batches.

package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"go.uber.org/automaxprocs"

	"github.com/yourorg/iot-platform/internal"
)

func main() {
	automaxprocs.Log()

	cfg, err := internal.LoadConfig()
	if err != nil {
		slog.Error("config", "error", err)
		os.Exit(1)
	}

	ctx := context.Background()

	// Connect databases
	pg, err := internal.ConnectPG(ctx, cfg.DatabaseURL)
	if err != nil {
		slog.Error("postgres", "error", err)
		os.Exit(1)
	}
	defer pg.Close()

	ch, err := internal.ConnectCH(ctx, cfg.ClickHouseURL)
	if err != nil {
		slog.Error("clickhouse", "error", err)
		os.Exit(1)
	}
	defer ch.Close()

	// Build pipeline
	resolver := internal.NewDeviceResolver(pg) // Phase 2: add caching
	enricher := internal.NewEnricher()
	chStore := internal.NewCHStore(ch)
	store := internal.NewBatchWriter(chStore, ch, pg)
	mqttPub := &mqttPublisher{client: nil}        // Phase 2: real MQTT publisher
	clock := internal.RealClock{}

	pipe := internal.NewPipeline(resolver, enricher, store, mqttPub, clock)

	// MQTT client
	mqttOpts := mqtt.NewClientOptions()
	mqttOpts.AddBroker(cfg.MQTTBroker)
	mqttOpts.SetClientID(cfg.MQTTClientID)
	mqttOpts.SetCleanSession(false)
	mqttOpts.SetAutoReconnect(true)
	mqttOpts.OnConnect = func(c mqtt.Client) {
		slog.Info("MQTT connected (ingest)")
		c.Subscribe("telemetry/#", 1, func(_ mqtt.Client, msg mqtt.Message) {
			if err := pipe.Process(ctx, msg); err != nil {
				slog.Error("pipeline", "error", err, "topic", msg.Topic())
			}
			msg.Ack()
		})
	}
	mqttClient := mqtt.NewClient(mqttOpts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		slog.Error("mqtt connect", "error", token.Error())
		os.Exit(1)
	}
	defer mqttClient.Disconnect(1000)

	// Start batch flush loop
	go store.FlushLoop(ctx)

	// Wait for shutdown
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	slog.Info("ingest worker shutting down")
	store.Flush(ctx) // final flush
}

// mqttPublisher wraps paho MQTT client for the MQTTPublisher interface.
type mqttPublisher struct {
	client mqtt.Client
}

func (p *mqttPublisher) Publish(topic string, qos byte, retained bool, payload []byte) error {
	if p.client == nil {
		return nil // Phase 2: wire real client
	}
	token := p.client.Publish(topic, qos, retained, payload)
	token.Wait()
	return token.Error()
}
