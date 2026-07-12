// internal/search.go — Full-text search across devices, audit log.
// Uses PostgreSQL tsvector with GIN indexes. No external search service.

package internal

import (
	"net/http"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5/pgxpool"
)

type SearchHandler struct {
	pg *pgxpool.Pool
}

type SearchResult struct {
	ID         string `json:"id"`
	EntityType string `json:"entity_type"`
	Label      string `json:"label"`
	Subtitle   string `json:"subtitle,omitempty"`
}

func NewSearchHandler(pg *pgxpool.Pool) *SearchHandler {
	return &SearchHandler{pg: pg}
}

func (h *SearchHandler) Search(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	types := r.URL.Query().Get("type") // comma-separated: devices,audit
	limit := parseInt(r.URL.Query().Get("limit"), 20)
	offset := parseInt(r.URL.Query().Get("offset"), 0)

	if q == "" {
		writeError(w, "validation_error", "query q is required", http.StatusBadRequest)
		return
	}

	results := []SearchResult{}

	if types == "" || strings.Contains(types, "devices") {
		rows, _ := h.pg.Query(r.Context(), `
			SELECT device_key, device_name, device_type
			FROM devices
			WHERE search_vector @@ plainto_tsquery('english', $1)
			   OR device_key ILIKE '%' || $1 || '%'
			ORDER BY ts_rank(search_vector, plainto_tsquery('english', $1)) DESC
			LIMIT $2 OFFSET $3`, q, limit, offset)
		if rows != nil {
			for rows.Next() {
				var key, name, dtype string
				rows.Scan(&key, &name, &dtype)
				results = append(results, SearchResult{
					ID: key, EntityType: "device", Label: name, Subtitle: dtype,
				})
			}
			rows.Close()
		}
	}

	if strings.Contains(types, "audit") {
		rows, _ := h.pg.Query(r.Context(), `
			SELECT id::text, action, resource_type
			FROM audit_log
			WHERE search_vector @@ plainto_tsquery('english', $1)
			ORDER BY created_at DESC
			LIMIT $2 OFFSET $3`, q, limit, offset)
		if rows != nil {
			for rows.Next() {
				var id, action, rtype string
				rows.Scan(&id, &action, &rtype)
				results = append(results, SearchResult{
					ID: id, EntityType: "audit", Label: action, Subtitle: rtype,
				})
			}
			rows.Close()
		}
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"results": results,
		"total":   len(results),
		"query":   q,
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
