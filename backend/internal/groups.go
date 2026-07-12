// internal/groups.go — Device groups and tags CRUD.
// Groups organize devices; tags attach key-value metadata.

package internal

import (
	"encoding/json"
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type GroupHandler struct {
	pg *pgxpool.Pool
}

func NewGroupHandler(pg *pgxpool.Pool) *GroupHandler {
	return &GroupHandler{pg: pg}
}

func (h *GroupHandler) ListGroups(w http.ResponseWriter, r *http.Request) {
	rows, err := h.pg.Query(r.Context(),
		`SELECT id, name, coalesce(description,''), coalesce(color,'') FROM device_groups ORDER BY name`)
	if err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	type Group struct {
		ID          string `json:"id"`
		Name        string `json:"name"`
		Description string `json:"description,omitempty"`
		Color       string `json:"color,omitempty"`
	}
	groups := []Group{}
	for rows.Next() {
		var g Group
		rows.Scan(&g.ID, &g.Name, &g.Description, &g.Color)
		groups = append(groups, g)
	}
	writeJSON(w, http.StatusOK, groups)
}

func (h *GroupHandler) CreateGroup(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name        string `json:"name"`
		Description string `json:"description"`
		Color       string `json:"color"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Name == "" {
		writeError(w, "validation_error", "name is required", http.StatusBadRequest)
		return
	}
	var id string
	err := h.pg.QueryRow(r.Context(),
		`INSERT INTO device_groups (name, description, color) VALUES ($1, $2, $3) RETURNING id`,
		req.Name, req.Description, req.Color).Scan(&id)
	if err != nil {
		writeError(w, "conflict", "group name already exists", http.StatusConflict)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]string{"id": id})
}

func (h *GroupHandler) AddDeviceToGroup(w http.ResponseWriter, r *http.Request) {
	groupID := chi.URLParam(r, "id")
	deviceKey := chi.URLParam(r, "key")
	_, err := h.pg.Exec(r.Context(),
		`INSERT INTO device_group_members (group_id, device_key) VALUES ($1, $2) ON CONFLICT DO NOTHING`,
		groupID, deviceKey)
	if err != nil {
		writeError(w, "internal_error", "failed to add device", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "added"})
}

func (h *GroupHandler) RemoveDeviceFromGroup(w http.ResponseWriter, r *http.Request) {
	groupID := chi.URLParam(r, "id")
	deviceKey := chi.URLParam(r, "key")
	h.pg.Exec(r.Context(),
		`DELETE FROM device_group_members WHERE group_id = $1 AND device_key = $2`,
		groupID, deviceKey)
	writeJSON(w, http.StatusOK, map[string]string{"status": "removed"})
}

// ── Tags ────────────────────────────────────────────────────────────

func (h *GroupHandler) ListTags(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	rows, err := h.pg.Query(r.Context(),
		`SELECT key, value FROM device_tags WHERE device_key = $1 ORDER BY key`, deviceKey)
	if err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
	}
	defer rows.Close()
	tags := map[string]string{}
	for rows.Next() {
		var k, v string
		rows.Scan(&k, &v)
		tags[k] = v
	}
	writeJSON(w, http.StatusOK, tags)
}

func (h *GroupHandler) SetTag(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	tagKey := chi.URLParam(r, "tag_key")
	var req struct {
		Value string `json:"value"`
	}
	json.NewDecoder(r.Body).Decode(&req)
	h.pg.Exec(r.Context(),
		`INSERT INTO device_tags (device_key, key, value) VALUES ($1, $2, $3)
		 ON CONFLICT (device_key, key) DO UPDATE SET value = $3`,
		deviceKey, tagKey, req.Value)
	writeJSON(w, http.StatusOK, map[string]string{"status": "set"})
}

func (h *GroupHandler) DeleteTag(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	tagKey := chi.URLParam(r, "tag_key")
	h.pg.Exec(r.Context(),
		`DELETE FROM device_tags WHERE device_key = $1 AND key = $2`,
		deviceKey, tagKey)
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}
