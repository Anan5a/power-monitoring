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

// GroupHandler implements device groups and device tags. Groups are
// per-user-owned collections of devices; tags are free-form key/value metadata
// attached to a single device. All mutations emit audit entries via the
// embedded Auditor.
type GroupHandler struct {
	pg      *pgxpool.Pool
	auditor *Auditor
}

// NewGroupHandler constructs a GroupHandler backed by the given pool and a
// per-handler Auditor for recording group/tag mutations.
func NewGroupHandler(pg *pgxpool.Pool) *GroupHandler {
	return &GroupHandler{pg: pg, auditor: NewAuditor(pg)}
}

// ListGroups returns all device groups
// @Summary      List device groups
// @Tags         Groups
// @Produce      json
// @Success      200  {array}  Group
// @Security     BearerAuth
// @Router       /groups [get]
func (h *GroupHandler) ListGroups(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	role, _ := r.Context().Value(ContextUserRole).(string)
	// Ownership scoping: a user sees groups they own, plus legacy
	// NULL-owner global groups that pre-date per-user ownership — but only
	// admins see those legacy groups. The OR clause is parameterised with the
	// role so the planner can short-circuit for non-admins.
	rows, err := h.pg.Query(r.Context(),
		`SELECT id, name, coalesce(description,''), coalesce(color,'')
		 FROM device_groups
		 WHERE owner_id = $1 OR ($2 = 'admin' AND owner_id IS NULL)
		 ORDER BY name`, userID, role)
	if err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	groups := []Group{}
	for rows.Next() {
		var g Group
		if err := rows.Scan(&g.ID, &g.Name, &g.Description, &g.Color); err != nil {
			writeError(w, "internal_error", "scan failed", http.StatusInternalServerError)
			return
		}
		groups = append(groups, g)
	}
	if err := rows.Err(); err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	var req CreateGroupRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Name == "" {
		writeError(w, "validation_error", "name is required", http.StatusBadRequest)
		return
	}
	var id string
	err := h.pg.QueryRow(r.Context(),
		`INSERT INTO device_groups (name, description, color, owner_id)
		 VALUES ($1, $2, $3, $4) RETURNING id`,
		req.Name, req.Description, req.Color, userID).Scan(&id)
	if err != nil {
		writeError(w, "conflict", "group name already exists", http.StatusConflict)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "group.create",
		ResourceType: "device_group",
		ResourceID:   id,
		Details:      map[string]any{"name": req.Name},
	})
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

	if !h.groupOwnedBy(r.Context(), groupID, userID) {
		// Hide existence of groups the caller does not own (returns 404, not 403,
		// to avoid leaking which group IDs exist).
		writeError(w, "not_found", "group not found", http.StatusNotFound)
		return
	}
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	// ON CONFLICT DO NOTHING makes adding a device that is already a member
	// idempotent — a retry or duplicate request succeeds without error.
	_, err := h.pg.Exec(r.Context(),
		`INSERT INTO device_group_members (group_id, device_key) VALUES ($1, $2) ON CONFLICT DO NOTHING`,
		groupID, deviceKey)
	if err != nil {
		writeError(w, "internal_error", "failed to add device", http.StatusInternalServerError)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "group.add_device",
		ResourceType: "device_group",
		ResourceID:   groupID,
		Details:      map[string]any{"device_key": deviceKey},
	})
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

	if !h.groupOwnedBy(r.Context(), groupID, userID) {
		// Same 404-not-403 concealment as AddDeviceToGroup.
		writeError(w, "not_found", "group not found", http.StatusNotFound)
		return
	}
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	if _, err := h.pg.Exec(r.Context(),
		`DELETE FROM device_group_members WHERE group_id = $1 AND device_key = $2`,
		groupID, deviceKey); err != nil {
		writeError(w, "internal_error", "failed to remove device", http.StatusInternalServerError)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "group.remove_device",
		ResourceType: "device_group",
		ResourceID:   groupID,
		Details:      map[string]any{"device_key": deviceKey},
	})
	writeJSON(w, http.StatusOK, map[string]string{"status": "removed"})
}

// groupOwnedBy returns true if the group exists and is owned by userID
// (or is a legacy NULL-owner group and the user is an admin).
func (h *GroupHandler) groupOwnedBy(ctx context.Context, groupID, userID string) bool {
	if h.pg == nil || groupID == "" || userID == "" {
		return false
	}
	var ownerID *string
	err := h.pg.QueryRow(ctx,
		`SELECT owner_id::text FROM device_groups WHERE id = $1`, groupID).Scan(&ownerID)
	if err != nil {
		return false
	}
	if ownerID == nil {
		// Legacy global group — only admins may modify. These pre-date the
		// owner_id column (added by migration 004_group_ownership) and have
		// no single owner, so admin role is the only permitted accessor.
		role, _ := ctx.Value(ContextUserRole).(string)
		return role == "admin"
	}
	return *ownerID == userID
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
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
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
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
	// Upsert: ON CONFLICT (device_key, key) turns a duplicate set into an
	// update of the existing value, so tags are set-or-replace semantics.
	if _, err := h.pg.Exec(r.Context(),
		`INSERT INTO device_tags (device_key, key, value) VALUES ($1, $2, $3)
		 ON CONFLICT (device_key, key) DO UPDATE SET value = $3`,
		deviceKey, tagKey, req.Value); err != nil {
		writeError(w, "internal_error", "failed to set tag", http.StatusInternalServerError)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "device.tag.set",
		ResourceType: "device_tag",
		ResourceID:   deviceKey,
		Details:      map[string]any{"tag_key": tagKey, "value": req.Value},
	})
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
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
	if _, err := h.pg.Exec(r.Context(),
		`DELETE FROM device_tags WHERE device_key = $1 AND key = $2`,
		deviceKey, tagKey); err != nil {
		writeError(w, "internal_error", "failed to delete tag", http.StatusInternalServerError)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "device.tag.delete",
		ResourceType: "device_tag",
		ResourceID:   deviceKey,
		Details:      map[string]any{"tag_key": tagKey},
	})
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}
