// internal/mqttauth.go — Mosquitto HTTP auth backend.
// Mosquitto calls POST /api/v1/mqtt/auth to validate device credentials.
// Returns 200 OK if valid, 403 Forbidden if not.

package internal

import (
	"encoding/json"
	"net/http"

	"github.com/jackc/pgx/v5/pgxpool"
)

type MQTTAuthHandler struct {
	pg *pgxpool.Pool
}

type mqttAuthRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type mqttAuthResponse struct {
	OK   bool      `json:"ok"`
	ACLs []mqttACL `json:"acls,omitempty"`
}

type mqttACL struct {
	Topic  string `json:"topic"`
	Access string `json:"access"`
}

func NewMQTTAuthHandler(pg *pgxpool.Pool) *MQTTAuthHandler {
	return &MQTTAuthHandler{pg: pg}
}

// ServeHTTP validates device credentials for Mosquitto
// @Summary      MQTT auth (Mosquitto backend)
// @Tags         MQTT
// @Accept       json
// @Produce      json
// @Param        body  body  MQTTAuthRequest  true  "Device credentials"
// @Success      200  {object}  MQTTAuthResponse
// @Failure      403  {object}  MQTTAuthResponse
// @Router       /mqtt/auth [post]
func (h *MQTTAuthHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	var req mqttAuthRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}

	var apiKey string
	err := h.pg.QueryRow(r.Context(),
		`SELECT api_key::text FROM devices WHERE device_key = $1 AND is_active = true`,
		req.Username).Scan(&apiKey)
	if err != nil || apiKey != req.Password {
		writeJSON(w, http.StatusForbidden, mqttAuthResponse{OK: false})
		return
	}

	writeJSON(w, http.StatusOK, mqttAuthResponse{
		OK: true,
		ACLs: []mqttACL{
			{Topic: "telemetry/" + req.Username + "/#", Access: "write"},
			{Topic: "status/" + req.Username + "/#", Access: "write"},
			{Topic: "commands/" + req.Username, Access: "read"},
			{Topic: "ota/" + req.Username, Access: "read"},
		},
	})
}
