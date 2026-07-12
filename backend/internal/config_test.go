// internal/config_test.go — Tests for config loading from environment variables.

package internal

import (
	"os"
	"testing"
	"time"
)

func TestLoadConfig_Defaults(t *testing.T) {
	os.Setenv("DATABASE_URL", "postgres://localhost:5432/test")
	os.Setenv("CLICKHOUSE_URL", "clickhouse://localhost:9000")
	os.Setenv("MQTT_BROKER", "tcp://localhost:1883")
	os.Setenv("JWT_SECRET", "test-secret-32-chars-minimum-for-hs256")

	cfg, err := LoadConfig()
	if err != nil {
		t.Fatalf("LoadConfig() error = %v", err)
	}
	if cfg.APIPort != 8080 {
		t.Errorf("APIPort = %d, want 8080", cfg.APIPort)
	}
	if cfg.JWTAccessTTL != 15*time.Minute {
		t.Errorf("JWTAccessTTL = %v, want 15m", cfg.JWTAccessTTL)
	}
	if cfg.CORSAllowedOrigins[0] != "http://localhost:3000" {
		t.Errorf("CORSAllowedOrigins = %v, want [http://localhost:3000]", cfg.CORSAllowedOrigins)
	}
}

func TestLoadConfig_MissingRequired(t *testing.T) {
	// Clear all env vars
	for _, k := range []string{"DATABASE_URL", "CLICKHOUSE_URL", "MQTT_BROKER", "JWT_SECRET"} {
		os.Unsetenv(k)
	}
	_, err := LoadConfig()
	if err == nil {
		t.Fatal("LoadConfig() expected error for missing required fields")
	}
}

func TestLoadConfig_ShortJWTSecret(t *testing.T) {
	os.Setenv("DATABASE_URL", "postgres://localhost:5432/test")
	os.Setenv("CLICKHOUSE_URL", "clickhouse://localhost:9000")
	os.Setenv("MQTT_BROKER", "tcp://localhost:1883")
	os.Setenv("JWT_SECRET", "short")

	_, err := LoadConfig()
	if err == nil {
		t.Fatal("LoadConfig() expected error for short JWT_SECRET")
	}
}
