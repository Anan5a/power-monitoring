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

// OTAHandler implements firmware-over-the-air release management. Org admins
// publish releases; ESP32 devices poll the check endpoint to discover updates
// and download the binary directly from object storage.
type OTAHandler struct {
	pg          *pgxpool.Pool
	auditor     *Auditor
	publicMinIO string // device-reachable base URL, e.g. https://firmware.example.com
	bucket      string
}

// OTACheckResponse is the body returned to a device polling for updates. Fields
// beyond UpdateAvailable are only populated when an update is available, so the
// firmware can decide whether to fetch the binary and verify its SHA256.
type OTACheckResponse struct {
	UpdateAvailable bool   `json:"update_available"`
	Version         string `json:"version,omitempty"`
	BinaryURL       string `json:"binary_url,omitempty"`
	SHA256          string `json:"sha256,omitempty"`
	BinarySize      int    `json:"binary_size,omitempty"`
}

// NewOTAHandler constructs an OTAHandler backed by the given pool, the
// device-reachable MinIO base URL, and the firmware bucket.
func NewOTAHandler(pg *pgxpool.Pool, publicMinIO, bucket string) *OTAHandler {
	return &OTAHandler{pg: pg, auditor: NewAuditor(pg), publicMinIO: publicMinIO, bucket: bucket}
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

	if h.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}

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
	// Pick the newest stable release for this device type. There is at most
	// one row because the INSERT path enforces (device_type, version)
	// uniqueness; channel is hard-coded to "stable" so beta/edge channels are
	// not offered to production devices.
	err = h.pg.QueryRow(r.Context(), `
		SELECT version, binary_path, sha256, binary_size
		FROM ota_releases
		WHERE device_type = $1 AND channel = 'stable'
		ORDER BY created_at DESC LIMIT 1`,
		deviceType).Scan(&release.Version, &release.BinaryPath, &release.SHA256, &release.BinarySize)
	if err != nil {
		// No release exists yet for this device type — not an error from the
		// device's perspective, just "no update available".
		writeJSON(w, http.StatusOK, OTACheckResponse{UpdateAvailable: false})
		return
	}

	// Only advertise an update if the published version is strictly newer than
	// the device's current version. A missing current_ver means "always offer".
	// semverGreater compares major.minor.patch numerically.
	if currentVer != "" && !semverGreater(release.Version, currentVer) {
		writeJSON(w, http.StatusOK, OTACheckResponse{UpdateAvailable: false})
		return
	}

	writeJSON(w, http.StatusOK, OTACheckResponse{
		UpdateAvailable: true,
		Version:         release.Version,
		// Build a device-reachable URL: publicMinIO is the externally resolvable
		// base (the API process may talk to an internal MinIO endpoint that
		// devices cannot reach). Trim a trailing slash to avoid double slashes.
		BinaryURL:  strings.TrimRight(h.publicMinIO, "/") + "/" + h.bucket + "/" + release.BinaryPath,
		SHA256:     release.SHA256,
		BinarySize: release.BinarySize,
	})
}

// semverGreater reports whether v1 is strictly newer than v2, comparing the
// three numeric components of a semantic version left-to-right. Non-numeric or
// missing components are treated as 0, so "1.2" is equivalent to "1.2.0".
// Pre-release suffixes (e.g. "-rc1") are ignored: this is intentionally simple
// because firmware tags are always plain MAJOR.MINOR.PATCH.
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

// parseSemver splits a version string like "v1.2.3" into a [3]int major,
// minor, patch triple. A leading "v" is stripped. If fewer than three dot
// separated components are present the remaining slots stay 0; extra
// components beyond the third are ignored.
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
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "ota.release.create",
		ResourceType: "ota_release",
		Details:      map[string]any{"device_type": req.DeviceType, "version": req.Version, "channel": req.Channel},
	})
	writeJSON(w, http.StatusCreated, map[string]string{"status": "created"})
}
