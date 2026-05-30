#include "supabase_realtime.h"

char SupabaseRealtime::_incoming_buf[2048];
JsonDocument SupabaseRealtime::_outgoing_doc;
SupabaseRealtime* SupabaseRealtime::_instance = nullptr;

SupabaseRealtime::SupabaseRealtime()
    : _connected(false)
    , _joined_channels(false)
    , _telemetry_joined(false)
    , _commands_joined(false)
    , _last_heartbeat_ms(0)
    , _reconnect_delay_ms(1000)
    , _last_reconnect_attempt_ms(0)
    , _last_wifi_check_ms(0)
    , _settings_cb(nullptr)
{
    _instance = this;
}

bool SupabaseRealtime::begin(const char* supabase_url, const char* anon_key, const char* device_key) {
    _anon_key = anon_key;
    _device_key = device_key;

    char ws_url[384];
    snprintf(ws_url, sizeof(ws_url), "wss://%s/realtime/v1/websocket?api_key=%s&vsn=1.0.0",
             supabase_url, anon_key);
    _url = ws_url;

    _client.onEvent([this](WStype_t type, uint8_t* payload, size_t len) {
        _instance->onWssEvent(type, payload, len);
    });

    _client.setReconnectInterval(0); // we handle reconnect ourselves

    connect();
    return true;
}

void SupabaseRealtime::end() {
    _client.disconnect();
    _connected = false;
    _telemetry_joined = false;
    _commands_joined = false;
    _joined_channels = false;
}

bool SupabaseRealtime::isConnected() {
    return _connected && _telemetry_joined && _commands_joined;
}

void SupabaseRealtime::loop() {
    _client.loop();

    unsigned long now = millis();

    if (!WiFi.isConnected()) {
        if (_connected || _telemetry_joined || _commands_joined) {
            _connected = false;
            _telemetry_joined = false;
            _commands_joined = false;
            _joined_channels = false;
            resetReconnectDelay();
        }
        return;
    }

    if (_connected && _joined_channels) {
        const unsigned long HB_INTERVAL_MS = 30000UL;
        if (now - _last_heartbeat_ms >= HB_INTERVAL_MS) {
            sendHeartbeat();
            _last_heartbeat_ms = now;
        }
    }

    if (!_connected && now - _last_reconnect_attempt_ms >= _reconnect_delay_ms) {
        updateReconnectDelay();
        connect();
        _last_reconnect_attempt_ms = now;
    }
}

int SupabaseRealtime::publishTelemetry(const char* json, size_t len) {
    if (!isConnected()) return -1;

    _outgoing_doc.clear();
    _outgoing_doc["topic"] = "devices:" + _device_key + ":telemetry";
    _outgoing_doc["event"] = "broadcast";
    _outgoing_doc["ref"] = nullptr;

    JsonObject payload = _outgoing_doc["payload"];
    payload["event"] = "telemetry";
    payload["data"] = json;

    char buf[1024];
    size_t n = serializeJson(_outgoing_doc, buf, sizeof(buf));
    sendRaw(buf);
    return n;
}

void SupabaseRealtime::onSettingsCommand(void (*callback)(const char* cmd_type, const char* payload_json)) {
    _settings_cb = callback;
}

void SupabaseRealtime::connect() {
    if (!WiFi.isConnected()) return;

    _client.beginSSL(_url.c_str(), 443, "/");
    char auth_hdr[384];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", _anon_key.c_str());
    _client.setExtraHeaders(auth_hdr);
    _last_reconnect_attempt_ms = millis();
}

void SupabaseRealtime::disconnect() {
    _client.disconnect();
    _connected = false;
    _telemetry_joined = false;
    _commands_joined = false;
    _joined_channels = false;
}

void SupabaseRealtime::sendRaw(const char* msg) {
    _client.sendTXT(msg);
}

void SupabaseRealtime::sendJoin(const char* topic, const char* ref) {
    _outgoing_doc.clear();
    _outgoing_doc["topic"] = topic;
    _outgoing_doc["event"] = "phx_join";
    _outgoing_doc["payload"] = JsonObject();
    _outgoing_doc["ref"] = ref;

    char buf[512];
    size_t n = serializeJson(_outgoing_doc, buf, sizeof(buf));
    sendRaw(buf);
}

void SupabaseRealtime::sendHeartbeat() {
    _outgoing_doc.clear();
    _outgoing_doc["topic"] = "phoenix";
    _outgoing_doc["event"] = "heartbeat";
    _outgoing_doc["payload"] = JsonObject();
    _outgoing_doc["ref"] = "hb";

    char buf[256];
    size_t n = serializeJson(_outgoing_doc, buf, sizeof(buf));
    sendRaw(buf);
}

void SupabaseRealtime::scheduleReconnect() {
    _connected = false;
    _telemetry_joined = false;
    _commands_joined = false;
    _joined_channels = false;
    updateReconnectDelay();
}

void SupabaseRealtime::updateReconnectDelay() {
    unsigned long max_delay = 60000UL;
    _reconnect_delay_ms = (_reconnect_delay_ms * 2 >= max_delay) ? max_delay : _reconnect_delay_ms * 2;
}

void SupabaseRealtime::resetReconnectDelay() {
    _reconnect_delay_ms = 1000UL;
    _last_reconnect_attempt_ms = millis();
}

void SupabaseRealtime::onWssEvent(WStype_t type, uint8_t* payload, size_t len) {
    switch (type) {
        case WStype_DISCONNECTED:
            _connected = false;
            _telemetry_joined = false;
            _commands_joined = false;
            _joined_channels = false;
            scheduleReconnect();
            break;
        case WStype_CONNECTED:
            _connected = true;
            _telemetry_joined = false;
            _commands_joined = false;
            resetReconnectDelay();
            {
                String telem_topic = "devices:" + _device_key + ":telemetry";
                sendJoin(telem_topic.c_str(), "1");
                String cmd_topic = "devices:" + _device_key + ":commands";
                sendJoin(cmd_topic.c_str(), "2");
            }
            _last_heartbeat_ms = millis();
            break;
        case WStype_TEXT:
            if (len < sizeof(_incoming_buf)) {
                memcpy(_incoming_buf, payload, len);
                _incoming_buf[len] = '\0';
                parseAndDispatch(_incoming_buf, len);
            }
            break;
        case WStype_ERROR:
            _connected = false;
            _telemetry_joined = false;
            _commands_joined = false;
            _joined_channels = false;
            scheduleReconnect();
            break;
        case WStype_PING:
        case WStype_PONG:
            break;
        default:
            break;
    }
}

void SupabaseRealtime::parseAndDispatch(const char* payload, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return;

    const char* topic = doc["topic"].as<const char*>();
    const char* event = doc["event"].as<const char*>();
    if (!topic || !event) return;

    // phx_reply: channel join acknowledgment
    if (strcmp(event, "phx_reply") == 0) {
        const char* status = doc["payload"]["status"].as<const char*>();
        if (status && strcmp(status, "ok") == 0) {
            const char* ref = doc["ref"].as<const char*>();
            const char* topic_str = doc["topic"].as<const char*>();

            if (ref && topic_str) {
                if (strcmp(ref, "1") == 0 && strstr(topic_str, ":telemetry")) {
                    _telemetry_joined = true;
                } else if (strcmp(ref, "2") == 0 && strstr(topic_str, ":commands")) {
                    _commands_joined = true;
                }
            }
            if (_telemetry_joined && _commands_joined) {
                _joined_channels = true;
            }
        }
        return;
    }

    if (strcmp(event, "phx_close") == 0 || strcmp(event, "phx_error") == 0) {
        scheduleReconnect();
        return;
    }

    if (strcmp(event, "broadcast") == 0) {
        const char* topic_str = doc["topic"].as<const char*>();
        if (topic_str && strstr(topic_str, ":commands")) {
            const char* payload_json = doc["payload"]["payload"].as<const char*>();
            const char* cmd_type = doc["payload"]["event"].as<const char*>();
            if (_settings_cb && cmd_type && payload_json) {
                _settings_cb(cmd_type, payload_json);
            }
        }
    }
}
