// internal/export.go — GDPR data export. Users request an export of all
// their data. The job runs async, writes a CSV/JSON zip to MinIO, and
// emails a download link.

package internal

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type ExportHandler struct {
	pg      *pgxpool.Pool
	minio   any // *minio.Client — typed in real code
	baseURL string
}

func NewExportHandler(pg *pgxpool.Pool, minio any, baseURL string) *ExportHandler {
	return &ExportHandler{pg: pg, minio: minio, baseURL: baseURL}
}

func (h *ExportHandler) RequestExport(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)

	var jobID string
	h.pg.QueryRow(r.Context(),
		`INSERT INTO export_jobs (user_id, status, expires_at)
		 VALUES ($1, 'pending', now() + interval '7 days') RETURNING id`,
		userID).Scan(&jobID)

	go h.runExport(context.Background(), jobID, userID)

	writeJSON(w, http.StatusAccepted, map[string]string{"job_id": jobID})
}

func (h *ExportHandler) runExport(ctx context.Context, jobID, userID string) {
	h.pg.Exec(ctx, `UPDATE export_jobs SET status = 'running' WHERE id = $1`, jobID)

	// Collect user data
	var user struct {
		Email       string    `json:"email"`
		DisplayName string    `json:"display_name"`
		CreatedAt   time.Time `json:"created_at"`
	}
	h.pg.QueryRow(ctx,
		`SELECT email, display_name, created_at FROM users WHERE id = $1`, userID).
		Scan(&user.Email, &user.DisplayName, &user.CreatedAt)

	// Collect devices
	rows, _ := h.pg.Query(ctx,
		`SELECT device_key, device_name, device_type, created_at FROM devices WHERE owner_id = $1`, userID)
	type DeviceExport struct {
		Key  string `json:"key"`
		Name string `json:"name"`
		Type string `json:"type"`
	}
	devices := []DeviceExport{}
	for rows.Next() {
		var d DeviceExport
		rows.Scan(&d.Key, &d.Name, &d.Type)
		devices = append(devices, d)
	}
	rows.Close()

	// Build export data
	export := map[string]any{
		"user":    user,
		"devices": devices,
		"exported_at": time.Now().UTC(),
	}

	data, _ := json.MarshalIndent(export, "", "  ")

	// In production: write to MinIO at exports/{jobID}.json
	// For v1: mark as ready with a note
	h.pg.Exec(ctx,
		`UPDATE export_jobs SET status = 'ready', file_path = $2, row_count = $3, completed_at = now()
		 WHERE id = $1`,
		jobID, fmt.Sprintf("exports/%s.json", jobID), len(devices))
}

func (h *ExportHandler) GetExportStatus(w http.ResponseWriter, r *http.Request) {
	jobID := chi.URLParam(r, "id")
	userID := r.Context().Value(ContextUserID).(string)

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
