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

type ExportHandler struct {
	pg       *pgxpool.Pool
	minio    *minio.Client
	bucket   string
	baseURL  string
}

func NewExportHandler(pg *pgxpool.Pool, minio *minio.Client, bucket, baseURL string) *ExportHandler {
	return &ExportHandler{pg: pg, minio: minio, bucket: bucket, baseURL: baseURL}
}

// RequestExport starts an async data export job
// @Summary      Request data export
// @Tags         Export
// @Produce      json
// @Success      202  {object}  ExportRequestResponse
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

	go h.runExport(context.Background(), jobID, userID)

	writeJSON(w, http.StatusAccepted, map[string]string{"job_id": jobID})
}

func (h *ExportHandler) runExport(ctx context.Context, jobID, userID string) {
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

	objectName := fmt.Sprintf("exports/%s.json", jobID)
	if _, err := h.minio.PutObject(ctx, h.bucket, objectName, bytes.NewReader(data), int64(len(data)), minio.PutObjectOptions{ContentType: "application/json"}); err != nil {
		markFailed(err)
		return
	}

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
// @Success      200  {object}  ExportStatusResponse
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
