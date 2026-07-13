// internal/ota.go — OTA firmware release management.
// Orgs create releases; devices poll the check endpoint to find updates.

package internal

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"

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

// CheckOTA is polled by ESP32 devices to check for firmware updates
// @Summary      Check for OTA update
// @Tags         OTA
// @Produce      json
// @Param        key          path  string  true   "Device key"
// @Param        current_ver  query string  false  "Current firmware version"
// @Success      200  {object}  OTACheckResponse
// @Router       /ota/check/{key} [get]
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
		ORDER BY created_at DESC LIMIT 1`,
		deviceType).Scan(&release.Version, &release.BinaryPath, &release.SHA256, &release.BinarySize)
	if err != nil {
		writeJSON(w, http.StatusOK, OTACheckResponse{UpdateAvailable: false})
		return
	}

	if currentVer != "" && !semverGreater(release.Version, currentVer) {
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

func semverGreater(v1, v2 string) bool {
	p1 := parseSemver(v1)
	p2 := parseSemver(v2)
	for i := 0; i < 3; i++ {
		if p1[i] != p2[i] {
			return p1[i] > p2[i]
		}
	}
	return false
}

func parseSemver(v string) [3]int {
	var parts [3]int
	v = strings.TrimLeft(v, "v")
	for i, s := range strings.SplitN(v, ".", 3) {
		if i < 3 {
			n, _ := strconv.Atoi(s)
			parts[i] = n
		}
	}
	return parts
}

// CreateRelease creates a new OTA release (admin only)
// @Summary      Create OTA release
// @Tags         OTA
// @Accept       json
// @Produce      json
// @Param        body  body  CreateReleaseRequest  true  "Release details"
// @Success      201  {object}  map[string]string
// @Failure      409  {object}  APIError
// @Security     BearerAuth
// @Router       /ota/releases [post]
func (h *OTAHandler) CreateRelease(w http.ResponseWriter, r *http.Request) {
	var req CreateReleaseRequest
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
