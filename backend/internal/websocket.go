// internal/websocket.go — WebSocket hub for live telemetry push.
// The API server subscribes to live/# on Mosquitto and pushes each
// message to browser sessions subscribed to that device_key.

package internal

import (
	"log/slog"
	"net/http"
	"strings"
	"sync"

	"golang.org/x/net/websocket"
)

type WebSocketHub struct {
	mu      sync.RWMutex
	clients map[string]map[*websocket.Conn]bool
}

func NewWebSocketHub() *WebSocketHub {
	return &WebSocketHub{
		clients: make(map[string]map[*websocket.Conn]bool),
	}
}

// HandleWS handles WebSocket connections for live telemetry
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
	websocket.Handler(func(conn *websocket.Conn) {
		defer conn.Close()
		for {
			var msg struct {
				Type       string   `json:"type"`
				DeviceKeys []string `json:"device_keys,omitempty"`
			}
			if err := websocket.JSON.Receive(conn, &msg); err != nil {
				return
			}
			switch msg.Type {
			case "subscribe":
				for _, key := range msg.DeviceKeys {
					hub.subscribe(key, conn)
				}
				websocket.JSON.Send(conn, map[string]string{"type": "subscribed"})
			case "unsubscribe":
				for _, key := range msg.DeviceKeys {
					hub.unsubscribe(key, conn)
				}
			case "ping":
				websocket.JSON.Send(conn, map[string]string{"type": "pong"})
			}
		}
	}).ServeHTTP(w, r)
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
		if err := websocket.Message.Send(conn, string(data)); err != nil {
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
