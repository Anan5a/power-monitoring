// cmd/api/main.go — API server entry point.
// Wires HTTP router, middleware, handlers, WebSocket hub, and MQTT live/#
// subscriber. Starts HTTP server on the configured port.

package main

import (
	"context"
	"encoding/json"
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
	httpSwagger "github.com/swaggo/http-swagger"
	_ "go.uber.org/automaxprocs"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"

	"github.com/Anan5a/iot-platform/internal"
)

func main() {
	cfg, err := internal.LoadConfig()
	if err != nil {
		slog.Error("config", "error", err)
		os.Exit(1)
	}

	slog.SetLogLoggerLevel(slog.LevelDebug)

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

	jwt := internal.NewJWTManager(cfg.JWTSecret, cfg.JWTAccessTTL, cfg.JWTRefreshTTL)

	emailSvc := internal.NewEmailService(pg, cfg.SMTPFrom, cfg.SMTPHost, cfg.SMTPPort, cfg.SMTPUser, cfg.SMTPPass)
	go emailSvc.DrainLoop(ctx)

	alertEngine := internal.NewAlertEngine(pg, emailSvc)

	hub := internal.NewWebSocketHub()

	mqttOpts := mqtt.NewClientOptions()
	mqttOpts.AddBroker(cfg.MQTTBroker)
	mqttOpts.SetClientID("iot-platform-api")
	mqttOpts.SetCleanSession(true)
	if cfg.MQTTUser != "" {
		mqttOpts.SetUsername(cfg.MQTTUser)
		mqttOpts.SetPassword(cfg.MQTTPassword)
	}
	mqttOpts.OnConnect = func(c mqtt.Client) {
		slog.Info("MQTT connected (api)")
		c.Subscribe("live/#", 0, func(_ mqtt.Client, msg mqtt.Message) {
			hub.OnLiveMessage(msg.Topic(), msg.Payload())
			var enriched internal.EnrichedTelemetry
			if err := json.Unmarshal(msg.Payload(), &enriched); err == nil {
				alertEngine.Evaluate(context.Background(), enriched.DeviceKey, &enriched)
			}
		})
	}
	mqttClient := mqtt.NewClient(mqttOpts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		slog.Error("mqtt connect", "error", token.Error())
		os.Exit(1)
	}
	defer mqttClient.Disconnect(1000)

	h := internal.NewHandlers(pg, jwt, ch)
	otaHandler := internal.NewOTAHandler(pg)
	groupHandler := internal.NewGroupHandler(pg)
	searchHandler := internal.NewSearchHandler(pg)
	oauthHandler := internal.NewOAuthHandler(pg, jwt, cfg.GoogleClientID, cfg.GoogleClientSecret, cfg.GitHubClientID, cfg.GitHubClientSecret, cfg.BaseURL)
	billingHandler := internal.NewBillingHandler(pg)

	minioClient, err := minio.New(cfg.MinIOEndpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(cfg.MinIOUser, cfg.MINIOPassword, ""),
		Secure: false,
	})
	if err != nil {
		slog.Error("minio", "error", err)
		os.Exit(1)
	}

	exportHandler := internal.NewExportHandler(pg, minioClient, cfg.MinIOBucket, cfg.BaseURL)
	maintenanceMode := internal.NewMaintenanceMode(pg)

	r := chi.NewRouter()
	r.Use(chimw.RequestID)
	r.Use(chimw.RealIP)
	r.Use(internal.LoggerMiddleware)
	r.Use(internal.CORSMiddleware(cfg.CORSAllowedOrigins))
	r.Use(maintenanceMode.Middleware())
	r.Use(chimw.Recoverer)

	r.Route("/api/v1", func(r chi.Router) {
		r.With(internal.RateLimitMiddleware(5, time.Minute)).Post("/auth/register", h.Register)
		r.With(internal.RateLimitMiddleware(10, time.Minute)).Post("/auth/login", h.Login)
		r.Post("/auth/refresh", h.RefreshToken)
		r.Get("/health", h.Health)

		r.Get("/auth/oauth/{provider}", oauthHandler.Redirect)
		r.Get("/auth/oauth/{provider}/callback", oauthHandler.Callback)

		r.Post("/mqtt/auth", internal.NewMQTTAuthHandler(pg).ServeHTTP)

		// Swagger UI
		r.Get("/swagger/*", httpSwagger.Handler(
			httpSwagger.URL("/api/v1/swagger/doc.json"),
		))

		r.Get("/ota/check/{key}", otaHandler.CheckOTA)

		r.Group(func(r chi.Router) {
			r.Use(internal.AuthMiddleware(jwt))
			r.Get("/devices", h.ListDevices)
			r.Get("/devices/{key}", h.GetDevice)
			r.Post("/devices/{key}/claim", h.ClaimDevice)
			r.Get("/telemetry/{key}/latest", h.GetLatestTelemetry)
			r.Get("/ws", hub.HandleWS)

			r.Post("/ota/releases", otaHandler.CreateRelease)

			r.Get("/groups", groupHandler.ListGroups)
			r.Post("/groups", groupHandler.CreateGroup)
			r.Post("/groups/{id}/devices/{key}", groupHandler.AddDeviceToGroup)
			r.Delete("/groups/{id}/devices/{key}", groupHandler.RemoveDeviceFromGroup)

			r.Get("/devices/{key}/tags", groupHandler.ListTags)
			r.Post("/devices/{key}/tags/{tag_key}", groupHandler.SetTag)
			r.Delete("/devices/{key}/tags/{tag_key}", groupHandler.DeleteTag)

			r.Get("/search", searchHandler.Search)

			r.Get("/users/me/notifications", h.GetNotificationPrefs)
			r.Patch("/users/me/notifications", h.UpdateNotificationPrefs)

			r.Get("/billing/invoices", billingHandler.ListInvoices)
			r.Post("/billing/invoices", billingHandler.CreateInvoice)
			r.Post("/billing/invoices/{id}/mark-paid", billingHandler.MarkInvoicePaid)

			r.Post("/export/request", exportHandler.RequestExport)
			r.Get("/export/status/{id}", exportHandler.GetExportStatus)

			r.Post("/admin/maintenance", maintenanceMode.ToggleHandler)
		})
	})

	srv := &http.Server{
		Addr:         fmt.Sprintf(":%d", cfg.APIPort),
		Handler:      r,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

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
