// internal/export.go — GDPR data export. Users request an export of all
// their data. The job runs async, writes a JSON blob to MinIO, and stores
// the object path in the export_jobs row.

package internal

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/minio/minio-go/v7"
)

// ExportHandler implements GDPR-style data export. A user requests an export;
// the job runs asynchronously, gathers the user's data into a JSON blob, puts
// it into MinIO, and records the object path on the export_jobs row. The user
// later polls status and downloads via a short-lived presigned URL.
type ExportHandler struct {
	pg      *pgxpool.Pool
	minio   *minio.Client
	bucket  string
	auditor *Auditor
}

// exportDownloadExpiry is how long a presigned export download URL stays valid.
// Kept short (1 hour) to limit exposure of exported personal data if the URL
// leaks; the blob itself remains in MinIO and can be re-signed on demand.
const exportDownloadExpiry = 3600 * time.Second

// NewExportHandler constructs an ExportHandler backed by the given pool,
// MinIO client, and bucket. A per-handler Auditor is created for audit logging.
func NewExportHandler(pg *pgxpool.Pool, minio *minio.Client, bucket string) *ExportHandler {
	return &ExportHandler{pg: pg, minio: minio, bucket: bucket, auditor: NewAuditor(pg)}
}

// RequestExport starts an async data export job
// @Summary      Request data export
// @Tags         Export
// @Produce      json
// @Success      202  {object}  map[string]string
// @Security     BearerAuth
// @Router       /export/request [post]
func (h *ExportHandler) RequestExport(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	if h.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}
	if h.minio == nil {
		writeError(w, "internal_error", "object storage unavailable", http.StatusInternalServerError)
		return
	}

	var jobID string
	if err := h.pg.QueryRow(r.Context(),
		`INSERT INTO export_jobs (user_id, status, expires_at)
		 VALUES ($1, 'pending', now() + interval '7 days') RETURNING id`,
		userID).Scan(&jobID); err != nil {
		writeError(w, "internal_error", "failed to create export job", http.StatusInternalServerError)
		return
	}

	// Detach from the request context: the job must keep running after the
	// HTTP response is sent, so we use a fresh background context rather than
	// r.Context() which would be cancelled when the handler returns.
	go h.runExport(context.Background(), jobID, userID)

	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "export.request",
		ResourceType: "export_job",
		ResourceID:   jobID,
	})

	writeJSON(w, http.StatusAccepted, map[string]string{"job_id": jobID})
}

// runExport performs the actual data gathering and MinIO upload. It runs on a
// background goroutine detached from any HTTP request. Any failure is recorded
// by flipping the export_jobs row to "failed" with the error message, so the
// user can see why their export never became ready.
func (h *ExportHandler) runExport(ctx context.Context, jobID, userID string) {
	// markFailed is the single failure sink: log + persist the error on the
	// job row so GetExportStatus can surface it to the user.
	markFailed := func(err error) {
		slog.Error("export failed", "job", jobID, "error", err)
		if h.pg != nil {
			h.pg.Exec(ctx, `UPDATE export_jobs SET status = 'failed', error = $2 WHERE id = $1`, jobID, err.Error())
		}
	}

	if h.pg == nil || h.minio == nil {
		markFailed(fmt.Errorf("export dependencies unavailable"))
		return
	}

	if _, err := h.pg.Exec(ctx, `UPDATE export_jobs SET status = 'running' WHERE id = $1`, jobID); err != nil {
		markFailed(err)
		return
	}

	var user struct {
		Email       string    `json:"email"`
		DisplayName string    `json:"display_name"`
		CreatedAt   time.Time `json:"created_at"`
	}
	if err := h.pg.QueryRow(ctx,
		`SELECT email, display_name, created_at FROM users WHERE id = $1`, userID).
		Scan(&user.Email, &user.DisplayName, &user.CreatedAt); err != nil {
		markFailed(err)
		return
	}

	rows, err := h.pg.Query(ctx,
		`SELECT device_key, device_name, device_type, created_at FROM devices WHERE owner_id = $1`, userID)
	if err != nil {
		markFailed(err)
		return
	}
	type DeviceExport struct {
		Key       string    `json:"key"`
		Name      string    `json:"name"`
		Type      string    `json:"type"`
		CreatedAt time.Time `json:"created_at"`
	}
	devices := []DeviceExport{}
	for rows.Next() {
		var d DeviceExport
		rows.Scan(&d.Key, &d.Name, &d.Type, &d.CreatedAt)
		devices = append(devices, d)
	}
	rows.Close()

	export := map[string]any{
		"user":        user,
		"devices":     devices,
		"exported_at": time.Now().UTC(),
	}

	data, err := json.MarshalIndent(export, "", "  ")
	if err != nil {
		markFailed(err)
		return
	}

	// Object name is derived from jobID only, so each export is immutable and
	// retry-safe (a re-run produces a new jobID → new object, never overwrites).
	objectName := fmt.Sprintf("exports/%s.json", jobID)
	if _, err := h.minio.PutObject(ctx, h.bucket, objectName, bytes.NewReader(data), int64(len(data)), minio.PutObjectOptions{ContentType: "application/json"}); err != nil {
		markFailed(err)
		return
	}

	// Flip the job to "ready" and persist the object path + row count. Only
	// after this commit does DownloadExport become able to serve the blob.
	if _, err := h.pg.Exec(ctx,
		`UPDATE export_jobs SET status = 'ready', file_path = $2, row_count = $3, completed_at = now()
		 WHERE id = $1`,
		jobID, objectName, len(devices)); err != nil {
		markFailed(err)
		return
	}
	slog.Info("export ready", "job", jobID, "path", objectName)
}

// GetExportStatus returns the status of an export job
// @Summary      Get export job status
// @Tags         Export
// @Produce      json
// @Param        id  path  string  true  "Export job ID"
// @Success      200  {object}  map[string]any
// @Failure      404  {object}  APIError
// @Security     BearerAuth
// @Router       /export/status/{id} [get]
func (h *ExportHandler) GetExportStatus(w http.ResponseWriter, r *http.Request) {
	jobID := chi.URLParam(r, "id")
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	if h.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}

	var status, filePath string
	var completedAt *time.Time
	err := h.pg.QueryRow(r.Context(),
		`SELECT status, file_path, completed_at FROM export_jobs WHERE id = $1 AND user_id = $2`,
		jobID, userID).Scan(&status, &filePath, &completedAt)
	if err != nil {
		writeError(w, "not_found", "export job not found", http.StatusNotFound)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"status":       status,
		"file_path":    filePath,
		"completed_at": completedAt,
	})
}

// DownloadExport returns a presigned URL to download a ready export.
// @Summary      Download export
// @Tags         Export
// @Produce      json
// @Param        id  path  string  true  "Export job ID"
// @Success      200  {object}  ExportDownloadResponse
// @Failure      400  {object}  APIError
// @Failure      404  {object}  APIError
// @Security     BearerAuth
// @Router       /export/download/{id} [get]
func (h *ExportHandler) DownloadExport(w http.ResponseWriter, r *http.Request) {
	jobID := chi.URLParam(r, "id")
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	if h.pg == nil || h.minio == nil {
		writeError(w, "internal_error", "service unavailable", http.StatusInternalServerError)
		return
	}

	var filePath string
	var status string
	// Scoped by user_id so users cannot probe other users' job IDs (defends
	// against IDOR): a foreign job ID simply yields "not found".
	err := h.pg.QueryRow(r.Context(),
		`SELECT status, file_path FROM export_jobs WHERE id = $1 AND user_id = $2`,
		jobID, userID).Scan(&status, &filePath)
	if err != nil {
		writeError(w, "not_found", "export job not found", http.StatusNotFound)
		return
	}
	if status != "ready" || filePath == "" {
		// Not yet ready (pending/running/failed) — tell the caller to keep polling.
		writeError(w, "bad_request", "export is not ready", http.StatusBadRequest)
		return
	}

	// Generate a short-lived presigned GET URL so the client downloads the
	// blob directly from MinIO without proxying through the API. Expiry is
	// bounded by exportDownloadExpiry to limit exposure of personal data.
	url, err := h.minio.PresignedGetObject(r.Context(), h.bucket, filePath, exportDownloadExpiry, nil)
	if err != nil {
		writeError(w, "internal_error", "failed to generate download URL", http.StatusInternalServerError)
		return
	}

	writeJSON(w, http.StatusOK, ExportDownloadResponse{
		DownloadURL:      url.String(),
		ExpiresInSeconds: int(exportDownloadExpiry.Seconds()),
	})
}
