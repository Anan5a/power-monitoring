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

type Config struct {
	// Server
	APIPort    int    `env:"API_PORT" default:"8080"`
	IngestPort int    `env:"INGEST_PORT" default:"9090"`
	LogLevel   string `env:"LOG_LEVEL" default:"debug"`

	// Database
	DatabaseURL   string `env:"DATABASE_URL" required:"true"`
	ClickHouseURL string `env:"CLICKHOUSE_URL" required:"true"`

	// MQTT
	MQTTBroker   string `env:"MQTT_BROKER" required:"true"`
	MQTTClientID string `env:"MQTT_CLIENT_ID" default:"iot-platform-ingest"`
	MQTTUser     string `env:"MQTT_USER"`
	MQTTPassword string `env:"MQTT_PASSWORD"`

	// JWT
	JWTSecret     string        `env:"JWT_SECRET" required:"true"`
	JWTAccessTTL  time.Duration `env:"JWT_ACCESS_TTL" default:"15m"`
	JWTRefreshTTL time.Duration `env:"JWT_REFRESH_TTL" default:"720h"`

	// MinIO
	MinIOEndpoint string `env:"MINIO_ENDPOINT" default:"minio:9000"`
	MinIOUser     string `env:"MINIO_ROOT_USER" default:"minioadmin"`
	MINIOPassword string `env:"MINIO_ROOT_PASSWORD" default:"minioadmin"`
	MinIOBucket   string `env:"MINIO_BUCKET" default:"firmware"`

	// SMTP (optional, Phase 2)
	SMTPHost string `env:"SMTP_HOST"`
	SMTPPort int    `env:"SMTP_PORT" default:"587"`
	SMTPUser string `env:"SMTP_USER"`
	SMTPPass string `env:"SMTP_PASS"`
	SMTPFrom string `env:"SMTP_FROM" default:"noreply@iotplatform.local"`

	// OAuth
	GoogleClientID     string `env:"GOOGLE_CLIENT_ID"`
	GoogleClientSecret string `env:"GOOGLE_CLIENT_SECRET"`
	GitHubClientID     string `env:"GITHUB_CLIENT_ID"`
	GitHubClientSecret string `env:"GITHUB_CLIENT_SECRET"`
	BaseURL            string `env:"BASE_URL" default:"http://localhost:8080"`

	// CORS
	CORSAllowedOrigins []string `env:"CORS_ALLOWED_ORIGINS" default:"http://localhost:3000"`

	// Misc
	AutoMigrate bool `env:"AUTO_MIGRATE" default:"true"`
}

// LoadConfig reads environment variables and returns a validated Config.
// Required fields must be set; missing ones return an error listing all of them.
func LoadConfig() (*Config, error) {
	cfg := &Config{}
	var missing []string

	setStr := func(field *string, key, def string) {
		if v := os.Getenv(key); v != "" {
			*field = v
		} else if def != "" {
			*field = def
		}
	}
	setInt := func(field *int, key string, def int) {
		if v := os.Getenv(key); v != "" {
			if i, err := strconv.Atoi(v); err == nil {
				*field = i
			}
		} else {
			*field = def
		}
	}
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
		cfg.CORSAllowedOrigins = strings.Split(v, ",")
	} else {
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
		return nil, fmt.Errorf("JWT_SECRET must be at least 32 characters")
	}

	if len(missing) > 0 {
		return nil, fmt.Errorf("missing required config: %s", strings.Join(missing, ", "))
	}
	return cfg, nil
}
