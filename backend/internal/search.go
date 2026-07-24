// internal/search.go — Full-text search across devices, audit log.
// Uses PostgreSQL tsvector with GIN indexes. No external search service.

package internal

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5/pgxpool"
)

// SearchHandler exposes a full-text search endpoint over devices and the audit
// log using PostgreSQL tsvector/GIN indexes. It is kept separate from Handlers
// so it can be wired independently of the auth/telemetry dependencies.
type SearchHandler struct {
	pg *pgxpool.Pool
}

// SearchResult is a single hit returned by Search. The same struct shape is
// reused for every entity type so the client can render results uniformly.
type SearchResult struct {
	// ID is the entity identifier: device_key for devices, audit_log.id for audit.
	ID string `json:"id"`
	// EntityType names the kind of record matched, e.g. "device" or "audit".
	EntityType string `json:"entity_type"`
	// Label is the human-readable primary text shown for the result.
	Label string `json:"label"`
	// Subtitle is an optional secondary line (e.g. device type or resource type).
	Subtitle string `json:"subtitle,omitempty"`
}

// NewSearchHandler constructs a SearchHandler backed by the given pool.
func NewSearchHandler(pg *pgxpool.Pool) *SearchHandler {
	return &SearchHandler{pg: pg}
}

// Search performs full-text search across devices and audit log
// @Summary      Full-text search
// @Tags         Search
// @Produce      json
// @Param        q       query  string  true   "Search query"
// @Param        type    query  string  false  "Entity types: devices,audit (comma-separated)"
// @Param        limit   query  int     false  "Max results"  default(20)
// @Param        offset  query  int     false  "Result offset"  default(0)
// @Success      200  {object}  SearchResponse
// @Failure      400  {object}  APIError
// @Security     BearerAuth
// @Router       /search [get]
func (h *SearchHandler) Search(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	types := r.URL.Query().Get("type")
	limit := parseInt(r.URL.Query().Get("limit"), 20)
	offset := parseInt(r.URL.Query().Get("offset"), 0)

	if q == "" {
		writeError(w, "validation_error", "query q is required", http.StatusBadRequest)
		return
	}
	if h.pg == nil {
		// No database wired — return an empty result set rather than erroring.
		writeJSON(w, http.StatusOK, SearchResponse{Results: []SearchResult{}, Total: 0, Query: q})
		return
	}

	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	role, _ := r.Context().Value(ContextUserRole).(string)

	results := []SearchResult{}

	// Device search: restricted to the caller's own devices via owner_id = $1.
	// Matches use the GIN-indexed tsvector (search_vector @@ plainto_tsquery)
	// plus an ILIKE fallback on device_key so exact substring matches that the
	// tsvector stemmer would miss (e.g. partial keys) are still found.
	if types == "" || strings.Contains(types, "devices") {
		like := "%" + q + "%"
		rows, err := h.pg.Query(r.Context(), `
			SELECT device_key, device_name, device_type
			FROM devices
			WHERE owner_id = $1
			  AND (search_vector @@ plainto_tsquery('english', $2)
			       OR device_key ILIKE $5)
			ORDER BY ts_rank(search_vector, plainto_tsquery('english', $2)) DESC
			LIMIT $3 OFFSET $4`, userID, q, limit, offset, like)
		if err != nil {
			writeError(w, "internal_error", "device search failed", http.StatusInternalServerError)
			return
		}
		for rows.Next() {
			var key, name, dtype string
			rows.Scan(&key, &name, &dtype)
			results = append(results, SearchResult{
				ID: key, EntityType: "device", Label: name, Subtitle: dtype,
			})
		}
		// Close explicitly rather than defer: this function issues a second
		// query below, and deferring both would keep the first rows open until
		// return, unnecessarily holding a connection-side resource.
		rows.Close()
	}

	// Audit search is admin-only; non-admins get their own entries via /admin/audit.
	if role == "admin" && strings.Contains(types, "audit") {
		rows, err := h.pg.Query(r.Context(), `
			SELECT id::text, action, resource_type
			FROM audit_log
			WHERE search_vector @@ plainto_tsquery('english', $1)
			ORDER BY created_at DESC
			LIMIT $2 OFFSET $3`, q, limit, offset)
		if err != nil {
			writeError(w, "internal_error", "audit search failed", http.StatusInternalServerError)
			return
		}
		for rows.Next() {
			var id, action, rtype string
			rows.Scan(&id, &action, &rtype)
			results = append(results, SearchResult{
				ID: id, EntityType: "audit", Label: action, Subtitle: rtype,
			})
		}
		rows.Close()
	}

	writeJSON(w, http.StatusOK, SearchResponse{
		Results: results,
		Total:   len(results),
		Query:   q,
	})
}

func parseInt(s string, def int) int {
	if s == "" {
		return def
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return def
	}
	return v
}
