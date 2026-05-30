#ifndef SUPABASE_REALTIME_H
#define SUPABASE_REALTIME_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFiClientSecure.h>

class SupabaseRealtime {
public:
    SupabaseRealtime();
    bool begin(const char* supabase_url, const char* anon_key, const char* device_key);
    void end();
    void loop();
    bool isConnected();
    int publishTelemetry(const char* json, size_t len);
    void onSettingsCommand(void (*callback)(const char* cmd_type, const char* payload_json));

private:
    void connect();
    void disconnect();
    void sendRaw(const char* msg);
    void sendJoin(const char* topic, const char* ref);
    void sendHeartbeat();
    void scheduleReconnect();
    void updateReconnectDelay();
    void resetReconnectDelay();
    void onWssEvent(WStype_t type, uint8_t* payload, size_t len);
    void parseAndDispatch(const char* payload, size_t len);

    bool _telemetry_joined = false;
    bool _commands_joined = false;

    WebSocketsClient _client;
    String _url;
    String _device_key;
    String _anon_key;
    bool _connected;
    bool _joined_channels;
    unsigned long _last_heartbeat_ms;
    unsigned long _reconnect_delay_ms;
    unsigned long _last_reconnect_attempt_ms;
    unsigned long _last_wifi_check_ms;

    void (*_settings_cb)(const char* cmd_type, const char* payload_json) = nullptr;

    static char _incoming_buf[2048];
    static JsonDocument _outgoing_doc;
    static SupabaseRealtime* _instance;
};

#endif // SUPABASE_REALTIME_H
