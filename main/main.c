#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h" // Required for esp_timer_get_time()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "hw_validate.h"
#include "esp_camera.h"

// Phase 2 Headers
#include "wifi.h"
#include "http_server.h"
#include "frame_pool.h"
#include "mqtt_mgr.h"
#include "recovery_mgr.h" // Added

// Phase 8 Headers
#include "ota_mgr.h"
#include "config_mgr.h"
#include "neopixel_mgr.h"
#include "led_mgr.h"
#include "mdns.h"
#include "log_buf.h"
#include "nvs_flash.h"

#if CONFIG_UNITCAMS3_BOARD_OV3660
// ========================================
// Generic ESP32-S3-CAM (OV3660) Pin Map
// Matches the Goouuu-Cam pinout: SIOD=4 SIOC=5 VSYNC=6 HREF=7 XCLK=15 PCLK=13
// Y9..Y2 = 16,17,18,12,10,8,9,11 — confirmed against board silkscreen/schematic,
// NOT hardware-validated on this firmware (untested ISR/VSYNC timing, see CLAUDE.md).
// ========================================
// Was 20MHz (divides the 160MHz LCD_CAM source evenly, 160/20=8, and is the commonly-used
// OV3660 XCLK) -- doesn't corrupt frames on this firmware's ISR (no_soi/no_eoi stayed at 0), but
// at 20MHz this sensor free-runs at ~18-20fps, roughly double PY260's validated 10MHz/~9.4fps
// ceiling. That's roughly double the VSYNC/GDMA-EOF interrupt rate hammering Core 1, which also
// hosts the MJPEG broadcaster/worker tasks and the Wi-Fi driver -- confirmed via /stats: capture
// stayed clean at 20MHz, but delivered stream fps collapsed to a fraction of a frame per second
// and didn't meaningfully recover even at QVGA (i.e. it wasn't payload-size-bound). Dropping to
// PY260's validated 10MHz halves that interrupt rate so the delivery-side tasks actually get Core
// 1 time, trading a lower native capture rate for one that's actually deliverable end to end.
#define CAM_XCLK_FREQ_HZ 10000000

// Resolution Configuration: FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XVGA, FRAMESIZE_UXGA
#define CAM_FRAME_SIZE   FRAMESIZE_VGA

static const char *TAG = "main";

#define CAM_PIN_PWDN    -1 // Not connected
#define CAM_PIN_RESET   -1 // Not connected
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4  // SDA
#define CAM_PIN_SIOC    5  // SCL

#define CAM_PIN_D7      16 // Y9
#define CAM_PIN_D6      17 // Y8
#define CAM_PIN_D5      18 // Y7
#define CAM_PIN_D4      12 // Y6
#define CAM_PIN_D3      10 // Y5
#define CAM_PIN_D2      8  // Y4
#define CAM_PIN_D1      9  // Y3
#define CAM_PIN_D0      11 // Y2
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13
#else
// ========================================
// Phase 1: UnitCamS3 Pin Map
// ========================================
// 10MHz: hard ceiling — PY260 front porch too short at 16/20MHz regardless of ISR weight
#define CAM_XCLK_FREQ_HZ 10000000

// Resolution Configuration: FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XVGA, FRAMESIZE_UXGA
#define CAM_FRAME_SIZE   FRAMESIZE_VGA

static const char *TAG = "main";

#define CAM_PIN_PWDN    -1 // Tied to GND
#define CAM_PIN_RESET   21
#define CAM_PIN_XCLK    11
#define CAM_PIN_SIOD    17 // SDA
#define CAM_PIN_SIOC    41 // SCL

#define CAM_PIN_D7      13 // Y9
#define CAM_PIN_D6      4  // Y8
#define CAM_PIN_D5      10 // Y7
#define CAM_PIN_D4      5  // Y6
#define CAM_PIN_D3      7  // Y5
#define CAM_PIN_D2      16 // Y4
#define CAM_PIN_D1      15 // Y3
#define CAM_PIN_D0      6  // Y2
#define CAM_PIN_VSYNC   42
#define CAM_PIN_HREF    18
#define CAM_PIN_PCLK    12
#endif

// ========================================
// Camera Configuration
// ========================================
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = CAM_FRAME_SIZE,
    .jpeg_quality = 12,
    .fb_count = 4,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST
};

// ========================================
// Camera Image Quality Configuration
// ========================================
// Special Effect: 0 = No Effect, 1 = Negative, 2 = Grayscale, 3 = Red Tint, 4 = Green Tint, 5 = Blue Tint, 6 = Sepia
#define CAM_SPECIAL_EFFECT  0
// Auto Exposure Control: 0 = Disable, 1 = Enable
#define CAM_AEC             1

static void apply_camera_settings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Sensor not found, cannot apply settings");
        return;
    }

    ESP_LOGI(TAG, "Applying camera settings...");

    // config_mgr's brightness/contrast/saturation/wb_mode range and neutral point track
    // whichever sensor CONFIG_UNITCAMS3_BOARD_* selects -- see config_mgr.h/config_mgr.c.
    if (s->set_brightness) s->set_brightness(s, config_mgr_get_brightness());
    if (s->set_contrast) s->set_contrast(s, config_mgr_get_contrast());
    if (s->set_saturation) s->set_saturation(s, config_mgr_get_saturation());
    if (s->set_special_effect) s->set_special_effect(s, CAM_SPECIAL_EFFECT);
    if (s->set_wb_mode) s->set_wb_mode(s, config_mgr_get_wb_mode());
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, CAM_AEC);
    // AE level: EV-style bias on top of AEC (still auto, just targets brighter/darker), not a
    // manual exposure override. Useful when a bright glare/hotspot in frame fools AEC's metering
    // into underexposing everything else. Only ov3660.c implements this -- PY260/mega_ccm wires
    // set_ae_level to a no-op, so this is inert (and harmless) on that board.
    if (s->set_ae_level) s->set_ae_level(s, config_mgr_get_exposure());
    // set_whitebal, set_awb_gain, set_gain_ctrl are not implemented by PY260 (wired to set_dummy)

    ESP_LOGI(TAG, "Camera settings applied");
}

SemaphoreHandle_t g_camera_reinit_mutex = NULL;

esp_err_t camera_reinit(void)
{
    if (!g_camera_reinit_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_camera_reinit_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Camera reinit already in progress, waiting...");
        xSemaphoreTake(g_camera_reinit_mutex, portMAX_DELAY);
        xSemaphoreGive(g_camera_reinit_mutex);
        return ESP_OK; // Another task just fixed it
    }

    ESP_LOGW(TAG, "Reinitializing camera driver...");
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Hardware reset
    gpio_set_level(CAM_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CAM_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera reinit failed: 0x%x", err);
        xSemaphoreGive(g_camera_reinit_mutex);
        return err;
    }

    apply_camera_settings();

    // Let sensor stabilize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Discard first frame (often corrupt)
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "Camera reinit successful");
    xSemaphoreGive(g_camera_reinit_mutex);
    return ESP_OK;
}

void app_main(void)
{
    g_camera_reinit_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  UnitCamS3-5MP Custom Firmware v2");
    ESP_LOGI(TAG, "  Boot Sequence Started");
    ESP_LOGI(TAG, "========================================");

    /* Phase 0: Validate PSRAM */
    esp_err_t err = hw_validate_psram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PSRAM validation FAILED. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    log_buf_init(); // Start capturing logs to PSRAM ring buffer (non-fatal if PSRAM unavailable)

    // Initialize NVS here, before recovery_mgr_init() and config_mgr_init() read
    // it. Previously the only nvs_flash_init() lived inside wifi_init_sta(), which
    // runs AFTER recovery_mgr_init() — so recovery's nvs_open() failed every boot,
    // the health timer never armed, and esp_ota_mark_app_valid_cancel_rollback()
    // was never called → an OTA'd image would roll back on the next reboot. This
    // is a flash READ/erase only and runs before the camera starts, so it is safe.
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (err=0x%x) — erasing and reinitializing", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x — recovery/config NVS will fall back to defaults", err);
    }

    ESP_LOGI(TAG, "--- Initializing Frame Pool ---");
    // 8 buffers x 512KB = 4.0MB fixed PSRAM usage
    // Sized to support up to UXGA (1600x1200) high-quality JPEGs
    // 5 slots × 512 KB = 2.5 MB PSRAM.
    // Normal peak usage is 2 slots (s_broadcast_fb + 1 worker mid-send, all
    // workers share the same ref). A simultaneous snapshot (GET /) holds a ref
    // to the current broadcast frame while sending the HTTP response; combined
    // with a worker mid-send on the previous frame, this can exhaust a 3-slot
    // pool and stall the broadcaster for up to ~5s → Frigate 20s watchdog fires.
    // 5 slots eliminates the exhaustion: snapshot(1) + worker(1) + broadcaster(1)
    // still leaves 2 free slots for the broadcaster to cycle into.
    err = frame_pool_init(5, 512 * 1024);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Frame Pool Init Failed!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* Phase 2: Network & Services */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 2: Network & Services");
    ESP_LOGI(TAG, "========================================");

    // Recovery manager writes NVS (nvs_commit) — must run BEFORE Wi-Fi and MQTT
    // are started. nvs_commit disables the OPI PSRAM cache; any concurrent PSRAM
    // access by Wi-Fi/MQTT tasks causes ExcCause=7 → crash. Running it here, while
    // no PSRAM-accessing tasks are alive, is safe.
    ESP_LOGI(TAG, "--- Starting Recovery Manager ---");
    err = recovery_mgr_init(NULL); // Use defaults
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recovery Mgr Init Failed!");
    }

    ESP_LOGI(TAG, "--- Starting Wi-Fi ---");
    err = wifi_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed! Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Check for pending OTA *before* the camera is initialized.
    // If a URL was saved to NVS by the previous boot (via MQTT trigger),
    // download + flash it now and reboot into the new firmware.
    // Camera DMA is never started, so flash operations cannot race with it.
    ESP_LOGI(TAG, "--- Checking for pending OTA ---");
    ota_mgr_run_pending(); // returns only if no pending OTA (or OTA failed)

    ESP_LOGI(TAG, "--- Loading Config ---");
    config_mgr_init();

    ESP_LOGI(TAG, "--- NeoPixel Ring ---");
    neopixel_mgr_init(); // no-op unless enabled in config -- see neopixel_mgr.h

    ESP_LOGI(TAG, "--- Onboard LED ---");
    led_mgr_init(); // configures the onboard WS2812 status LED and restores its persisted on/off state

    ESP_LOGI(TAG, "--- Starting mDNS ---");
    mdns_init();
    mdns_hostname_set(config_mgr_get_device_id());
    mdns_instance_name_set("M5Stack UnitCamS3-5MP");
    mdns_service_add("HTTP", "_http", "_tcp", 80, NULL, 0);
    mdns_service_add("Stream", "_http", "_tcp", 81, NULL, 0);
    ESP_LOGI(TAG, "mDNS hostname: %s.local", config_mgr_get_device_id());

    ESP_LOGI(TAG, "--- Starting MQTT Manager ---");
    if (config_mgr_is_mqtt_enabled()) {
        err = mqtt_mgr_start(
            config_mgr_get_mqtt_url(),
            config_mgr_get_mqtt_user(),
            config_mgr_get_mqtt_pass(),
            config_mgr_get_device_id());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT Init Failed! Continuing...");
        }
        mqtt_mgr_register_reprovision_callback(wifi_start_reprovision);
        mqtt_mgr_register_led_callback(led_mgr_set_level);
    } else {
        ESP_LOGI(TAG, "MQTT disabled by config — skipping");
    }

    /* Phase 1: Initialize Camera AFTER Wi-Fi/NVS are settled.
     * esp_camera_init() enables the VSYNC ISR (cam_start=1). If it runs
     * concurrently with Wi-Fi's NVS flash reads (which disable the OPI cache
     * bus), the ISR touching GDMA/LCD_CAM registers causes a double exception.
     * Initializing here guarantees the cache is stable before the ISR goes live.
     */
    ESP_LOGI(TAG, "--- Initializing Camera Driver ---");
    camera_config.frame_size   = config_mgr_get_cam_resolution();
    camera_config.jpeg_quality = config_mgr_get_jpeg_quality();
    err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed! Error: 0x%x", err);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    apply_camera_settings();

    // Prime DMA pipeline — Wi-Fi is fully connected, OPI bus is stable
    ESP_LOGI(TAG, "--- Priming camera pipeline ---");
    for (int attempt = 0; attempt < 4; attempt++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGI(TAG, "Warmup frame captured (%zu bytes, attempt %d)", fb->len, attempt + 1);
            esp_camera_fb_return(fb);
            break;
        }
        ESP_LOGW(TAG, "Warmup capture failed (attempt %d), reinitializing...", attempt + 1);
        camera_reinit();
    }

#ifdef CONFIG_UNITCAMS3_FPS_TEST_MODE
    ESP_LOGI(TAG, "--- ENTERING FPS TEST MODE ---");
    ESP_LOGW(TAG, "HTTP Servers will NOT start.");

    int64_t last_time = esp_timer_get_time();
    int frame_count = 0;
    size_t total_bytes = 0;
    cam_stats_t last_stats = {0};

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            frame_count++;
            total_bytes += fb->len;
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "FPS Test: Frame drop / timeout");
        }

        int64_t now = esp_timer_get_time();
        if ((now - last_time) >= 5000000) { // Every 5 seconds
            float elapsed = (now - last_time) / 1000000.0f;
            float fps = frame_count / elapsed;
            float kbps = (total_bytes / 1024.0f) / elapsed;

            cam_stats_t cur = {0};
            esp_camera_get_stats(&cur);
            uint32_t d_vsync  = cur.vsync_isr_count       - last_stats.vsync_isr_count;
            uint32_t d_start  = cur.start_trigger_count    - last_stats.start_trigger_count;
            uint32_t d_eof    = cur.eof_count              - last_stats.eof_count;
            uint32_t d_nosoi  = cur.no_soi_count           - last_stats.no_soi_count;
            uint32_t d_noeoi  = cur.no_eoi_count           - last_stats.no_eoi_count;
            uint32_t d_drops  = cur.drops_no_free_buf      - last_stats.drops_no_free_buf;
            uint32_t d_qovf   = cur.queue_overflow_count   - last_stats.queue_overflow_count;

            uint32_t d_partial = cur.partial_chunk_bytes - last_stats.partial_chunk_bytes;
            ESP_LOGI(TAG, "FPS: %.2f | %.1f KB/s | frames=%d", fps, kbps, frame_count);
            ESP_LOGI(TAG, "  VSYNC=%lu START=%lu EOF=%lu | NO-SOI=%lu NO-EOI=%lu | drops=%lu qovf=%lu | partial_bytes=%lu",
                     (unsigned long)d_vsync, (unsigned long)d_start, (unsigned long)d_eof,
                     (unsigned long)d_nosoi, (unsigned long)d_noeoi,
                     (unsigned long)d_drops, (unsigned long)d_qovf,
                     (unsigned long)d_partial);

            last_stats = cur;
            last_time = now;
            frame_count = 0;
            total_bytes = 0;
        }

        vTaskDelay(1);
    }
#endif

    ESP_LOGI(TAG, "--- Starting HTTP Server (Port 80) ---");
    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server failed to start! Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "--- Starting Stream Server (Port 81) ---");
    err = start_stream_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream Server failed to start! Continuing...");
    }

    ESP_LOGI(TAG, "--- Initializing OTA Manager ---");
    err = ota_mgr_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA Manager init failed! Continuing...");
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  SYSTEM READY");
    ESP_LOGI(TAG, "  HTTP: port 80  |  Stream: port 81");
    ESP_LOGI(TAG, "========================================");
}
