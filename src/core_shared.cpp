#include "core_shared.h"
#include "connectivity_manager.h"

QueueHandle_t g_sensor_queue = nullptr;
QueueHandle_t g_cmd_queue = nullptr;
SemaphoreHandle_t g_relay_mutex = nullptr;

void init_core_shared() {
    g_sensor_queue = xQueueCreate(16, sizeof(SensorSnapshot));
    g_cmd_queue = xQueueCreate(8, 128);
    g_relay_mutex = xSemaphoreCreateMutex();
}

void push_sensor_data(const SensorSnapshot& data) {
    if (!g_sensor_queue) return;
    xQueueSend(g_sensor_queue, &data, 0);
}

bool pop_settings_cmd(char* cmd_type_buf, char* payload_json_buf, size_t buf_len) {
    if (!g_cmd_queue || !cmd_type_buf || !payload_json_buf) return false;
    uint8_t buf[256];
    if (xQueueReceive(g_cmd_queue, buf, 0) != pdTRUE) return false;
    size_t idx = 0;
    size_t pos = 0;
    while (idx < 256 && buf[idx] != 0) {
        cmd_type_buf[pos++] = (char)buf[idx++];
    }
    cmd_type_buf[pos] = 0;
    idx++;
    pos = 0;
    while (idx < 256 && buf[idx] != 0 && pos < (int)buf_len - 1) {
        payload_json_buf[pos++] = (char)buf[idx++];
    }
    payload_json_buf[pos] = 0;
    return true;
}