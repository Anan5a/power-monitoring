// cmd/api/main.go — API server entry point.
// Wires HTTP router, middleware, handlers, WebSocket hub, and MQTT live/#
// subscriber. Starts HTTP server on the configured port.

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/go-chi/chi/v5"
	chimw "github.com/go-chi/chi/v5/middleware"
	mqtt "github.com/eclipse/paho.mqtt.golang"
	"go.uber.org/automaxprocs"

	"github.com/yourorg/iot-platform/internal"
)

func main() {
	// Auto-detect CPU limits in Docker
	automaxprocs.Log()

	// Load config
	cfg, err := internal.LoadConfig()
	if err != nil {
		slog.Error("config", "error", err)
		os.Exit(1)
	}

	// Set log level
	slog.SetLogLoggerLevel(slog.LevelDebug)

	// Connect databases
	ctx := context.Background()
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

	// JWT manager
	jwt := internal.NewJWTManager(cfg.JWTSecret, cfg.JWTAccessTTL, cfg.JWTRefreshTTL)

	// WebSocket hub
	hub := internal.NewWebSocketHub()

	// MQTT client for live/# subscription
	mqttOpts := mqtt.NewClientOptions()
	mqttOpts.AddBroker(cfg.MQTTBroker)
	mqttOpts.SetClientID("iot-platform-api")
	mqttOpts.SetCleanSession(true)
	mqttOpts.OnConnect = func(c mqtt.Client) {
		slog.Info("MQTT connected (api)")
		c.Subscribe("live/#", 0, func(_ mqtt.Client, msg mqtt.Message) {
			hub.OnLiveMessage(msg.Topic(), msg.Payload())
		})
	}
	mqttClient := mqtt.NewClient(mqttOpts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		slog.Error("mqtt connect", "error", token.Error())
		os.Exit(1)
	}
	defer mqttClient.Disconnect(1000)

	// Handlers
	h := internal.NewHandlers(pg, jwt, ch)

	// Router
	r := chi.NewRouter()
	r.Use(chimw.RequestID)
	r.Use(chimw.RealIP)
	r.Use(internal.LoggerMiddleware)
	r.Use(internal.CORSMiddleware(cfg.CORSAllowedOrigins))
	r.Use(chimw.Recoverer)

	r.Route("/api/v1", func(r chi.Router) {
		// Public
		r.Post("/auth/register", h.Register)
		r.Post("/auth/login", h.Login)
		r.Post("/auth/refresh", h.RefreshToken)
		r.Get("/health", h.Health)

		// Mosquitto auth (called by Mosquitto HTTP plugin)
		r.Post("/mqtt/auth", internal.NewMQTTAuthHandler(pg).ServeHTTP)

		// Protected
		r.Group(func(r chi.Router) {
			r.Use(internal.AuthMiddleware(jwt))
			r.Get("/devices", h.ListDevices)
			r.Get("/devices/{key}", h.GetDevice)
			r.Post("/devices/{key}/claim", h.ClaimDevice)
			r.Get("/telemetry/{key}/latest", h.GetLatestTelemetry)
			r.Get("/ws", hub.HandleWS)
		})
	})

	// Server
	srv := &http.Server{
		Addr:         fmt.Sprintf(":%d", cfg.APIPort),
		Handler:      r,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Graceful shutdown
	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
		<-sig
		slog.Info("shutting down")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(shutdownCtx)
	}()

	slog.Info("API server starting", "port", cfg.APIPort)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		slog.Error("server", "error", err)
		os.Exit(1)
	}
}
