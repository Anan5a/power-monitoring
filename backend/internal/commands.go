// internal/commands.go — Device command queue.
// Web UI inserts commands; devices poll GET /commands/{key}/pending to claim them.

package internal

import (
	"encoding/json"
	"net/http"
	"strconv"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// CommandHandler exposes HTTP endpoints for the device command queue. The web
// UI enqueues commands addressed to a device; devices poll to claim and later
// report execution results back.
type CommandHandler struct {
	pg *pgxpool.Pool
}

// NewCommandHandler constructs a CommandHandler backed by the given pool.
func NewCommandHandler(pg *pgxpool.Pool) *CommandHandler {
	return &CommandHandler{pg: pg}
}

// CreateCommand enqueues a command for a device.
// @Summary      Send command to device
// @Tags         Commands
// @Accept       json
// @Produce      json
// @Param        body  body  CreateCommandRequest  true  "Command details"
// @Success      202  {object}  DeviceCommand
// @Failure      400  {object}  APIError
// @Failure      404  {object}  APIError
// @Security     BearerAuth
// @Router       /commands [post]
func (h *CommandHandler) CreateCommand(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	var req CreateCommandRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}
	if req.DeviceKey == "" || req.CmdType == "" {
		writeError(w, "validation_error", "device_key and cmd_type are required", http.StatusBadRequest)
		return
	}

	if !IsDeviceOwner(r.Context(), h.pg, req.DeviceKey, userID) {
		// Hide the existence of devices the caller does not own.
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}

	// Payload is an opaque JSON blob forwarded verbatim to the device firmware.
	payload, _ := json.Marshal(req.Payload)
	var cmd DeviceCommand
	err := h.pg.QueryRow(r.Context(), `
		INSERT INTO device_commands (device_key, cmd_type, payload, status)
		VALUES ($1, $2, $3, 'pending')
		RETURNING id, device_key, cmd_type, payload, status, created_at`,
		req.DeviceKey, req.CmdType, payload).Scan(
		&cmd.ID, &cmd.DeviceKey, &cmd.CmdType, &cmd.Payload, &cmd.Status, &cmd.CreatedAt)
	if err != nil {
		writeError(w, "internal_error", "failed to create command", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusAccepted, cmd)
}

// GetPendingCommands returns pending commands for a device and marks them claimed.
// Devices poll this endpoint. Optional JWT is accepted but not required (firmware polling).
// @Summary      Poll pending commands for a device
// @Tags         Commands
// @Produce      json
// @Param        key  path  string  true  "Device key"
// @Success      200  {array}  DeviceCommand
// @Router       /commands/{key}/pending [get]
func (h *CommandHandler) GetPendingCommands(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	if deviceKey == "" {
		writeError(w, "bad_request", "device key required", http.StatusBadRequest)
		return
	}

	// Firmware authenticates via DeviceAuthMiddleware (X-Device-Key/X-Api-Key or
	// Basic auth). The authenticated device must match the path device key.
	authedKey, _ := r.Context().Value(ContextDeviceKey).(string)
	if authedKey != "" {
		if authedKey != deviceKey {
			writeError(w, "forbidden", "device key mismatch", http.StatusForbidden)
			return
		}
	} else if userID, ok := r.Context().Value(ContextUserID).(string); ok && userID != "" {
		// A user token was supplied instead — verify ownership.
		if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
			writeError(w, "not_found", "device not found", http.StatusNotFound)
			return
		}
	} else {
		writeError(w, "unauthorized", "device or user credentials required", http.StatusUnauthorized)
		return
	}

	// Atomically claim pending commands in a single statement: the inner
	// SELECT ... FOR UPDATE SKIP LOCKED locks rows that are not already
	// locked by concurrent polls (so two devices never receive the same
	// command), and the outer UPDATE flips them to "claimed" in the same
	// transaction. This avoids a separate SELECT-then-UPDATE race.
	rows, err := h.pg.Query(r.Context(), `
		UPDATE device_commands
		SET status = 'claimed'
		WHERE id IN (
			SELECT id FROM device_commands
			WHERE device_key = $1 AND status = 'pending'
			ORDER BY created_at ASC
			FOR UPDATE SKIP LOCKED
		)
		RETURNING id, device_key, cmd_type, payload, status, created_at`,
		deviceKey)
	if err != nil {
		writeError(w, "internal_error", "failed to fetch commands", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	commands := []DeviceCommand{}
	for rows.Next() {
		var cmd DeviceCommand
		var payload []byte
		if err := rows.Scan(&cmd.ID, &cmd.DeviceKey, &cmd.CmdType, &payload, &cmd.Status, &cmd.CreatedAt); err != nil {
			writeError(w, "internal_error", "failed to read command", http.StatusInternalServerError)
			return
		}
		json.Unmarshal(payload, &cmd.Payload)
		commands = append(commands, cmd)
	}
	if err := rows.Err(); err != nil {
		writeError(w, "internal_error", "failed to read commands", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, commands)
}

// UpdateCommandResult records a device's response to a command.
// @Summary      Report command result
// @Tags         Commands
// @Accept       json
// @Produce      json
// @Param        id    path  string                true  "Command ID"
// @Param        body  body  CommandResultRequest  true  "Result details"
// @Success      200  {object}  map[string]string
// @Router       /commands/{id}/result [post]
func (h *CommandHandler) UpdateCommandResult(w http.ResponseWriter, r *http.Request) {
	idStr := chi.URLParam(r, "id")
	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		writeError(w, "bad_request", "invalid command id", http.StatusBadRequest)
		return
	}

	var req CommandResultRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}
	if req.Status != "applied" && req.Status != "failed" {
		writeError(w, "validation_error", "status must be applied or failed", http.StatusBadRequest)
		return
	}

	// Resolve which device may update this command. Firmware authenticates via
	// DeviceAuthMiddleware; the command must belong to that device. A user token
	// is also accepted if the user owns the command's device.
	var cmdDeviceKey string
	if err := h.pg.QueryRow(r.Context(),
		`SELECT device_key FROM device_commands WHERE id = $1`, id).Scan(&cmdDeviceKey); err != nil {
		writeError(w, "not_found", "command not found", http.StatusNotFound)
		return
	}
	authedKey, _ := r.Context().Value(ContextDeviceKey).(string)
	if authedKey != "" {
		if authedKey != cmdDeviceKey {
			writeError(w, "forbidden", "command does not belong to this device", http.StatusForbidden)
			return
		}
	} else if userID, ok := r.Context().Value(ContextUserID).(string); ok && userID != "" {
		if !IsDeviceOwner(r.Context(), h.pg, cmdDeviceKey, userID) {
			writeError(w, "not_found", "command not found", http.StatusNotFound)
			return
		}
	} else {
		writeError(w, "unauthorized", "device or user credentials required", http.StatusUnauthorized)
		return
	}

	result, _ := json.Marshal(req.Result)
	_, err = h.pg.Exec(r.Context(), `
		UPDATE device_commands
		SET status = $2, result = $3, error = $4, applied_at = now()
		WHERE id = $1`,
		id, req.Status, result, req.Error)
	if err != nil {
		writeError(w, "internal_error", "failed to update command", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "updated"})
}
