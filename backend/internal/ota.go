// internal/ota.go — OTA firmware release management.
// Orgs create releases; devices poll the check endpoint to find updates.

package internal

import (
	"encoding/json"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type OTAHandler struct {
	pg *pgxpool.Pool
}

type OTACheckResponse struct {
	UpdateAvailable bool   `json:"update_available"`
	Version         string `json:"version,omitempty"`
	BinaryURL       string `json:"binary_url,omitempty"`
	SHA256          string `json:"sha256,omitempty"`
	BinarySize      int    `json:"binary_size,omitempty"`
}

func NewOTAHandler(pg *pgxpool.Pool) *OTAHandler {
	return &OTAHandler{pg: pg}
}

// CheckOTA is polled by ESP32: GET /api/v1/ota/check/{key}?current_ver=2.0.0
func (h *OTAHandler) CheckOTA(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	currentVer := r.URL.Query().Get("current_ver")

	var deviceType string
	err := h.pg.QueryRow(r.Context(),
		`SELECT device_type FROM devices WHERE device_key = $1 AND is_active = true`,
		deviceKey).Scan(&deviceType)
	if err != nil {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	var release struct {
		Version    string
		BinaryPath string
		SHA256     string
		BinarySize int
	}
	err = h.pg.QueryRow(r.Context(), `
		SELECT version, binary_path, sha256, binary_size
		FROM ota_releases
		WHERE device_type = $1 AND channel = 'stable'
		  AND version > $2
		ORDER BY created_at DESC LIMIT 1`,
		deviceType, currentVer).Scan(&release.Version, &release.BinaryPath, &release.SHA256, &release.BinarySize)
	if err != nil {
		writeJSON(w, http.StatusOK, OTACheckResponse{UpdateAvailable: false})
		return
	}

	writeJSON(w, http.StatusOK, OTACheckResponse{
		UpdateAvailable: true,
		Version:         release.Version,
		BinaryURL:       "http://minio:9000/firmware/" + release.BinaryPath,
		SHA256:          release.SHA256,
		BinarySize:      release.BinarySize,
	})
}

// CreateRelease is an admin endpoint: POST /api/v1/ota/releases
func (h *OTAHandler) CreateRelease(w http.ResponseWriter, r *http.Request) {
	var req struct {
		DeviceType string `json:"device_type"`
		Version    string `json:"version"`
		Channel    string `json:"channel"`
		BinaryPath string `json:"binary_path"`
		BinarySize int    `json:"binary_size"`
		SHA256     string `json:"sha256"`
		Changelog  string `json:"changelog"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}

	_, err := h.pg.Exec(r.Context(), `
		INSERT INTO ota_releases (device_type, version, channel, binary_path, binary_size, sha256, changelog)
		VALUES ($1, $2, $3, $4, $5, $6, $7)`,
		req.DeviceType, req.Version, req.Channel, req.BinaryPath, req.BinarySize, req.SHA256, req.Changelog)
	if err != nil {
		writeError(w, "conflict", "release version already exists", http.StatusConflict)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]string{"status": "created"})
}
