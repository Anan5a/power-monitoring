// internal/config.go — Loads configuration from environment variables.
// All config is validated at startup; missing required vars cause a fatal error.

package internal

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config holds all runtime configuration loaded from environment variables by
// LoadConfig. Required fields cause startup to fail when missing; optional
// fields fall back to the defaults encoded in the env tags.
type Config struct {
	// Server
	// APIPort is the TCP port the public REST API (cmd/api) listens on.
	APIPort int `env:"API_PORT" default:"8080"`
	// IngestPort is the TCP port the MQTT/ingest worker (cmd/ingest) listens on.
	IngestPort int `env:"INGEST_PORT" default:"9090"`
	// LogLevel controls slog verbosity (debug/info/warn/error).
	LogLevel string `env:"LOG_LEVEL" default:"debug"`

	// Database
	// DatabaseURL is the PostgreSQL DSN shared by both binaries.
	DatabaseURL string `env:"DATABASE_URL" required:"true"`
	// ClickHouseURL is the ClickHouse DSN used for telemetry storage.
	ClickHouseURL string `env:"CLICKHOUSE_URL" required:"true"`

	// MQTT
	// MQTTBroker is the broker address the ingest worker subscribes to.
	MQTTBroker string `env:"MQTT_BROKER" required:"true"`
	// MQTTClientID identifies the ingest client on the broker; must be unique
	// per running ingest process to avoid session takeover.
	MQTTClientID string `env:"MQTT_CLIENT_ID" default:"iot-platform-ingest"`
	// MQTTUser is the optional username for broker auth.
	MQTTUser string `env:"MQTT_USER"`
	// MQTTPassword is the optional password for broker auth.
	MQTTPassword string `env:"MQTT_PASSWORD"`

	// JWT
	// JWTSecret signs access and refresh tokens; must be >=32 chars and not
	// the shipped example placeholder.
	JWTSecret string `env:"JWT_SECRET" required:"true"`
	// JWTAccessTTL is the lifetime of short-lived access tokens.
	JWTAccessTTL time.Duration `env:"JWT_ACCESS_TTL" default:"15m"`
	// JWTRefreshTTL is the lifetime of long-lived refresh tokens.
	JWTRefreshTTL time.Duration `env:"JWT_REFRESH_TTL" default:"720h"`

	// MinIO
	// MinIOEndpoint is the internal host:port used by the API to upload firmware.
	MinIOEndpoint string `env:"MINIO_ENDPOINT" default:"minio:9000"`
	// MinIOUser is the MinIO root user (access key).
	MinIOUser string `env:"MINIO_ROOT_USER" default:"minioadmin"`
	// MINIOPassword is the MinIO root password (secret key).
	MINIOPassword string `env:"MINIO_ROOT_PASSWORD" default:"minioadmin"`
	// MinIOBucket is the S3 bucket holding firmware binaries.
	MinIOBucket string `env:"MINIO_BUCKET" default:"firmware"`
	// MinIOPublicURL is the device-reachable base URL for firmware downloads
	// (the internal MinIOEndpoint is not reachable from devices). Must be set
	// in any deployment where OTA is used.
	MinIOPublicURL string `env:"MINIO_PUBLIC_URL" default:"http://localhost:9002"`

	// SMTP (optional, Phase 2)
	// SMTPHost is the outbound mail server hostname; empty disables email.
	SMTPHost string `env:"SMTP_HOST"`
	// SMTPPort is the SMTP port (typically 587 for STARTTLS).
	SMTPPort int `env:"SMTP_PORT" default:"587"`
	// SMTPUser is the optional SMTP auth username.
	SMTPUser string `env:"SMTP_USER"`
	// SMTPPass is the optional SMTP auth password.
	SMTPPass string `env:"SMTP_PASS"`
	// SMTPFrom is the From address on outgoing messages.
	SMTPFrom string `env:"SMTP_FROM" default:"noreply@iotplatform.local"`

	// OAuth
	// GoogleClientID is the Google OAuth client ID; empty disables Google login.
	GoogleClientID string `env:"GOOGLE_CLIENT_ID"`
	// GoogleClientSecret is the Google OAuth client secret.
	GoogleClientSecret string `env:"GOOGLE_CLIENT_SECRET"`
	// GitHubClientID is the GitHub OAuth client ID; empty disables GitHub login.
	GitHubClientID string `env:"GITHUB_CLIENT_ID"`
	// GitHubClientSecret is the GitHub OAuth client secret.
	GitHubClientSecret string `env:"GITHUB_CLIENT_SECRET"`
	// BaseURL is the externally reachable API base, used for OAuth callbacks.
	BaseURL string `env:"BASE_URL" default:"http://localhost:8080"`

	// CORS
	// CORSAllowedOrigins is the allowlist for browser origins; wildcard and
	// "null" entries are stripped because they are unsafe with credentials.
	CORSAllowedOrigins []string `env:"CORS_ALLOWED_ORIGINS" default:"http://localhost:3000"`

	// Misc
	// AutoMigrate runs schema migrations on startup when true.
	AutoMigrate bool `env:"AUTO_MIGRATE" default:"true"`
}

// LoadConfig reads environment variables and returns a validated Config.
// Required fields must be set; missing ones return an error listing all of them.
func LoadConfig() (*Config, error) {
	cfg := &Config{}
	var missing []string

	// setStr assigns an env value, falling back to def when unset. A required
	// field is expressed by passing an empty def and validating later.
	setStr := func(field *string, key, def string) {
		if v := os.Getenv(key); v != "" {
			*field = v
		} else if def != "" {
			*field = def
		}
	}
	// setInt parses an int env var; an invalid value leaves the field at its
	// zero value rather than failing startup, so misconfigured ports degrade
	// to defaults handled by the caller.
	setInt := func(field *int, key string, def int) {
		if v := os.Getenv(key); v != "" {
			if i, err := strconv.Atoi(v); err == nil {
				*field = i
			}
		} else {
			*field = def
		}
	}
	// setDuration parses a Go duration string (e.g. "15m"); invalid values
	// silently keep the zero value and rely on later validation.
	setDuration := func(field *time.Duration, key string, def time.Duration) {
		if v := os.Getenv(key); v != "" {
			if d, err := time.ParseDuration(v); err == nil {
				*field = d
			}
		} else {
			*field = def
		}
	}

	setInt(&cfg.APIPort, "API_PORT", 8080)
	setInt(&cfg.IngestPort, "INGEST_PORT", 9090)
	setStr(&cfg.LogLevel, "LOG_LEVEL", "debug")
	setStr(&cfg.DatabaseURL, "DATABASE_URL", "")
	setStr(&cfg.ClickHouseURL, "CLICKHOUSE_URL", "")
	setStr(&cfg.MQTTBroker, "MQTT_BROKER", "")
	setStr(&cfg.MQTTClientID, "MQTT_CLIENT_ID", "iot-platform-ingest")
	setStr(&cfg.MQTTUser, "MQTT_USER", "")
	setStr(&cfg.MQTTPassword, "MQTT_PASSWORD", "")
	setStr(&cfg.JWTSecret, "JWT_SECRET", "")
	setDuration(&cfg.JWTAccessTTL, "JWT_ACCESS_TTL", 15*time.Minute)
	setDuration(&cfg.JWTRefreshTTL, "JWT_REFRESH_TTL", 720*time.Hour)
	setStr(&cfg.MinIOEndpoint, "MINIO_ENDPOINT", "minio:9000")
	setStr(&cfg.MinIOUser, "MINIO_ROOT_USER", "minioadmin")
	setStr(&cfg.MINIOPassword, "MINIO_ROOT_PASSWORD", "minioadmin")
	setStr(&cfg.MinIOBucket, "MINIO_BUCKET", "firmware")
	setStr(&cfg.MinIOPublicURL, "MINIO_PUBLIC_URL", "http://localhost:9002")
	setStr(&cfg.SMTPHost, "SMTP_HOST", "")
	setInt(&cfg.SMTPPort, "SMTP_PORT", 587)
	setStr(&cfg.SMTPUser, "SMTP_USER", "")
	setStr(&cfg.SMTPPass, "SMTP_PASS", "")
	setStr(&cfg.SMTPFrom, "SMTP_FROM", "noreply@iotplatform.local")
	setStr(&cfg.GoogleClientID, "GOOGLE_CLIENT_ID", "")
	setStr(&cfg.GoogleClientSecret, "GOOGLE_CLIENT_SECRET", "")
	setStr(&cfg.GitHubClientID, "GITHUB_CLIENT_ID", "")
	setStr(&cfg.GitHubClientSecret, "GITHUB_CLIENT_SECRET", "")
	setStr(&cfg.BaseURL, "BASE_URL", "http://localhost:8080")

	if v := os.Getenv("AUTO_MIGRATE"); v != "" {
		cfg.AutoMigrate = v == "true"
	} else {
		cfg.AutoMigrate = true
	}

	if v := os.Getenv("CORS_ALLOWED_ORIGINS"); v != "" {
		for _, o := range strings.Split(v, ",") {
			o = strings.TrimSpace(o)
			if o == "" || o == "*" || o == "null" {
				// Wildcard/null are unsafe with AllowCredentials; skip them.
				continue
			}
			cfg.CORSAllowedOrigins = append(cfg.CORSAllowedOrigins, o)
		}
	}
	if len(cfg.CORSAllowedOrigins) == 0 {
		cfg.CORSAllowedOrigins = []string{"http://localhost:3000"}
	}

	if cfg.DatabaseURL == "" {
		missing = append(missing, "DATABASE_URL")
	}
	if cfg.ClickHouseURL == "" {
		missing = append(missing, "CLICKHOUSE_URL")
	}
	if cfg.MQTTBroker == "" {
		missing = append(missing, "MQTT_BROKER")
	}
	if cfg.JWTSecret == "" {
		missing = append(missing, "JWT_SECRET")
	}
	if len(cfg.JWTSecret) < 32 {
		// Weak secret rejection: a short HMAC key is brute-forceable and would
		// let an attacker forge access tokens. Require >=32 chars upfront.
		return nil, fmt.Errorf("JWT_SECRET must be at least 32 characters")
	}
	// Reject the exact placeholder shipped in the old .env.example so it cannot
	// accidentally sign production tokens. Replace with a random secret.
	if cfg.JWTSecret == "change-me-to-a-random-64-char-string" {
		return nil, fmt.Errorf("JWT_SECRET is still the example placeholder; generate a random 64-character secret")
	}

	if len(missing) > 0 {
		// Aggregate all missing required keys into one error so the operator
		// sees the complete list rather than fixing them one at a time.
		return nil, fmt.Errorf("missing required config: %s", strings.Join(missing, ", "))
	}
	return cfg, nil
}
