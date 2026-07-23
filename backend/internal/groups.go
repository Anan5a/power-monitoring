// internal/groups.go — Device groups and tags CRUD.
// Groups organize devices; tags attach key-value metadata.

package internal

import (
	"context"
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

// ListGroups returns all device groups
// @Summary      List device groups
// @Tags         Groups
// @Produce      json
// @Success      200  {array}  Group
// @Security     BearerAuth
// @Router       /groups [get]
func (h *GroupHandler) ListGroups(w http.ResponseWriter, r *http.Request) {
	rows, err := h.pg.Query(r.Context(),
		`SELECT id, name, coalesce(description,''), coalesce(color,'') FROM device_groups ORDER BY name`)
	if err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	groups := []Group{}
	for rows.Next() {
		var g Group
		rows.Scan(&g.ID, &g.Name, &g.Description, &g.Color)
		groups = append(groups, g)
	}
	writeJSON(w, http.StatusOK, groups)
}

// CreateGroup creates a new device group
// @Summary      Create a device group
// @Tags         Groups
// @Accept       json
// @Produce      json
// @Param        body  body  CreateGroupRequest  true  "Group details"
// @Success      201  {object}  map[string]string
// @Failure      400  {object}  APIError
// @Security     BearerAuth
// @Router       /groups [post]
func (h *GroupHandler) CreateGroup(w http.ResponseWriter, r *http.Request) {
	var req CreateGroupRequest
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

// AddDeviceToGroup adds a device to a group
// @Summary      Add device to group
// @Tags         Groups
// @Produce      json
// @Param        id   path  string  true  "Group ID"
// @Param        key  path  string  true  "Device key"
// @Success      200  {object}  map[string]string
// @Security     BearerAuth
// @Router       /groups/{id}/devices/{key} [post]
func (h *GroupHandler) AddDeviceToGroup(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	groupID := chi.URLParam(r, "id")
	deviceKey := chi.URLParam(r, "key")

	if !h.isDeviceOwner(r.Context(), deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	_, err := h.pg.Exec(r.Context(),
		`INSERT INTO device_group_members (group_id, device_key) VALUES ($1, $2) ON CONFLICT DO NOTHING`,
		groupID, deviceKey)
	if err != nil {
		writeError(w, "internal_error", "failed to add device", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "added"})
}

// RemoveDeviceFromGroup removes a device from a group
// @Summary      Remove device from group
// @Tags         Groups
// @Produce      json
// @Param        id   path  string  true  "Group ID"
// @Param        key  path  string  true  "Device key"
// @Success      200  {object}  map[string]string
// @Security     BearerAuth
// @Router       /groups/{id}/devices/{key} [delete]
func (h *GroupHandler) RemoveDeviceFromGroup(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	groupID := chi.URLParam(r, "id")
	deviceKey := chi.URLParam(r, "key")

	if !h.isDeviceOwner(r.Context(), deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	h.pg.Exec(r.Context(),
		`DELETE FROM device_group_members WHERE group_id = $1 AND device_key = $2`,
		groupID, deviceKey)
	writeJSON(w, http.StatusOK, map[string]string{"status": "removed"})
}

// ── Tags ────────────────────────────────────────────────────────────

// ListTags returns all tags for a device
// @Summary      List device tags
// @Tags         Tags
// @Produce      json
// @Param        key  path  string  true  "Device key"
// @Success      200  {object}  Tags
// @Security     BearerAuth
// @Router       /devices/{key}/tags [get]
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

// SetTag sets a tag on a device
// @Summary      Set a device tag
// @Tags         Tags
// @Accept       json
// @Produce      json
// @Param        key     path  string         true  "Device key"
// @Param        tag_key path  string         true  "Tag key"
// @Param        body    body  SetTagRequest  true  "Tag value"
// @Success      200  {object}  map[string]string
// @Security     BearerAuth
// @Router       /devices/{key}/tags/{tag_key} [post]
func (h *GroupHandler) SetTag(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	deviceKey := chi.URLParam(r, "key")
	tagKey := chi.URLParam(r, "tag_key")
	if !h.isDeviceOwner(r.Context(), deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
	var req struct {
		Value string `json:"value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}
	if _, err := h.pg.Exec(r.Context(),
		`INSERT INTO device_tags (device_key, key, value) VALUES ($1, $2, $3)
		 ON CONFLICT (device_key, key) DO UPDATE SET value = $3`,
		deviceKey, tagKey, req.Value); err != nil {
		writeError(w, "internal_error", "failed to set tag", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "set"})
}

// DeleteTag deletes a tag from a device
// @Summary      Delete a device tag
// @Tags         Tags
// @Produce      json
// @Param        key     path  string  true  "Device key"
// @Param        tag_key path  string  true  "Tag key"
// @Success      200  {object}  map[string]string
// @Security     BearerAuth
// @Router       /devices/{key}/tags/{tag_key} [delete]
func (h *GroupHandler) DeleteTag(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	deviceKey := chi.URLParam(r, "key")
	tagKey := chi.URLParam(r, "tag_key")
	if !h.isDeviceOwner(r.Context(), deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
	if _, err := h.pg.Exec(r.Context(),
		`DELETE FROM device_tags WHERE device_key = $1 AND key = $2`,
		deviceKey, tagKey); err != nil {
		writeError(w, "internal_error", "failed to delete tag", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}

// isDeviceOwner returns true if the device is owned by the given user.
func (h *GroupHandler) isDeviceOwner(ctx context.Context, deviceKey, userID string) bool {
	if h.pg == nil {
		return false
	}
	var ownerID *string
	err := h.pg.QueryRow(ctx,
		`SELECT owner_id::text FROM devices WHERE device_key = $1`, deviceKey).Scan(&ownerID)
	if err != nil {
		return false
	}
	return ownerID != nil && *ownerID == userID
}
