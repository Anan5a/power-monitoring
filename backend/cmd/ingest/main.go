// Command ingest is the durable telemetry ingest worker.
//
// It connects to MQTT, subscribes to telemetry/# at QoS 1, runs each message
// through the processing pipeline (resolve device → enrich → batch write), and
// flushes the accumulated batches to ClickHouse on a timer. It also runs the
// hourly retention cleanup and performs a final bounded flush on shutdown.

package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	_ "go.uber.org/automaxprocs"

	"github.com/Anan5a/iot-platform/internal"
)

func main() {
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

	// Build pipeline. Each stage is constructed separately so they can be
	// swapped (e.g. a cached resolver) or unit-tested in isolation.
	resolver := internal.NewDeviceResolver(pg) // Phase 2: add caching
	enricher := internal.NewEnricher()
	chStore := internal.NewCHStore(ch)
	// BatchWriter wraps chStore and accumulates rows in memory, flushing to
	// ClickHouse periodically to amortize round-trips.
	store := internal.NewBatchWriter(chStore)
	// RealClock injects wall-clock timestamps; tests can pass a fake clock.
	clock := internal.RealClock{}

	// MQTT client. CleanSession=false + AutoReconnect=true means the broker
	// keeps our subscriptions across reconnects and the client resubscribes
	// automatically, so we don't risk losing messages during a transient
	// network blip. QoS 1 on the subscription below guarantees at-least-once
	// delivery into the pipeline.
	mqttOpts := mqtt.NewClientOptions()
	mqttOpts.AddBroker(cfg.MQTTBroker)
	mqttOpts.SetClientID(cfg.MQTTClientID)
	mqttOpts.SetCleanSession(false)
	mqttOpts.SetAutoReconnect(true)
	if cfg.MQTTUser != "" {
		mqttOpts.SetUsername(cfg.MQTTUser)
		mqttOpts.SetPassword(cfg.MQTTPassword)
	}

	mqttClient := mqtt.NewClient(mqttOpts)
	// Block on the initial connect: if the broker is unreachable at boot we
	// would rather fail fast than silently drop telemetry.
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		slog.Error("mqtt connect", "error", token.Error())
		os.Exit(1)
	}
	defer mqttClient.Disconnect(1000)

	// Wire MQTT publisher with the connected client. The pipeline publishes
	// enriched messages to live/# for the API server to fan out; wrapping the
	// paho client behind the MQTTPublisher interface keeps the pipeline
	// decoupled from the MQTT driver and testable with a fake.
	mqttPub := &mqttPublisher{client: mqttClient}
	pipe := internal.NewPipeline(resolver, enricher, store, mqttPub, clock)

	// Maintenance mode check (pauses ingest when enabled). Reading the flag
	// from Postgres on every message is cheap relative to a pipeline run and
	// ensures a flag toggle takes effect immediately without a restart.
	maintenanceMode := internal.NewMaintenanceMode(pg)

	// Subscribe to telemetry topics at QoS 1 for durable delivery. The token
	// error check is required: paho Subscribe is asynchronous, so a failed
	// subscription (e.g. broker rejected the ACL) would otherwise be silent
	// and the worker would appear healthy while ingesting nothing.
	if token := mqttClient.Subscribe("telemetry/#", 1, func(_ mqtt.Client, msg mqtt.Message) {
		if maintenanceMode.IsEnabled() {
			// Maintenance window: skip processing but do NOT NACK/ack — at
			// QoS 1 paho has already acked delivery, and dropping the message
			// here is intentional so operators can drain backlogs safely.
			slog.Warn("ingest paused — maintenance mode enabled")
			return
		}
		if err := pipe.Process(ctx, msg); err != nil {
			slog.Error("pipeline", "error", err, "topic", msg.Topic())
		}
	}); token.Wait() && token.Error() != nil {
		slog.Error("subscribe telemetry/#", "error", token.Error())
		os.Exit(1)
	}

	// Start batch flush loop. FlushLoop periodically calls store.Flush so
	// buffered rows land in ClickHouse even under low message volume.
	go store.FlushLoop(ctx)

	// Start retention cleanup (hourly). Removes telemetry older than the
	// configured retention window from both Postgres metadata and ClickHouse.
	retention := internal.NewRetentionCleanup(pg, ch)
	go retention.RunLoop(ctx)

	// Wait for shutdown. SIGINT/SIGTERM unblock the receive and let us run
	// a final flush before the deferred closes run.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	slog.Info("ingest worker shutting down")
	// Final flush with a fresh context — the main ctx is still Background here,
	// but use a bounded shutdown context so a slow ClickHouse can't hang exit.
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := store.Flush(shutdownCtx); err != nil {
		slog.Error("final flush", "error", err)
	}
}

// mqttPublisher wraps a paho MQTT client so it satisfies the internal
// MQTTPublisher interface used by the pipeline. It is the concrete
// implementation used in production; tests substitute a fake publisher.
type mqttPublisher struct {
	client mqtt.Client
}

// Publish sends a message to the broker, blocking until the publish token
// resolves. A nil receiver client is treated as a no-op so the publisher
// can be safely embedded in tests or disabled configurations.
func (p *mqttPublisher) Publish(topic string, qos byte, retained bool, payload []byte) error {
	if p.client == nil {
		return nil
	}
	token := p.client.Publish(topic, qos, retained, payload)
	token.Wait()
	return token.Error()
}
