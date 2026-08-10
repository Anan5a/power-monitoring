// Command api serves the IoT platform's HTTP API.
//
// It is the public-facing API server entry point: it wires the HTTP router,
// middleware chain, handlers, the WebSocket hub, and the MQTT live/# and
// status/+/online subscribers that fan live telemetry out to connected
// WebSocket clients and the alert engine. It starts an HTTP server on the
// configured APIPort and performs graceful shutdown on SIGINT/SIGTERM.

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

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/go-chi/chi/v5"
	chimw "github.com/go-chi/chi/v5/middleware"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	httpSwagger "github.com/swaggo/http-swagger"
	_ "go.uber.org/automaxprocs"

	_ "github.com/Anan5a/iot-platform/docs/api"
	"github.com/Anan5a/iot-platform/internal"
)

func main() {
	// Load configuration from environment (.env in dev, real env in prod).
	cfg, err := internal.LoadConfig()
	if err != nil {
		slog.Error("config", "error", err)
		os.Exit(1)
	}

	// Enable debug-level logs from the stdlib log bridge (e.g. driver logs).
	slog.SetLogLoggerLevel(slog.LevelDebug)

	ctx := context.Background()
	// Postgres holds relational state: users, devices, groups, commands, audit.
	pg, err := internal.ConnectPG(ctx, cfg.DatabaseURL)
	if err != nil {
		slog.Error("postgres", "error", err)
		os.Exit(1)
	}
	defer pg.Close()

	// ClickHouse is the time-series store for telemetry reads (latest/latest).
	ch, err := internal.ConnectCH(ctx, cfg.ClickHouseURL)
	if err != nil {
		slog.Error("clickhouse", "error", err)
		os.Exit(1)
	}
	defer ch.Close()

	// JWT manager mints/validates access + refresh tokens; refresh store
	// persists refresh tokens in Postgres so they can be revoked/rotated.
	jwt := internal.NewJWTManager(cfg.JWTSecret, cfg.JWTAccessTTL, cfg.JWTRefreshTTL)
	refreshStore := internal.NewRefreshTokenStore(pg, jwt, cfg.JWTRefreshTTL)

	// Email service queues outbound mail in Postgres; a background goroutine
	// drains the queue and sends via SMTP so request handlers never block on SMTP.
	emailSvc := internal.NewEmailService(pg, cfg.SMTPFrom, cfg.SMTPHost, cfg.SMTPPort, cfg.SMTPUser, cfg.SMTPPass)
	go emailSvc.DrainLoop(ctx)

	// Alert engine evaluates telemetry thresholds and sends notifications via emailSvc.
	alertEngine := internal.NewAlertEngine(pg, emailSvc)

	// WebSocket hub fans live MQTT messages out to connected browsers.
	hub := internal.NewWebSocketHub(pg)
	// Status manager tracks online/offline state from status/+/online and
	// is wired to the hub so status changes are pushed to subscribers.
	statusMgr := internal.NewDeviceStatusManager(pg, hub)
	// Background loop marks devices stale/online based on last-seen timestamps.
	statusMgr.StartStalenessChecker(ctx)

	// MQTT subscriber for live telemetry + device status. OnConnect is used so
	// that subscriptions are re-established automatically after any reconnect
	// (paho fires OnConnect on every successful (re)connection). QoS 0 is fine
	// here because the API server only mirrors data to browsers; the durable
	// path is handled by the ingest worker on telemetry/# at QoS 1.
	mqttOpts := mqtt.NewClientOptions()
	mqttOpts.AddBroker(cfg.MQTTBroker)
	// Fixed client ID: only one API server instance should connect with this
	// name; sessions are clean so no stale subscriptions persist across restarts.
	mqttOpts.SetClientID("iot-platform-api")
	mqttOpts.SetCleanSession(true)
	if cfg.MQTTUser != "" {
		mqttOpts.SetUsername(cfg.MQTTUser)
		mqttOpts.SetPassword(cfg.MQTTPassword)
	}
	mqttOpts.OnConnect = func(c mqtt.Client) {
		slog.Info("MQTT connected (api)")
		// live/# carries enriched telemetry published by the ingest worker.
		// Fan out to WebSocket clients and evaluate threshold rules. The token
		// error check is required because paho Subscribe is async; without
		// Wait()+Error() a failed subscription would be silently lost.
		if token := c.Subscribe("live/#", 0, func(_ mqtt.Client, msg mqtt.Message) {
			hub.OnLiveMessage(msg.Topic(), msg.Payload())
			var enriched internal.EnrichedTelemetry
			if err := json.Unmarshal(msg.Payload(), &enriched); err == nil {
				alertEngine.Evaluate(context.Background(), enriched.DeviceKey, &enriched)
			}
		}); token.Wait() && token.Error() != nil {
			slog.Error("subscribe live/#", "error", token.Error())
		}
		// status/+/online is published by devices on connect; update the
		// status manager which in turn notifies WebSocket subscribers.
		if token := c.Subscribe("status/+/online", 0, func(_ mqtt.Client, msg mqtt.Message) {
			statusMgr.HandleStatusMessage(msg.Topic(), msg.Payload())
		}); token.Wait() && token.Error() != nil {
			slog.Error("subscribe status/+/online", "error", token.Error())
		}
	}
	mqttClient := mqtt.NewClient(mqttOpts)
	// Block on connect so we never start serving HTTP without the MQTT fan-out wired.
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		slog.Error("mqtt connect", "error", token.Error())
		os.Exit(1)
	}
	defer mqttClient.Disconnect(1000)

	// Construct handlers. Each handler owns its subset of routes below.
	h := internal.NewHandlers(pg, jwt, ch, refreshStore)
	otaHandler := internal.NewOTAHandler(pg, cfg.MinIOPublicURL, cfg.MinIOBucket)
	groupHandler := internal.NewGroupHandler(pg)
	searchHandler := internal.NewSearchHandler(pg)
	// OAuth handler wires Google + GitHub providers against the configured
	// client secrets; BaseURL is used to build the redirect URI.
	oauthHandler := internal.NewOAuthHandler(pg, jwt, refreshStore, cfg.GoogleClientID, cfg.GoogleClientSecret, cfg.GitHubClientID, cfg.GitHubClientSecret, cfg.BaseURL)
	billingHandler := internal.NewBillingHandler(pg)
	commandHandler := internal.NewCommandHandler(pg)

	// MinIO object store for large artifacts: OTA firmware binaries and exports.
	minioClient, err := minio.New(cfg.MinIOEndpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(cfg.MinIOUser, cfg.MINIOPassword, ""),
		Secure: false,
	})
	if err != nil {
		slog.Error("minio", "error", err)
		os.Exit(1)
	}

	exportHandler := internal.NewExportHandler(pg, minioClient, cfg.MinIOBucket)
	// Maintenance mode is backed by a DB flag; the middleware below short-
	// circuits non-admin requests with 503 while it is enabled.
	maintenanceMode := internal.NewMaintenanceMode(pg)

	r := chi.NewRouter()
	// Middleware order matters: RequestID first so every downstream logger
	// line carries a request id, then RealIP to rewrite X-Forwarded-For into
	// RemoteAddr before logging, then our structured logger. CORS must come
	// before body-size limiting and maintenance-mode checks so preflight
	// OPTIONS requests succeed even while the service is degraded. Recoverer
	// last so a panic is logged with request context and then recovered.
	r.Use(chimw.RequestID)
	r.Use(chimw.RealIP)
	r.Use(internal.LoggerMiddleware)
	r.Use(internal.CORSMiddleware(cfg.CORSAllowedOrigins))
	r.Use(internal.MaxBodySize)
	r.Use(maintenanceMode.Middleware())
	r.Use(chimw.Recoverer)

	r.Route("/api/v1", func(r chi.Router) {
		// Public auth endpoints with per-route rate limiting: registration is
		// more aggressively throttled (5/min) than login (10/min) to deter
		// account-creation abuse. Refresh is un-rate-limited because the
		// refresh token itself is the rate limiter (single-use, rotating).
		r.With(internal.RateLimitMiddleware(5, time.Minute)).Post("/auth/register", h.Register)
		r.With(internal.RateLimitMiddleware(10, time.Minute)).Post("/auth/login", h.Login)
		r.Post("/auth/refresh", h.RefreshToken)
		r.Get("/health", h.Health)

		// OAuth login flows: redirect to provider and handle the callback.
		r.Get("/auth/oauth/{provider}", oauthHandler.Redirect)
		r.Get("/auth/oauth/{provider}/callback", oauthHandler.Callback)

		// MQTT broker authentication endpoint (mosquitto auth hook target).
		r.Post("/mqtt/auth", internal.NewMQTTAuthHandler(pg).ServeHTTP)

		// Swagger UI
		r.Get("/swagger/*", httpSwagger.Handler(
			httpSwagger.URL("/api/v1/swagger/doc.json"),
		))

		// Device-facing OTA check (no auth: device key in path identifies the device).
		r.Get("/ota/check/{key}", otaHandler.CheckOTA)

		// Firmware-facing command endpoints. DeviceAuthMiddleware validates the
		// device's API key; a user Bearer token is also accepted (handlers check
		// both). Mounted outside AuthMiddleware so devices can poll without a JWT.
		r.With(internal.DeviceAuthMiddleware(pg)).Get("/commands/{key}/pending", commandHandler.GetPendingCommands)
		r.With(internal.DeviceAuthMiddleware(pg)).Post("/commands/{id}/result", commandHandler.UpdateCommandResult)

		// Public billing plan catalog.
		r.Get("/billing/plans", h.ListPlans)

		// Authenticated routes: AuthMiddleware verifies the access JWT and
		// injects the user identity into the request context. AdminOnly is
		// layered per-route for privileged mutations rather than globally.
		r.Group(func(r chi.Router) {
			r.Use(internal.AuthMiddleware(jwt))
			r.Get("/devices", h.ListDevices)
			r.Get("/devices/{key}", h.GetDevice)
			r.Post("/devices/{key}/claim", h.ClaimDevice)
			r.Get("/telemetry/{key}/latest", h.GetLatestTelemetry)
			// WebSocket upgrade; hub.HandleWS registers the connection and
			// pumps live messages received via hub.OnLiveMessage to it.
			r.Get("/ws", hub.HandleWS)

			r.With(internal.AdminOnly).Post("/ota/releases", otaHandler.CreateRelease)

			// Group + tag management (grouping is relational, hence Postgres).
			r.Get("/groups", groupHandler.ListGroups)
			r.Post("/groups", groupHandler.CreateGroup)
			r.Post("/groups/{id}/devices/{key}", groupHandler.AddDeviceToGroup)
			r.Delete("/groups/{id}/devices/{key}", groupHandler.RemoveDeviceFromGroup)

			// Per-device free-form tags keyed by tag_key.
			r.Get("/devices/{key}/tags", groupHandler.ListTags)
			r.Post("/devices/{key}/tags/{tag_key}", groupHandler.SetTag)
			r.Delete("/devices/{key}/tags/{tag_key}", groupHandler.DeleteTag)

			// Full-text / metadata search across owned devices.
			r.Get("/search", searchHandler.Search)

			// Per-user notification preference toggles.
			r.Get("/users/me/notifications", h.GetNotificationPrefs)
			r.Patch("/users/me/notifications", h.UpdateNotificationPrefs)

			// Billing: invoices are visible to any authed user, but creation
			// and state changes are admin-only.
			r.Get("/billing/invoices", billingHandler.ListInvoices)
			r.With(internal.AdminOnly).Post("/billing/invoices", billingHandler.CreateInvoice)
			r.With(internal.AdminOnly).Post("/billing/invoices/{id}/mark-paid", billingHandler.MarkInvoicePaid)

			// User-initiated command enqueue for a owned device.
			r.Post("/commands", commandHandler.CreateCommand)

			// Export: request an async export job, poll for status, then
			// download the produced artifact from MinIO.
			r.Post("/export/request", exportHandler.RequestExport)
			r.Get("/export/status/{id}", exportHandler.GetExportStatus)
			r.Get("/export/download/{id}", exportHandler.DownloadExport)

			// Audit log listing (read access for any authed user; admin-only
			// toggle flips the maintenance flag used by the middleware above).
			r.Get("/admin/audit", h.ListAudit)
			r.With(internal.AdminOnly).Post("/admin/maintenance", maintenanceMode.ToggleHandler)
		})
	})

	// Server timeouts: ReadTimeout bounds slow clients during request reads,
	// WriteTimeout covers the response write (longer to allow SSE/WS-style
	// flows), IdleTimeout reaps keep-alive connections to avoid fd leaks.
	srv := &http.Server{
		Addr:         fmt.Sprintf(":%d", cfg.APIPort),
		Handler:      r,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Graceful shutdown goroutine: wait for SIGINT/SIGTERM, then give
	// in-flight requests up to 10s to finish before the server returns and
	// main() proceeds to the deferred close calls (pg/ch/mqtt).
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
	// ListenAndServe blocks until Shutdown is called (returning ErrServerClosed);
	// any other error is fatal.
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		slog.Error("server", "error", err)
		os.Exit(1)
	}
}
