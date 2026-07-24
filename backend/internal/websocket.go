// internal/websocket.go — WebSocket hub for live telemetry push.
// Uses gorilla/websocket. The API server subscribes to live/# on Mosquitto
// and pushes each message to browser sessions subscribed to that device_key.

package internal

import (
	"log/slog"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/jackc/pgx/v5/pgxpool"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	CheckOrigin:     func(r *http.Request) bool { return true },
}

type WebSocketHub struct {
	pg      *pgxpool.Pool
	mu      sync.RWMutex
	clients map[string]map[*websocket.Conn]bool
}

func NewWebSocketHub(pg *pgxpool.Pool) *WebSocketHub {
	return &WebSocketHub{
		pg:      pg,
		clients: make(map[string]map[*websocket.Conn]bool),
	}
}

// HandleWS handles WebSocket connections for live telemetry.
// It runs under AuthMiddleware, so a valid JWT context is required.
// @Summary      WebSocket endpoint
// @Tags         WebSocket
// @Description  Connect via WebSocket, then send JSON messages:\n
// @Description  {"type":"subscribe","device_keys":["AABB..."]}\n
// @Description  {"type":"unsubscribe","device_keys":["AABB..."]}\n
// @Description  {"type":"ping"}\n
// @Description  Server pushes enriched telemetry as JSON strings.
// @Success      101  "Switching Protocols"
// @Security     BearerAuth
// @Router       /ws [get]
func (hub *WebSocketHub) HandleWS(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		w.WriteHeader(http.StatusUnauthorized)
		return
	}

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		slog.Warn("websocket upgrade failed", "error", err)
		return
	}
	defer conn.Close()

	// Start a goroutine that closes the connection if the client goes silent.
	// It closes the underlying conn on ping failure; the read loop then errors
	// out and exits without re-closing the done channel.
	done := make(chan struct{})
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				if err := conn.WriteControl(websocket.PingMessage, []byte{}, time.Now().Add(5*time.Second)); err != nil {
					_ = conn.Close()
					return
				}
			case <-done:
				return
			}
		}
	}()

	for {
		var msg struct {
			Type       string   `json:"type"`
			DeviceKeys []string `json:"device_keys,omitempty"`
		}
		if err := conn.ReadJSON(&msg); err != nil {
			break
		}
		switch msg.Type {
		case "subscribe":
			subscribed := []string{}
			for _, key := range msg.DeviceKeys {
				if !IsDeviceOwner(r.Context(), hub.pg, key, userID) {
					_ = conn.WriteJSON(map[string]any{"type": "error", "message": "device not found: " + key})
					continue
				}
				hub.subscribe(key, conn)
				subscribed = append(subscribed, key)
			}
			_ = conn.WriteJSON(map[string]any{"type": "subscribed", "device_keys": subscribed})
		case "unsubscribe":
			for _, key := range msg.DeviceKeys {
				hub.unsubscribe(key, conn)
			}
		case "ping":
			_ = conn.WriteJSON(map[string]string{"type": "pong"})
		}
	}

	// Clean up subscriptions when the connection closes.
	hub.removeConn(conn)
	close(done)
}

func (hub *WebSocketHub) subscribe(deviceKey string, conn *websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	if hub.clients[deviceKey] == nil {
		hub.clients[deviceKey] = make(map[*websocket.Conn]bool)
	}
	hub.clients[deviceKey][conn] = true
}

func (hub *WebSocketHub) unsubscribe(deviceKey string, conn *websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	if hub.clients[deviceKey] != nil {
		delete(hub.clients[deviceKey], conn)
	}
}

func (hub *WebSocketHub) removeConn(conn *websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	for _, conns := range hub.clients {
		delete(conns, conn)
	}
}

// Broadcast sends an enriched telemetry message to all clients subscribed
// to the given device_key.
func (hub *WebSocketHub) Broadcast(deviceKey string, data []byte) {
	hub.mu.RLock()
	conns := hub.clients[deviceKey]
	snapshot := make([]*websocket.Conn, 0, len(conns))
	for conn := range conns {
		snapshot = append(snapshot, conn)
	}
	hub.mu.RUnlock()

	for _, conn := range snapshot {
		if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
			slog.Warn("websocket send failed", "device", deviceKey, "error", err)
			hub.unsubscribe(deviceKey, conn)
		}
	}
}

// OnLiveMessage is called by the MQTT subscriber when a message arrives on live/#.
func (hub *WebSocketHub) OnLiveMessage(topic string, payload []byte) {
	deviceKey := strings.TrimPrefix(topic, "live/")
	hub.Broadcast(deviceKey, payload)
}
