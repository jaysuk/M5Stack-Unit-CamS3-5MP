#include "mqtt_mgr.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_camera.h"
#include "jpeg_validate.h"
#include "recovery_mgr.h"
#include "http_server.h"
#include "config_mgr.h"

// OTA callback — registered by ota_mgr_init() to break circular link dependency
static esp_err_t (*s_ota_cb)(const char *url, const char *sha256_hex) = NULL;

void mqtt_mgr_register_ota_callback(esp_err_t (*cb)(const char *url, const char *sha256_hex)) {
    s_ota_cb = cb;
}

// Reprovision callback — registered by main.c to break circular link dependency
static void (*s_reprovision_cb)(void) = NULL;

void mqtt_mgr_register_reprovision_callback(void (*cb)(void)) {
    s_reprovision_cb = cb;
}

// LED callback — registered by main.c; keeps esp_driver_gpio out of mqtt_mgr
static void (*s_led_cb)(int level) = NULL;

void mqtt_mgr_register_led_callback(void (*cb)(int level)) {
    s_led_cb = cb;
}

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t client = NULL;
static TaskHandle_t s_telemetry_task = NULL;
static char device_id[32] = CONFIG_UNITCAMS3_DEVICE_ID;
static char base_topic[64];
static volatile bool s_connected = false;

// ========================================
// Discovery & Configuration
// ========================================

static void publish_discovery(const char *component, const char *entity_id, const char *name, 
                              const char *device_class, const char *unit, 
                              const char *command_topic, const char *state_topic,
                              int min, int max)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    
    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "%s_%s", device_id, entity_id);
    cJSON_AddStringToObject(root, "unique_id", unique_id);

    if (device_class) cJSON_AddStringToObject(root, "device_class", device_class);
    if (unit) cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    if (command_topic) cJSON_AddStringToObject(root, "command_topic", command_topic);
    if (state_topic) cJSON_AddStringToObject(root, "state_topic", state_topic);

    // Device Info
    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "identifiers", device_id);
    cJSON_AddStringToObject(device, "name", "UnitCamS3 5MP");
    cJSON_AddStringToObject(device, "model", "UnitCamS3-5MP");
    cJSON_AddStringToObject(device, "manufacturer", "M5Stack/Custom");
    cJSON_AddItemToObject(root, "device", device);

    // Entity specific
    if (min != max) {
        cJSON_AddNumberToObject(root, "min", min);
        cJSON_AddNumberToObject(root, "max", max);
    }
    
    // Availability
    char avail_topic[128];
    snprintf(avail_topic, sizeof(avail_topic), "%s/status", base_topic);
    cJSON_AddStringToObject(root, "availability_topic", avail_topic);
    cJSON_AddStringToObject(root, "payload_available", "ON");
    cJSON_AddStringToObject(root, "payload_not_available", "OFF");

    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, device_id, entity_id);
    
    char *json_str = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(client, topic, json_str, 0, 1, 1); // Retain=1
    
    free(json_str);
    cJSON_Delete(root);
}

static void send_ha_discovery(void)
{
    // Connectivity
    char state_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/status", base_topic);
    publish_discovery("binary_sensor", "connectivity", "Connectivity", "connectivity", NULL, NULL, state_topic, 0, 0);

    // RSSI
    snprintf(state_topic, sizeof(state_topic), "%s/rssi", base_topic);
    publish_discovery("sensor", "rssi", "Signal Strength", "signal_strength", "dBm", NULL, state_topic, 0, 0);

    // Uptime
    snprintf(state_topic, sizeof(state_topic), "%s/uptime", base_topic);
    publish_discovery("sensor", "uptime", "Uptime", "duration", "s", NULL, state_topic, 0, 0);

    // Free Heap
    snprintf(state_topic, sizeof(state_topic), "%s/heap", base_topic);
    publish_discovery("sensor", "heap", "Free Heap", "data_size", "B", NULL, state_topic, 0, 0);

    // Recovery Count
    snprintf(state_topic, sizeof(state_topic), "%s/recovery_count", base_topic);
    publish_discovery("sensor", "recovery_count", "Recovery Events", NULL, NULL, NULL, state_topic, 0, 0);

    // Controls
    // Brightness (0 to 8, driver range in mega_ccm.c)
    char cmd_topic[128];
    snprintf(state_topic, sizeof(state_topic), "%s/brightness", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/brightness/set", base_topic);
    publish_discovery("number", "brightness", "Brightness", NULL, NULL, cmd_topic, state_topic, 0, 8);

    // Contrast (0 to 6, driver range in mega_ccm.c)
    snprintf(state_topic, sizeof(state_topic), "%s/contrast", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/contrast/set", base_topic);
    publish_discovery("number", "contrast", "Contrast", NULL, NULL, cmd_topic, state_topic, 0, 6);

    // Saturation (0 to 6, driver range in mega_ccm.c)
    snprintf(state_topic, sizeof(state_topic), "%s/saturation", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/saturation/set", base_topic);
    publish_discovery("number", "saturation", "Saturation", NULL, NULL, cmd_topic, state_topic, 0, 6);
    
    // AWB Mode (Switch? or Select? Number is easiest for now: 0=Auto, 1=Sunny, etc)
    snprintf(state_topic, sizeof(state_topic), "%s/wb_mode", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/wb_mode/set", base_topic);
    publish_discovery("number", "wb_mode", "WB Mode (0=Auto,1=Sun,2=Cloud,3=Office,4=Home)", NULL, NULL, cmd_topic, state_topic, 0, 4);
    
    // PSRAM Free
    snprintf(state_topic, sizeof(state_topic), "%s/psram_free", base_topic);
    publish_discovery("sensor", "psram_free", "PSRAM Free", "data_size", "B", NULL, state_topic, 0, 0);

    // Camera FPS
    snprintf(state_topic, sizeof(state_topic), "%s/fps", base_topic);
    publish_discovery("sensor", "fps", "Camera FPS", NULL, "fps", NULL, state_topic, 0, 0);

    // JPEG Drops
    snprintf(state_topic, sizeof(state_topic), "%s/jpeg_drops", base_topic);
    publish_discovery("sensor", "jpeg_drops", "JPEG Drops", NULL, NULL, NULL, state_topic, 0, 0);

    // NO-SOI Errors
    snprintf(state_topic, sizeof(state_topic), "%s/no_soi", base_topic);
    publish_discovery("sensor", "no_soi", "Frame Errors (NO-SOI)", NULL, NULL, NULL, state_topic, 0, 0);

    // Active Streams
    snprintf(state_topic, sizeof(state_topic), "%s/streams", base_topic);
    publish_discovery("sensor", "streams", "Active Streams", NULL, "streams", NULL, state_topic, 0, 0);

    // OTA Status
    snprintf(state_topic, sizeof(state_topic), "%s/ota_status", base_topic);
    publish_discovery("sensor", "ota_status", "OTA Status", NULL, NULL, NULL, state_topic, 0, 0);

    // Camera Reinit Events
    snprintf(state_topic, sizeof(state_topic), "%s/reinit_count", base_topic);
    publish_discovery("sensor", "reinit_count", "Camera Reinit Events", NULL, NULL, NULL, state_topic, 0, 0);

    // Low Heap (binary sensor — fires when internal SRAM free < 40 KB)
    snprintf(state_topic, sizeof(state_topic), "%s/low_heap", base_topic);
    publish_discovery("binary_sensor", "low_heap", "Low Heap", "problem", NULL, NULL, state_topic, 0, 0);

    // LED (light entity)
    snprintf(state_topic, sizeof(state_topic), "%s/led", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/led/set", base_topic);
    publish_discovery("light", "led", "Onboard LED", NULL, NULL, cmd_topic, state_topic, 0, 0);

    // Min internal heap since boot (useful for detecting slow leaks over uptime)
    snprintf(state_topic, sizeof(state_topic), "%s/heap_min", base_topic);
    publish_discovery("sensor", "heap_min", "Heap Min (internal)", "data_size", "B", NULL, state_topic, 0, 0);

    // FPS Cap (number: 0=unlimited, 1-15)
    snprintf(state_topic, sizeof(state_topic), "%s/fps_cap", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/fps_cap/set", base_topic);
    publish_discovery("number", "fps_cap", "FPS Cap (0=unlimited)", NULL, "fps", cmd_topic, state_topic, 0, 15);

    // Reboot Button
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/restart", base_topic);
    publish_discovery("button", "restart", "Restart Camera", "restart", NULL, cmd_topic, NULL, 0, 0);
}

// ========================================
// Command Handling
// ========================================

static void handle_command(const char *topic_ptr, int topic_len, const char *data, int data_len)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    // Safety copy of topic and data
    char topic[128];
    if (topic_len >= sizeof(topic)) topic_len = sizeof(topic) - 1;
    memcpy(topic, topic_ptr, topic_len);
    topic[topic_len] = '\0';

    int orig_data_len = data_len; // Save before clamping for OTA URL path
    char val_str[16];
    if (data_len >= sizeof(val_str)) data_len = sizeof(val_str) - 1;
    memcpy(val_str, data, data_len);
    val_str[data_len] = '\0';
    int val = atoi(val_str);

    ESP_LOGI(TAG, "Command: %s = %d", topic, val);

    char state_topic[128];

    // config_mgr_set_* here only updates in-memory state (no flash write -- that must never
    // happen while the camera's DMA/capture pipeline is running, see config_mgr.h) so a value
    // set over MQTT is remembered and included in the *next* full save (e.g. from /setup),
    // rather than silently reverting to whatever was last written to NVS on the next reboot.
    if (strstr(topic, "brightness/set")) {
        if (s->set_brightness) s->set_brightness(s, val);
        config_mgr_set_brightness((uint8_t)val);
        snprintf(state_topic, sizeof(state_topic), "%s/brightness", base_topic);
        esp_mqtt_client_publish(client, state_topic, val_str, 0, 0, 0);
    } else if (strstr(topic, "contrast/set")) {
        if (s->set_contrast) s->set_contrast(s, val);
        config_mgr_set_contrast((uint8_t)val);
        snprintf(state_topic, sizeof(state_topic), "%s/contrast", base_topic);
        esp_mqtt_client_publish(client, state_topic, val_str, 0, 0, 0);
    } else if (strstr(topic, "saturation/set")) {
        if (s->set_saturation) s->set_saturation(s, val);
        config_mgr_set_saturation((uint8_t)val);
        snprintf(state_topic, sizeof(state_topic), "%s/saturation", base_topic);
        esp_mqtt_client_publish(client, state_topic, val_str, 0, 0, 0);
    } else if (strstr(topic, "wb_mode/set")) {
        if (s->set_wb_mode) s->set_wb_mode(s, val);
        config_mgr_set_wb_mode((uint8_t)val);
        snprintf(state_topic, sizeof(state_topic), "%s/wb_mode", base_topic);
        esp_mqtt_client_publish(client, state_topic, val_str, 0, 0, 0);
    } else if (strstr(topic, "ota/set")) {
        char raw[300];
        int raw_len = (orig_data_len < (int)sizeof(raw) - 1) ? orig_data_len : (int)sizeof(raw) - 1;
        memcpy(raw, data, raw_len);
        raw[raw_len] = '\0';

        const char *expected_token = config_mgr_get_ota_token();
        char url[256] = {0};
        char sha256_hex[65] = {0};

        if (expected_token && expected_token[0] != '\0') {
            // Token auth required — parse JSON {"url":"...","token":"...",["sha256":"..."]}
            cJSON *root = cJSON_ParseWithLength(raw, raw_len);
            if (!root) {
                ESP_LOGW(TAG, "OTA: payload is not valid JSON — rejecting");
                return;
            }
            cJSON *j_url   = cJSON_GetObjectItem(root, "url");
            cJSON *j_token = cJSON_GetObjectItem(root, "token");
            cJSON *j_sha   = cJSON_GetObjectItem(root, "sha256");

            if (!cJSON_IsString(j_url) || !cJSON_IsString(j_token)) {
                ESP_LOGW(TAG, "OTA: missing url or token field — rejecting");
                cJSON_Delete(root);
                return;
            }
            if (strcmp(j_token->valuestring, expected_token) != 0) {
                ESP_LOGW(TAG, "OTA: token mismatch — rejecting");
                cJSON_Delete(root);
                return;
            }
            strlcpy(url, j_url->valuestring, sizeof(url));
            if (cJSON_IsString(j_sha) && strlen(j_sha->valuestring) == 64) {
                strlcpy(sha256_hex, j_sha->valuestring, sizeof(sha256_hex));
            }
            cJSON_Delete(root);
        } else {
            // No token configured — check if it's JSON anyway (to support SHA-256 in legacy mode)
            if (raw[0] == '{') {
                cJSON *json_root = cJSON_ParseWithLength(raw, raw_len);
                if (json_root) {
                    cJSON *j_url = cJSON_GetObjectItem(json_root, "url");
                    cJSON *j_sha = cJSON_GetObjectItem(json_root, "sha256");
                    if (cJSON_IsString(j_url)) {
                        strlcpy(url, j_url->valuestring, sizeof(url));
                        if (cJSON_IsString(j_sha) && strlen(j_sha->valuestring) == 64) {
                            strlcpy(sha256_hex, j_sha->valuestring, sizeof(sha256_hex));
                        }
                    }
                    cJSON_Delete(json_root);
                }
            }
            
            // If still no URL, it's a bare URL string
            if (url[0] == '\0') {
                strlcpy(url, raw, sizeof(url));
            }
        }

        if (url[0] == '\0') return;
        ESP_LOGI(TAG, "OTA URL accepted: %s", url);
        if (s_ota_cb) s_ota_cb(url, sha256_hex[0] ? sha256_hex : NULL);
    } else if (strstr(topic, "led/set")) {
        int level = (val != 0) ? 1 : 0;
        if (s_led_cb) s_led_cb(level);
        snprintf(state_topic, sizeof(state_topic), "%s/led", base_topic);
        esp_mqtt_client_publish(client, state_topic, level ? "ON" : "OFF", 0, 0, 0);
    } else if (strstr(topic, "fps_cap/set")) {
        uint8_t cap = (val < 0 || val > 15) ? 0 : (uint8_t)val;
        http_server_set_fps_cap(cap);
        snprintf(state_topic, sizeof(state_topic), "%s/fps_cap", base_topic);
        char cap_str[8];
        snprintf(cap_str, sizeof(cap_str), "%u", cap);
        esp_mqtt_client_publish(client, state_topic, cap_str, 0, 0, 1); // retain
    } else if (strstr(topic, "reprovision")) {
        ESP_LOGW(TAG, "Re-provisioning requested via MQTT");
        if (s_reprovision_cb) s_reprovision_cb();
    } else if (strstr(topic, "restart")) {
        ESP_LOGW(TAG, "Restarting via MQTT command...");
        esp_restart();
    }
}

// ========================================
// Telemetry Task
// ========================================

static void mqtt_telemetry_task(void *arg)
{
    // Register this task with Task WDT (30s timeout)
    esp_task_wdt_add(NULL);

    char topic[128];
    char payload[32];

    // For camera FPS calculation
    static uint32_t last_vsync = 0;
    static int64_t last_stats_time = 0;
    // Min internal heap since boot
    static size_t s_min_internal = SIZE_MAX;

    while (1) {
        esp_task_wdt_reset();
        if (s_connected) {
            // RSSI
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                snprintf(topic, sizeof(topic), "%s/rssi", base_topic);
                snprintf(payload, sizeof(payload), "%d", ap_info.rssi);
                esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            }

            // Uptime
            snprintf(topic, sizeof(topic), "%s/uptime", base_topic);
            snprintf(payload, sizeof(payload), "%lld", esp_timer_get_time() / 1000000);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Heap (internal)
            snprintf(topic, sizeof(topic), "%s/heap", base_topic);
            snprintf(payload, sizeof(payload), "%lu", (unsigned long)esp_get_free_heap_size());
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // PSRAM Free
            snprintf(topic, sizeof(topic), "%s/psram_free", base_topic);
            snprintf(payload, sizeof(payload), "%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Camera stats: FPS + NO-SOI
            cam_stats_t cam;
            esp_camera_get_stats(&cam);
            int64_t now = esp_timer_get_time();
            if (last_stats_time > 0) {
                float elapsed = (now - last_stats_time) / 1000000.0f;
                uint32_t d_vsync = cam.vsync_isr_count - last_vsync;
                float fps = (elapsed > 0) ? (d_vsync / elapsed) : 0.0f;
                snprintf(topic, sizeof(topic), "%s/fps", base_topic);
                snprintf(payload, sizeof(payload), "%.1f", fps);
                esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
            }
            last_vsync = cam.vsync_isr_count;
            last_stats_time = now;

            snprintf(topic, sizeof(topic), "%s/no_soi", base_topic);
            snprintf(payload, sizeof(payload), "%lu", (unsigned long)cam.no_soi_count);
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // JPEG Drops
            snprintf(topic, sizeof(topic), "%s/jpeg_drops", base_topic);
            snprintf(payload, sizeof(payload), "%lu",
                     (unsigned long)jpeg_validate_get_drop_count());
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Recovery Events
            snprintf(topic, sizeof(topic), "%s/recovery_count", base_topic);
            snprintf(payload, sizeof(payload), "%d", recovery_mgr_get_error_count());
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Active Streams
            snprintf(topic, sizeof(topic), "%s/streams", base_topic);
            snprintf(payload, sizeof(payload), "%u", http_server_get_active_streams());
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Camera Reinit Events
            snprintf(topic, sizeof(topic), "%s/reinit_count", base_topic);
            snprintf(payload, sizeof(payload), "%lu",
                     (unsigned long)http_server_get_reinit_count());
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);

            // Low Heap alert (ON/OFF binary sensor — threshold 40 KB internal SRAM)
            #define LOW_HEAP_THRESHOLD_BYTES 40000
            size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (internal_free < s_min_internal) s_min_internal = internal_free;
            snprintf(topic, sizeof(topic), "%s/low_heap", base_topic);
            if (internal_free < LOW_HEAP_THRESHOLD_BYTES) {
                ESP_LOGW(TAG, "Low internal heap: %zu bytes", internal_free);
                esp_mqtt_client_publish(client, topic, "ON", 0, 0, 0);
            } else {
                esp_mqtt_client_publish(client, topic, "OFF", 0, 0, 0);
            }

            // Min internal heap since boot
            snprintf(topic, sizeof(topic), "%s/heap_min", base_topic);
            snprintf(payload, sizeof(payload), "%lu",
                     (unsigned long)((s_min_internal == SIZE_MAX) ? internal_free : s_min_internal));
            esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Every 10 seconds
    }
}

// ========================================
// Event Handler
// ========================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT Connected");
        s_connected = true;
        
        // Start telemetry task on first connection
        if (s_telemetry_task == NULL) {
            xTaskCreatePinnedToCore(mqtt_telemetry_task, "mqtt_telemetry", 4096, NULL, 5, &s_telemetry_task, 1);
        }

        // Publish Online Status
        char status_topic[128];
        snprintf(status_topic, sizeof(status_topic), "%s/status", base_topic);
        esp_mqtt_client_publish(client, status_topic, "ON", 0, 1, 1);

        send_ha_discovery();

        // Subscribe to all command topics
        char cmd_sub[128];
        snprintf(cmd_sub, sizeof(cmd_sub), "%s/+/set", base_topic); // Wildcard for settings
        esp_mqtt_client_subscribe(client, cmd_sub, 0);
        
        snprintf(cmd_sub, sizeof(cmd_sub), "%s/restart", base_topic);
        esp_mqtt_client_subscribe(client, cmd_sub, 0);

        snprintf(cmd_sub, sizeof(cmd_sub), "%s/ota/set", base_topic);
        esp_mqtt_client_subscribe(client, cmd_sub, 1); // QoS 1 — ensures delivery survives brief disconnects

        snprintf(cmd_sub, sizeof(cmd_sub), "%s/reprovision", base_topic);
        esp_mqtt_client_subscribe(client, cmd_sub, 1); // QoS 1 — same reasoning as OTA

        snprintf(cmd_sub, sizeof(cmd_sub), "%s/led/set", base_topic);
        esp_mqtt_client_subscribe(client, cmd_sub, 0);

        snprintf(cmd_sub, sizeof(cmd_sub), "%s/fps_cap/set", base_topic);
        esp_mqtt_client_subscribe(client, cmd_sub, 0);

        // Publish Initial State
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/brightness", base_topic);
        esp_mqtt_client_publish(client, topic, "0", 0, 1, 1);
        snprintf(topic, sizeof(topic), "%s/contrast", base_topic);
        esp_mqtt_client_publish(client, topic, "0", 0, 1, 1);
        snprintf(topic, sizeof(topic), "%s/saturation", base_topic);
        esp_mqtt_client_publish(client, topic, "0", 0, 1, 1);
        snprintf(topic, sizeof(topic), "%s/wb_mode", base_topic);
        esp_mqtt_client_publish(client, topic, "0", 0, 1, 1);
        // Fix Unknown sensors: publish placeholder values until telemetry loop fires
        snprintf(topic, sizeof(topic), "%s/ota_status", base_topic);
        esp_mqtt_client_publish(client, topic, "idle", 0, 1, 1);
        snprintf(topic, sizeof(topic), "%s/recovery_count", base_topic);
        esp_mqtt_client_publish(client, topic, "0", 0, 1, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len, event->data, event->data_len);
        break;

    default:
        break;
    }
}

// ========================================
// Public API
// ========================================

esp_err_t mqtt_mgr_start(const char *broker_url, const char *username, const char *password, const char *dev_id)
{
    strlcpy(device_id, (dev_id && dev_id[0]) ? dev_id : CONFIG_UNITCAMS3_DEVICE_ID, sizeof(device_id));
    snprintf(base_topic, sizeof(base_topic), "%s", device_id);

    char lwt_topic[80];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_url,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "OFF",
            .qos = 1,
            .retain = 1
        },
        .session.keepalive = 60,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) return ESP_FAIL;

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    return ESP_OK;
}

esp_err_t mqtt_mgr_publish(const char *topic_suffix, const char *value)
{
    if (!s_connected || !client) return ESP_FAIL;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", base_topic, topic_suffix);
    esp_mqtt_client_publish(client, topic, value, 0, 0, 0);
    return ESP_OK;
}

void mqtt_mgr_stop(void)
{
    if (client) {
        ESP_LOGW(TAG, "Stopping MQTT client...");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
        s_connected = false;
    }
    if (s_telemetry_task) {
        ESP_LOGW(TAG, "Deleting MQTT telemetry task...");
        esp_task_wdt_delete(s_telemetry_task); // Unregister before delete — avoids WDT panic
        vTaskDelete(s_telemetry_task);
        s_telemetry_task = NULL;
    }
}
