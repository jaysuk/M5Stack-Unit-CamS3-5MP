#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "lwip/sockets.h"
#include "jpeg_validate.h"
#include "frame_pool.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include "esp_task_wdt.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "config_mgr.h"
#include "log_buf.h"
#include "recovery_mgr.h"
#include "ota_mgr.h"
#include "neopixel_mgr.h"
#include "led_mgr.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;
static httpd_handle_t s_stream_server = NULL;
static volatile bool s_ota_pending = false;

#define MJPEG_BOUNDARY "frame"
#define STREAM_PART_HDR "--" MJPEG_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n"

// Multi-client MJPEG structures
typedef struct {
    httpd_req_t *req;
    TaskHandle_t task;
    SemaphoreHandle_t sync_sem;
    bool active;
    int slot; // Index into s_stream_clients — used to record per-slot HWM at exit
} mjpeg_client_t;

#define MAX_STREAM_CLIENTS 5
static mjpeg_client_t s_stream_clients[MAX_STREAM_CLIENTS];
static uint32_t s_worker_hwm_min_words[MAX_STREAM_CLIENTS] = {0}; // min HWM per slot (0 = not yet run)
static SemaphoreHandle_t s_clients_mutex = NULL;

static frame_buffer_t *s_broadcast_fb = NULL; // Current latest frame
static SemaphoreHandle_t s_broadcast_mutex = NULL;
static uint32_t s_broadcast_frame_id = 0;
static TaskHandle_t s_broadcaster_task_handle = NULL;

// Observability counters
static _Atomic uint32_t s_reinit_count = 0;     // camera_reinit() calls from broadcaster
static _Atomic uint32_t s_frames_delivered = 0; // successful worker frame sends (all clients)
static uint32_t s_broadcaster_hwm_words = 0;    // broadcaster stack HWM, updated every 100 frames

/// FPS cap: 0 = unlimited, 1-15 = max frames/sec delivered to stream clients
static _Atomic uint8_t s_fps_cap = 0;

void http_server_set_fps_cap(uint8_t fps_cap) {
    atomic_store(&s_fps_cap, fps_cap);
    ESP_LOGI("http", "FPS cap set to %u (%s)", fps_cap, fps_cap ? "limited" : "unlimited");
}

uint32_t http_server_get_reinit_count(void) { return atomic_load(&s_reinit_count); }

// Forward declaration - defined in main.c
extern esp_err_t camera_reinit(void);

// Boot timestamp for uptime calculation
static int64_t s_boot_time_us;

// Handler for the "/" endpoint — returns a single JPEG snapshot
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Snapshot requested");

    if (s_ota_pending) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    frame_buffer_t *pool_fb = NULL;
    camera_fb_t *cam_fb = NULL;

    // 1. Try to grab a reference to the active stream frame (Zero hardware contention)
    if (s_broadcast_mutex) {
        xSemaphoreTake(s_broadcast_mutex, portMAX_DELAY);
        if (s_broadcast_fb) {
            pool_fb = frame_pool_ref(s_broadcast_fb);
        }
        xSemaphoreGive(s_broadcast_mutex);
    }

    // 2. If no stream is active, capture directly from camera
    if (!pool_fb) {
        cam_fb = esp_camera_fb_get();
        if (!cam_fb) {
            ESP_LOGW(TAG, "Snapshot: failed to get frame, reinitializing...");
            esp_err_t err = camera_reinit();
            if (err == ESP_OK) {
                cam_fb = esp_camera_fb_get();
            }
        }
        if (!cam_fb) {
            ESP_LOGE(TAG, "Snapshot: camera capture failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        // Validate JPEG integrity
        if (!jpeg_validate_frame(cam_fb)) {
            ESP_LOGW(TAG, "Invalid JPEG, retrying once...");
            esp_camera_fb_return(cam_fb);
            cam_fb = esp_camera_fb_get();
            if (!cam_fb || !jpeg_validate_frame(cam_fb)) {
                if (cam_fb) esp_camera_fb_return(cam_fb);
                ESP_LOGE(TAG, "JPEG validation failed");
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }
    }

    // 3. Set headers
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    // 4. Send the payload
    esp_err_t res;
    size_t sent_len = 0;
    
    if (pool_fb) {
        res = httpd_resp_send(req, (const char *)pool_fb->buf, pool_fb->len);
        sent_len = pool_fb->len;
        frame_pool_unref(pool_fb);
    } else {
        res = httpd_resp_send(req, (const char *)cam_fb->buf, cam_fb->len);
        sent_len = cam_fb->len;
        esp_camera_fb_return(cam_fb);
    }

    ESP_LOGI(TAG, "JPEG sent (%zu bytes, heap free: %lu)",
             sent_len, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    return res;
}

// Handler for "/health" endpoint
static esp_err_t health_handler(httpd_req_t *req)
{
    int64_t uptime_s = (esp_timer_get_time() - s_boot_time_us) / 1000000;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t drops = jpeg_validate_get_drop_count();

    const esp_app_desc_t *app = esp_app_get_description();
    char sha256_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha256_hex + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }

    static const char *reset_reasons[] = {
        "unknown", "power_on", "external", "software",
        "panic", "int_watchdog", "task_watchdog", "watchdog",
        "deep_sleep", "brownout", "sdio", "usb", "jtag",
        "efuse", "pwr_glitch", "cpu_lockup"
    };
    esp_reset_reason_t rr = esp_reset_reason();
    const char *reset_str = (rr < (esp_reset_reason_t)(sizeof(reset_reasons)/sizeof(reset_reasons[0])))
                            ? reset_reasons[rr] : "unknown";

    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"uptime_s\":%lld,"
        "\"version\":\"%s\","
        "\"heap_free\":%zu,"
        "\"internal_free\":%zu,"
        "\"psram_free\":%zu,"
        "\"jpeg_drops\":%lu,"
        "\"reset_reason\":\"%s\","
        "\"app_sha256\":\"%s\"}",
        (long long)uptime_s, app->version, free_heap, free_internal, free_psram,
        (unsigned long)drops, reset_str, sha256_hex);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

// Handler for "/stats" endpoint
static esp_err_t stats_handler(httpd_req_t *req)
{
    static uint32_t last_vsync = 0;
    static uint32_t last_broadcast_id = 0;
    static int64_t last_time = 0;
    static float vsync_fps = 0;
    static float broadcast_fps = 0;
    // Min heap since boot — updated on every /stats call
    static size_t min_internal = SIZE_MAX;
    static size_t min_psram = SIZE_MAX;

    cam_stats_t cam;
    wifi_stats_t wifi;
    esp_camera_get_stats(&cam);
    wifi_get_stats(&wifi);

    int64_t now = esp_timer_get_time();
    int64_t uptime_s = now / 1000000;
    if (last_time > 0) {
        float elapsed = (now - last_time) / 1000000.0f;
        vsync_fps = (float)(cam.vsync_isr_count - last_vsync) / elapsed;
        broadcast_fps = (float)(s_broadcast_frame_id - last_broadcast_id) / elapsed;
    }
    last_vsync = cam.vsync_isr_count;
    last_broadcast_id = s_broadcast_frame_id;
    last_time = now;

    size_t cur_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t cur_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (cur_internal < min_internal) min_internal = cur_internal;
    if (cur_psram    < min_psram)    min_psram    = cur_psram;

    // Stack HWM: HTTP server task (live) and broadcaster (sampled every 100 frames)
    uint32_t http_task_hwm = uxTaskGetStackHighWaterMark(NULL);

    // Worker HWM: minimum across all slots that have completed at least one session
    uint32_t worker_hwm_min = 0;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_worker_hwm_min_words[i] > 0) {
            if (worker_hwm_min == 0 || s_worker_hwm_min_words[i] < worker_hwm_min) {
                worker_hwm_min = s_worker_hwm_min_words[i];
            }
        }
    }

    char *buf = heap_caps_malloc(1800, MALLOC_CAP_DEFAULT);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int len = snprintf(buf, 1800,
        "{"
        "\"uptime_s\":%lld,"
        "\"camera\":{"
            "\"fps\":%.2f,"
            "\"broadcast_fps\":%.2f,"
            "\"vsync_count\":%lu,"
            "\"eof_count\":%lu,"
            "\"no_soi\":%lu,"
            "\"no_eoi\":%lu,"
            "\"queue_overflow\":%lu,"
            "\"drops_no_buf\":%lu,"
            "\"frames_via_recovery\":%lu,"
            "\"reinit_count\":%lu,"
            "\"active_streams\":%u,"
            "\"frames_delivered\":%lu,"
            "\"soi_hist\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]"
        "},"
        "\"wifi\":{"
            "\"rssi\":%d,"
            "\"disconnects\":%lu,"
            "\"ip\":\"%s\""
        "},"
        "\"memory\":{"
            "\"internal_free\":%zu,"
            "\"psram_free\":%zu,"
            "\"internal_min\":%zu,"
            "\"psram_min\":%zu"
        "},"
        "\"stack_hwm\":{"
            "\"broadcaster_words\":%lu,"
            "\"http_task_words\":%lu,"
            "\"worker_min_words\":%lu"
        "},"
        "\"fps_cap\":%u"
        "}",
        (long long)uptime_s,
        vsync_fps, broadcast_fps,
        (unsigned long)cam.vsync_isr_count, (unsigned long)cam.eof_count,
        (unsigned long)cam.no_soi_count, (unsigned long)cam.no_eoi_count,
        (unsigned long)cam.queue_overflow_count, (unsigned long)cam.drops_no_free_buf,
        (unsigned long)cam.frames_via_recovery,
        (unsigned long)atomic_load(&s_reinit_count),
        http_server_get_active_streams(),
        (unsigned long)atomic_load(&s_frames_delivered),
        (unsigned long)cam.soi_offset_histogram[0], (unsigned long)cam.soi_offset_histogram[1],
        (unsigned long)cam.soi_offset_histogram[2], (unsigned long)cam.soi_offset_histogram[3],
        (unsigned long)cam.soi_offset_histogram[4], (unsigned long)cam.soi_offset_histogram[5],
        (unsigned long)cam.soi_offset_histogram[6], (unsigned long)cam.soi_offset_histogram[7],
        wifi.rssi, (unsigned long)wifi.disconnect_count, wifi.ip,
        cur_internal, cur_psram,
        (min_internal == SIZE_MAX) ? cur_internal : min_internal,
        (min_psram    == SIZE_MAX) ? cur_psram    : min_psram,
        (unsigned long)s_broadcaster_hwm_words,
        (unsigned long)http_task_hwm,
        (unsigned long)worker_hwm_min,
        (unsigned)atomic_load(&s_fps_cap));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, buf, len);
    free(buf);
    return ret;
}

// ========================================
// Multi-client MJPEG Tasks
// ========================================

static void mjpeg_broadcaster_task(void *arg)
{
    ESP_LOGI(TAG, "MJPEG broadcaster task started on Core 1");
    esp_task_wdt_add(NULL);
    uint32_t fail_count = 0;

    while (1) {
        esp_task_wdt_reset();
        if (s_ota_pending) goto broadcaster_exit;

        // Check for active clients before capturing
        bool has_clients = false;
        if (s_clients_mutex) {
            xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
            for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
                if (s_stream_clients[i].active) {
                    has_clients = true;
                    break;
                }
            }
            xSemaphoreGive(s_clients_mutex);
        }

        if (!has_clients) {
            // Idle if no one is watching
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (s_ota_pending) {
            if (fb) esp_camera_fb_return(fb);
            goto broadcaster_exit;
        }

        if (!fb) {
            fail_count++;
            if (fail_count >= 10) {
                ESP_LOGE(TAG, "Broadcaster: hard camera stall, reinit...");
                atomic_fetch_add(&s_reinit_count, 1);
                camera_reinit();
                fail_count = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        fail_count = 0;

        // Sample our own stack HWM every 100 frames (cheap enough, no lock needed)
        if (s_broadcast_frame_id % 100 == 0) {
            s_broadcaster_hwm_words = uxTaskGetStackHighWaterMark(NULL);
        }

        // Basic JPEG validation
        if (fb->len < 100 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
            esp_camera_fb_return(fb);
            continue;
        }

        // Copy to a new pool buffer for broadcast
        frame_buffer_t *new_fb = frame_pool_get(100); // 100ms timeout
        if (new_fb && fb->len > new_fb->capacity) {
            // Pool buffers are sized for the resolutions this board actually captures (measured
            // up to ~100KB at UXGA against a 512KB buffer); this only guards against a JPEG that
            // somehow exceeds that, since memcpy below has no bounds of its own and would corrupt
            // adjacent PSRAM heap otherwise.
            ESP_LOGE(TAG, "Captured frame (%zu B) exceeds pool buffer capacity (%zu B), dropping",
                     fb->len, new_fb->capacity);
            frame_pool_unref(new_fb);
            new_fb = NULL;
        }
        if (new_fb) {
            memcpy(new_fb->buf, fb->buf, fb->len);
            new_fb->len = fb->len;

            // Swap global pointer
            xSemaphoreTake(s_broadcast_mutex, portMAX_DELAY);
            frame_buffer_t *old_fb = s_broadcast_fb;
            s_broadcast_fb = new_fb; // Broadcaster's ref_count=1 transfers to global
            s_broadcast_frame_id++;
            xSemaphoreGive(s_broadcast_mutex);

            // Release old frame (if no workers hold it, it returns to pool)
            if (old_fb) frame_pool_unref(old_fb);

            // Signal all clients
            if (s_clients_mutex) {
                xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
                for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
                    if (s_stream_clients[i].active && s_stream_clients[i].sync_sem) {
                        xSemaphoreGive(s_stream_clients[i].sync_sem);
                    }
                }
                xSemaphoreGive(s_clients_mutex);
            }
        }

        esp_camera_fb_return(fb);

        // FPS cap pacing — sleep the remainder of the frame interval after each delivery.
        // CAMERA_GRAB_LATEST discards stale frames while we sleep, so next get() returns
        // the freshest available frame. Reduces CPU and OPI bus load when clients need
        // fewer fps than the camera's native ~9.4 fps.
        {
            static int64_t s_last_broadcast_us = 0;
            uint8_t cap = atomic_load(&s_fps_cap);
            if (cap > 0 && s_last_broadcast_us > 0) {
                int64_t interval_us = 1000000LL / cap;
                int64_t now = esp_timer_get_time();
                int64_t wait_us = (s_last_broadcast_us + interval_us) - now;
                if (wait_us > 2000) { // only sleep if > 2ms remaining
                    vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
                }
            }
            s_last_broadcast_us = esp_timer_get_time();
        }
    }

broadcaster_exit:
    ESP_LOGW(TAG, "Broadcaster: shutting down");
    esp_task_wdt_delete(NULL);
    xSemaphoreTake(s_broadcast_mutex, portMAX_DELAY);
    if (s_broadcast_fb) {
        frame_pool_unref(s_broadcast_fb);
        s_broadcast_fb = NULL;
    }
    xSemaphoreGive(s_broadcast_mutex);
    s_broadcaster_task_handle = NULL;
    vTaskDelete(NULL);
}

static void mjpeg_client_worker_task(void *arg)
{
    mjpeg_client_t *client = (mjpeg_client_t *)arg;
    httpd_req_t *req = client->req;
    int sockfd = httpd_req_to_sockfd(req);

    ESP_LOGI(TAG, "MJPEG worker started for socket %d", sockfd);

    // Set socket send timeout to gracefully handle stalled clients
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Each frame goes out as three separate chunk writes below (small multipart header, the JPEG
    // payload, then a 2-byte "\r\n" trailer). Without TCP_NODELAY, Nagle's algorithm holds those small
    // writes back waiting to coalesce with more outbound data or for the peer's ACK, which stalls on
    // essentially every frame -- this is what was turning ~18fps of valid captures (see /stats
    // vsync_count) into well under 1fps actually reaching the browser.
    int nodelay = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set headers (part of the async response)
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    char part_hdr[128];
    uint32_t frame_count = 0;
    uint32_t last_sent_id = 0;
    esp_err_t res = ESP_OK;

    while (res == ESP_OK && !s_ota_pending) {
        // Wait for broadcaster signal (up to 1s)
        if (xSemaphoreTake(client->sync_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue; // No new frame within timeout
        }

        frame_buffer_t *local_fb = NULL;

        // Grab reference to current broadcast frame
        xSemaphoreTake(s_broadcast_mutex, portMAX_DELAY);
        if (s_broadcast_fb && s_broadcast_frame_id != last_sent_id) {
            local_fb = frame_pool_ref(s_broadcast_fb);
            last_sent_id = s_broadcast_frame_id;
        }
        xSemaphoreGive(s_broadcast_mutex);

        if (!local_fb) continue;

        // Send multipart boundary + headers
        int hdr_len = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART_HDR, local_fb->len);
        res = httpd_resp_send_chunk(req, part_hdr, hdr_len);

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)local_fb->buf, local_fb->len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        // Release reference (returns to pool if broadcaster and all other workers are done)
        frame_pool_unref(local_fb);

        if (res == ESP_OK) {
            atomic_fetch_add(&s_frames_delivered, 1);
        }
        frame_count++;
    }

    // Record stack HWM for this slot before the task exits
    uint32_t hwm = uxTaskGetStackHighWaterMark(NULL);
    int my_slot = client->slot;
    if (my_slot >= 0 && my_slot < MAX_STREAM_CLIENTS) {
        if (s_worker_hwm_min_words[my_slot] == 0 || hwm < s_worker_hwm_min_words[my_slot]) {
            s_worker_hwm_min_words[my_slot] = hwm;
        }
    }

    ESP_LOGI(TAG, "MJPEG worker slot %d stopping (frames: %lu, hwm: %lu words)",
             my_slot, (unsigned long)frame_count, (unsigned long)hwm);

    // Free the request (important for async)
    httpd_req_async_handler_complete(req);

    // Unregister client
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    client->active = false;
    client->req = NULL;
    client->task = NULL;
    // We keep the sync_sem for reuse
    xSemaphoreGive(s_clients_mutex);

    vTaskDelete(NULL);
}

// ========================================
// MJPEG Stream Handler — /stream
// ========================================
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!s_clients_mutex) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (!s_stream_clients[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        xSemaphoreGive(s_clients_mutex);
        ESP_LOGW(TAG, "Stream server full (max %d clients)", MAX_STREAM_CLIENTS);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Too many clients");
        return ESP_FAIL;
    }

    // Allocate sem if not already there
    if (!s_stream_clients[slot].sync_sem) {
        s_stream_clients[slot].sync_sem = xSemaphoreCreateBinary();
    }

    // Begin async response
    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        xSemaphoreGive(s_clients_mutex);
        ESP_LOGE(TAG, "Async handler begin failed");
        return err;
    }

    s_stream_clients[slot].req = copy;
    s_stream_clients[slot].active = true;
    s_stream_clients[slot].slot = slot;

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "mjpeg_cl_%d", slot);
    BaseType_t res = xTaskCreatePinnedToCore(
        mjpeg_client_worker_task, task_name, 8192,
        &s_stream_clients[slot], 5, &s_stream_clients[slot].task, 1);

    if (res != pdPASS) {
        s_stream_clients[slot].active = false;
        httpd_req_async_handler_complete(copy);
        xSemaphoreGive(s_clients_mutex);
        ESP_LOGE(TAG, "Failed to create client task");
        return ESP_FAIL;
    }

    xSemaphoreGive(s_clients_mutex);
    return ESP_OK;
}


// Handler for "/api/logs" — return ring-buffered log output as plain text
static esp_err_t logs_handler(httpd_req_t *req)
{
    size_t len;
    char *snap = log_buf_snapshot(&len);
    if (!snap) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Log buffer unavailable");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, snap, (ssize_t)len);
    free(snap);
    return err;
}

// Handler for "/api/neopixel" (GET) — capability + current state as JSON. duet-tool-align (or
// any other consumer) polls this before showing any light control at all: "enabled" is the
// /setup master switch, "active" additionally confirms the driver actually came up (e.g. still
// false if enabled but led_strip_new_rmt_device() failed).
static esp_err_t neopixel_get_handler(httpd_req_t *req)
{
    char buf[128];
    int len = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"active\":%s,\"on\":%s,\"brightness\":%u}",
        config_mgr_is_neopixel_enabled() ? "true" : "false",
        neopixel_mgr_is_active() ? "true" : "false",
        config_mgr_get_neopixel_on() ? "true" : "false",
        (unsigned)config_mgr_get_neopixel_brightness());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// Handler for "/api/neopixel" (POST) — form body on=0|1&brightness=0-255 (either may be
// omitted to leave that field unchanged). Applies live and white-only (see neopixel_mgr.h);
// deliberately does NOT persist to NVS, same reasoning as /setup/image's "Apply Now" -- a flash
// write must never race the camera's DMA pipeline. Persisting goes through the normal
// Save & Restart path on /setup, which already defers to after esp_camera_deinit().
static esp_err_t neopixel_post_handler(httpd_req_t *req)
{
#define NEOPIXEL_BODY_MAX 64
    char body[NEOPIXEL_BODY_MAX + 1];
    int total = req->content_len;
    if (total > NEOPIXEL_BODY_MAX) total = NEOPIXEL_BODY_MAX;

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        received += r;
    }
    body[received > 0 ? received : 0] = '\0';

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (!neopixel_mgr_is_active()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "NeoPixel ring not enabled/active");
        return ESP_FAIL;
    }

    bool on = config_mgr_get_neopixel_on();
    int brightness = config_mgr_get_neopixel_brightness();

    char on_s[8] = {0}, br_s[8] = {0};
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair;
            const char *val = eq + 1;
            if (strcmp(key, "on") == 0) strlcpy(on_s, val, sizeof(on_s));
            else if (strcmp(key, "brightness") == 0) strlcpy(br_s, val, sizeof(br_s));
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }
    if (on_s[0]) on = (atoi(on_s) != 0);
    if (br_s[0]) {
        int v = atoi(br_s);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        brightness = v;
    }

    esp_err_t err = neopixel_mgr_set_state(on, (uint8_t)brightness);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply NeoPixel state");
        return ESP_FAIL;
    }

    char resp[96];
    int len = snprintf(resp, sizeof(resp), "{\"on\":%s,\"brightness\":%u}", on ? "true" : "false", (unsigned)brightness);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len);
#undef NEOPIXEL_BODY_MAX
}

// Handler for "/api/led" (GET) — the onboard GPIO14 LED. Always available (built into the
// board, no wiring/enable gate like the NeoPixel ring), so this is just its current state.
static esp_err_t led_get_handler(httpd_req_t *req)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "{\"on\":%s}", led_mgr_get() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// Handler for "/api/led" (POST) — form body on=0|1. Applies immediately and updates config_mgr's
// in-memory state (see led_mgr.h) so it's included in the next Save & Restart, but does not
// itself write NVS -- safe to call while the camera's DMA pipeline is running.
static esp_err_t led_post_handler(httpd_req_t *req)
{
#define LED_BODY_MAX 32
    char body[LED_BODY_MAX + 1];
    int total = req->content_len;
    if (total > LED_BODY_MAX) total = LED_BODY_MAX;

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        received += r;
    }
    body[received > 0 ? received : 0] = '\0';

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    bool on = led_mgr_get();
    char on_s[8] = {0};
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(pair, "on") == 0) strlcpy(on_s, eq + 1, sizeof(on_s));
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }
    if (on_s[0]) on = (atoi(on_s) != 0);

    led_mgr_set(on);

    char resp[32];
    int len = snprintf(resp, sizeof(resp), "{\"on\":%s}", on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len);
#undef LED_BODY_MAX
}

// Handler for "/api/coredump" — stream raw coredump partition
static esp_err_t coredump_handler(httpd_req_t *req)
{
    // Bearer token auth — if a token is configured, enforce it.
    const char *expected = config_mgr_get_coredump_token();
    if (expected && expected[0] != '\0') {
        char auth_hdr[96] = {0};
        esp_err_t hdr_err = httpd_req_get_hdr_value_str(req, "Authorization",
                                                          auth_hdr, sizeof(auth_hdr));
        bool authorized = false;
        if (hdr_err == ESP_OK) {
            // auth_hdr should be "Bearer <token>"
            const char *prefix = "Bearer ";
            if (strncmp(auth_hdr, prefix, strlen(prefix)) == 0) {
                authorized = (strcmp(auth_hdr + strlen(prefix), expected) == 0);
            }
        }
        if (!authorized) {
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"coredump\"");
            httpd_resp_sendstr(req, "Unauthorized");
            return ESP_FAIL;
        }
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No coredump partition");
        return ESP_FAIL;
    }

    // Check if there's a valid coredump (first 4 bytes are non-0xFF)
    uint32_t magic = 0;
    esp_partition_read(part, 0, &magic, sizeof(magic));
    if (magic == 0xFFFFFFFF || magic == 0x00000000) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No coredump saved");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Streaming coredump (%lu bytes)", (unsigned long)part->size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=coredump.bin");

    // Stream in 4KB chunks
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t offset = 0;
    while (offset < part->size) {
        size_t chunk = (part->size - offset > 4096) ? 4096 : (part->size - offset);
        esp_partition_read(part, offset, buf, chunk);
        esp_err_t err = httpd_resp_send_chunk(req, buf, chunk);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        offset += chunk;
    }

    free(buf);
    httpd_resp_send_chunk(req, NULL, 0); // End chunked response
    return ESP_OK;
}

// ========================================
// /setup — browser-accessible config page
// ========================================

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Escape HTML-special characters so user-controlled config values (MQTT URL,
 * username, password, device ID) cannot break out of an attribute or inject
 * markup into the /setup admin page. Truncates safely on the entity boundary
 * (never splits an entity) if dst is too small, and always null-terminates. */
static void html_escape(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0) return;
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        const char *ent; size_t elen;
        switch (*p) {
            case '&':  ent = "&amp;";  elen = 5; break;
            case '<':  ent = "&lt;";   elen = 4; break;
            case '>':  ent = "&gt;";   elen = 4; break;
            case '"':  ent = "&quot;"; elen = 6; break;
            case '\'': ent = "&#39;";  elen = 5; break;
            default:   ent = NULL;     elen = 1; break;
        }
        if (o + elen >= dst_size) break;   // leave room for the null terminator
        if (ent) { memcpy(dst + o, ent, elen); o += elen; }
        else     { dst[o++] = *p; }
    }
    dst[o] = '\0';
}

/* GET /setup — HTML config form pre-filled with current values */
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    static const char *res_names[] = {"QVGA (320x240)", "VGA (640x480)", "HD (1280x720)", "UXGA (1600x1200)"};
    static const uint8_t res_vals[] = {6, 10, 13, 15}; // FRAMESIZE_QVGA/VGA/HD/UXGA — PY260-supported only

    /* Get device IP for display */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip_info);
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    const esp_app_desc_t *app = esp_app_get_description();

    // static, not stack: this handler's task stack is only 8192 bytes (see start_webserver()),
    // and buf here plus the url_esc/user_esc/pass_esc/dev_esc locals below plus the httpd dispatch
    // call chain overflowed it once buf grew past 4096 for the Firmware Update section -- crashing
    // this task (and, downstream, the device) on every /setup request. Not reentrant-unsafe: this
    // handler runs synchronously on a single httpd worker, never concurrently with itself.
    static char buf[6144];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Camera Setup</title>"
        "<style>body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 16px}"
        "label{display:block;margin-top:12px;font-weight:bold}"
        "input[type=text],select{width:100%%;box-sizing:border-box;padding:6px;margin-top:4px}"
        "input[type=submit]{margin-top:20px;padding:10px 24px;background:#2563eb;color:#fff;"
        "border:none;border-radius:4px;cursor:pointer;font-size:1em}"
        ".info{background:#f0f9ff;border:1px solid #bae6fd;border-radius:4px;"
        "padding:10px 12px;margin-bottom:16px;font-size:0.9em}"
        ".info a{color:#2563eb}"
        "</style></head><body>"
        "<h2>Camera Configuration</h2>"
        "<div class='info'>"
        "Device IP: <strong>%s</strong><br>"
        "Stream: <a href='http://%s:81/stream'>http://%s:81/stream</a><br>"
        "Version: <strong>%s</strong>"
        "</div>"
        "<form method='POST' action='/setup'>",
        ip_str, ip_str, ip_str, app->version);

    /* HTML-escape user-controlled values before embedding them in attributes */
    char url_esc[384], user_esc[256], pass_esc[256], dev_esc[192];
    html_escape(url_esc,  config_mgr_get_mqtt_url(),  sizeof(url_esc));
    html_escape(user_esc, config_mgr_get_mqtt_user(), sizeof(user_esc));
    html_escape(pass_esc, config_mgr_get_mqtt_pass(), sizeof(pass_esc));
    html_escape(dev_esc,  config_mgr_get_device_id(), sizeof(dev_esc));

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<label><input type='checkbox' name='mqtt_en' value='1' id='mqtt_en'%s> Enable MQTT</label>"
        "<div id='mqtt_fields'>"
        "<label>MQTT URL</label>"
        "<input type='text' name='mqtt_url' value='%s'>"
        "<label>MQTT Username</label>"
        "<input type='text' name='mqtt_user' value='%s'>"
        "<label>MQTT Password</label>"
        "<input type='password' name='mqtt_pass' value='%s'>"
        "</div>"
        "<label>Device ID "
        "<span title='Used as: mDNS hostname (&lt;id&gt;.local), MQTT topic prefix, "
        "Home Assistant entity prefix, and BLE provisioning name (PROV_&lt;id&gt;). "
        "Use lowercase letters, numbers and underscores only.' "
        "style='font-weight:normal;cursor:help;color:#6b7280'>&#9432;</span>"
        "</label>"
        "<input type='text' name='device_id' value='%s' pattern='[a-z0-9_]+' "
        "title='Lowercase letters, numbers and underscores only'>"
        
        "<label>OTA Token (empty to disable auth)</label>"
        "<input type='password' name='ota_token' placeholder='%s'>"
        "<label>Coredump Token (empty to disable auth)</label>"
        "<input type='password' name='cd_token' placeholder='%s'>"

        "<label>Camera Resolution</label>"
        "<select name='cam_res'>",
        config_mgr_is_mqtt_enabled() ? " checked" : "",
        url_esc,
        user_esc,
        pass_esc,
        dev_esc,
        config_mgr_get_ota_token()[0] ? "(saved — leave blank to keep)" : "(no token set)",
        config_mgr_get_coredump_token()[0] ? "(saved — leave blank to keep)" : "(no token set)");

    uint8_t cur_res = config_mgr_get_cam_resolution();
    for (int i = 0; i < 4; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<option value='%u'%s>%s</option>",
            res_vals[i],
            (cur_res == res_vals[i]) ? " selected" : "",
            res_names[i]);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "</select>"
        "<label>JPEG Quality (1=best, 63=lowest)</label>"
        "<input type='text' name='jpeg_qual' value='%u'>"

#if CONFIG_UNITCAMS3_BOARD_OV3660
        "<label>Brightness (-3 to 3, 0=default)</label>"
        "<input type='text' name='brightness' value='%d'>"
        "<label>Contrast (-3 to 3, 0=default)</label>"
        "<input type='text' name='contrast' value='%d'>"
        "<label>Saturation (-4 to 4, 0=default)</label>"
        "<input type='text' name='saturation' value='%d'>"
#else
        "<label>Brightness (0-8, 4=default)</label>"
        "<input type='text' name='brightness' value='%d'>"
        "<label>Contrast (0-6, 3=default)</label>"
        "<input type='text' name='contrast' value='%d'>"
        "<label>Saturation (0-6, 3=default)</label>"
        "<input type='text' name='saturation' value='%d'>"
#endif
        "<label>White Balance</label>"
        "<select name='wb_mode'>",
        config_mgr_get_jpeg_quality(),
        config_mgr_get_brightness(),
        config_mgr_get_contrast(),
        config_mgr_get_saturation());

#if CONFIG_UNITCAMS3_BOARD_OV3660
    /* ov3660.c's set_wb_mode: 1=Sunny 2=Cloudy 3=Office 4=Home -- index 2/3
     * swapped vs mega_ccm.c below. */
    static const char *wb_names[] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
#else
    static const char *wb_names[] = {"Auto", "Sunny", "Office", "Cloudy", "Home"};
#endif
    uint8_t cur_wb = config_mgr_get_wb_mode();
    for (int i = 0; i < 5; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<option value='%d'%s>%s</option>",
            i, (cur_wb == i) ? " selected" : "", wb_names[i]);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "</select>");

#if CONFIG_UNITCAMS3_BOARD_OV3660
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<label>Exposure comp. (-5 to 5, 0=default)</label>"
        "<input type='text' name='exposure' value='%d'>"
        "<p style='font-size:0.8em;color:#6b7280;margin:-4px 0 8px'>"
        "EV bias on top of auto-exposure -- lower it if a bright glare/hotspot in frame is "
        "fooling auto-exposure into underexposing everything else. Auto-exposure stays on; "
        "this just shifts its target.</p>",
        config_mgr_get_exposure());
#endif

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<h3>NeoPixel Ring</h3>"
        "<label><input type='checkbox' name='neopixel_en' value='1' id='neopixel_en'%s> "
        "Enable NeoPixel ring (WS2812, wired to GPIO%d)</label>"
        "<p style='font-size:0.8em;color:#6b7280;margin:-4px 0 8px'>"
        "Only tick this if a WS2812/NeoPixel ring is physically connected to the pin above -- "
        "see the README for wiring. Takes effect after Save &amp; Restart.</p>"
        "<label>NeoPixel LED count</label>"
        "<input type='text' name='neopixel_cnt' value='%u'>",
        config_mgr_is_neopixel_enabled() ? " checked" : "",
        CONFIG_UNITCAMS3_NEOPIXEL_PIN,
        config_mgr_get_neopixel_count());

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<p style='font-size:0.85em;color:#6b7280;margin-bottom:4px'>"
        "\"Apply Now\" takes effect immediately with no reboot, but is lost on the next restart "
        "unless you also use \"Save &amp; Restart\" at some point (which saves everything on this "
        "page, image settings included).</p>"
        "<input type='submit' formaction='/setup/image' value='Apply Now (Brightness/Contrast/Saturation/WB/Exposure only)'>"
        " <input type='submit' value='Save &amp; Restart'>"
        "<script>function tog(){var e=document.getElementById('mqtt_en').checked;"
        "var d=document.getElementById('mqtt_fields');"
        "d.style.opacity=e?'1':'0.4';"
        "d.querySelectorAll('input').forEach(function(i){i.disabled=!e;});}"
        "document.getElementById('mqtt_en').addEventListener('change',tog);tog();"
        "</script>"
        "</form>");

    if (neopixel_mgr_is_active()) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<hr>"
            "<h3>NeoPixel Live Control</h3>"
            "<label><input type='checkbox' id='np_on'%s> On (white)</label>"
            "<label style='display:block;margin-top:8px'>Brightness"
            "<input type='range' id='np_br' min='0' max='255' value='%u' style='width:100%%'></label>"
            "<button type='button' id='np_btn' onclick='npApply()' "
            "style='margin-top:10px;padding:8px 16px;background:#2563eb;color:#fff;border:none;"
            "border-radius:4px;cursor:pointer;font-size:1em'>Apply</button>"
            "<p id='np_status' style='font-size:0.9em;color:#6b7280'></p>"
            "<script>function npApply(){"
            "var on=document.getElementById('np_on').checked?1:0;"
            "var br=document.getElementById('np_br').value;"
            "var st=document.getElementById('np_status');"
            "st.textContent='Applying...';"
            "fetch('/api/neopixel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'on='+on+'&brightness='+br}).then(function(r){"
            "if(!r.ok){return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});}"
            "return r.json();"
            "}).then(function(j){st.textContent='Applied: '+(j.on?'on':'off')+', brightness '+j.brightness+'.';"
            "}).catch(function(e){st.textContent='Failed: '+e.message;});}"
            "</script>",
            config_mgr_get_neopixel_on() ? " checked" : "",
            config_mgr_get_neopixel_brightness());
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<hr>"
        "<h3>Onboard LED</h3>"
        "<label><input type='checkbox' id='led_on'%s onchange='ledApply()'> On</label>"
        "<p id='led_status' style='font-size:0.9em;color:#6b7280'></p>"
        "<script>function ledApply(){"
        "var on=document.getElementById('led_on').checked?1:0;"
        "var st=document.getElementById('led_status');"
        "st.textContent='Applying...';"
        "fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'on='+on}).then(function(r){"
        "if(!r.ok){return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});}"
        "return r.json();"
        "}).then(function(j){st.textContent='Applied: '+(j.on?'on':'off')+'.';"
        "}).catch(function(e){st.textContent='Failed: '+e.message;});}"
        "</script>",
        led_mgr_get() ? " checked" : "");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<hr>"
        "<h3>Firmware Update</h3>"
        "<p style='font-size:0.85em;color:#6b7280;margin-top:-8px'>"
        "Upload a unitcams3_firmware.bin built for this board. The device writes it to the "
        "inactive OTA partition and reboots into it; if the new firmware fails to come up "
        "cleanly, the bootloader automatically rolls back to this version.</p>"
        "<input type='file' id='fw_file' accept='.bin'>"
        "<button type='button' id='fw_btn' onclick='fwUpload()' "
        "style='margin-top:10px;padding:8px 16px;background:#2563eb;color:#fff;border:none;"
        "border-radius:4px;cursor:pointer;font-size:1em'>Upload &amp; Flash</button>"
        "<p id='fw_status' style='font-size:0.9em;color:#6b7280'></p>"
        "<script>function fwUpload(){"
        "var f=document.getElementById('fw_file').files[0];"
        "var st=document.getElementById('fw_status');"
        "if(!f){st.textContent='Choose a .bin file first.';return;}"
        "if(!confirm('Flash '+f.name+' ('+f.size+' bytes) and reboot the camera?'))return;"
        "document.getElementById('fw_btn').disabled=true;"
        "st.textContent='Uploading '+f.size+' bytes...';"
        "fetch('/setup/ota',{method:'POST',body:f}).then(function(r){"
        "if(!r.ok){return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});}"
        "st.textContent='Uploaded. Flashing and rebooting -- reload /setup in about 20s.';"
        "}).catch(function(e){"
        "st.textContent='Upload failed: '+e.message;"
        "document.getElementById('fw_btn').disabled=false;"
        "});}</script>"
        "<hr>"
        "<details><summary style='cursor:pointer;color:#dc2626;font-weight:bold'>Danger Zone</summary>"
        "<form method='POST' action='/setup/factory-reset' style='margin-top:12px'>"
        "<p style='color:#dc2626;font-size:0.9em'>Erases all settings and Wi-Fi credentials. "
        "Device will reboot into BLE provisioning mode.</p>"
        "<input type='submit' value='Factory Reset' "
        "onclick=\"return confirm('Erase all NVS data and reboot into BLE provisioning mode?')\" "
        "style='background:#dc2626;color:#fff;border:none;padding:8px 16px;"
        "border-radius:4px;cursor:pointer;font-size:1em'>"
        "</form></details>"
        "</body></html>");

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, pos);
}

/* One-shot task: deinit camera, save config, restart */
static void setup_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_camera_deinit();
    config_mgr_save();
    recovery_mgr_signal_planned_reboot();
    esp_restart();
}

/* One-shot task: deinit camera, erase all NVS, restart into BLE provisioning */
static void factory_reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_camera_deinit();
    nvs_flash_erase();
    recovery_mgr_signal_planned_reboot();
    esp_restart();
}

/* POST /setup/factory-reset — erase NVS and reboot into BLE provisioning mode */
static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested — erasing NVS and rebooting");
    char dev_esc[192];
    html_escape(dev_esc, config_mgr_get_device_id(), sizeof(dev_esc));
    char resp_html[512];
    snprintf(resp_html, sizeof(resp_html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h2>Factory Reset</h2>"
        "<p>NVS erased. Device is rebooting into BLE provisioning mode.</p>"
        "<p>Use the <strong>Espressif BLE Provisioning</strong> app to reconnect "
        "(<strong>PROV_%s</strong>), then visit /setup to reconfigure.</p>"
        "</body></html>",
        dev_esc);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, strlen(resp_html));
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 5, NULL);
    return ESP_OK;
}

// ========================================
// Firmware update — direct upload from /setup
// ========================================

// Current image is ~1.3MB (see build output); this leaves headroom for growth while still
// bounding the PSRAM allocation to something that always fits alongside the frame pool.
#define OTA_UPLOAD_MAX_SIZE (2 * 1024 * 1024)

typedef struct {
    uint8_t *fw_buf;
    int fw_len;
} ota_upload_ctx_t;

// One-shot task: stop streams/camera (flash writes must never race camera DMA -- see
// ota_mgr_flash_from_buffer()'s doc comment), flash, then reboot. Runs after the HTTP response
// for the upload has already been sent, mirroring setup_restart_task()'s "respond, then act" shape.
static void ota_upload_finish_task(void *arg)
{
    ota_upload_ctx_t *ctx = (ota_upload_ctx_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(500)); // let the HTTP response flush first

    ESP_LOGW(TAG, "OTA upload: stopping camera/streams before flashing...");
    http_server_prepare_ota(); // stops broadcaster/clients, drains and deinits the camera

    esp_task_wdt_add(NULL);
    esp_err_t err = ota_mgr_flash_from_buffer(ctx->fw_buf, ctx->fw_len, NULL);
    esp_task_wdt_delete(NULL);

    heap_caps_free(ctx->fw_buf);
    free(ctx);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA upload: flash OK, rebooting into new firmware");
        recovery_mgr_signal_planned_reboot();
        esp_restart();
        /* NOT REACHED */
    }

    // Flash failed before Wi-Fi was stopped (ota_mgr_flash_from_buffer() reboots on its own for a
    // failure AFTER that point -- see its doc comment). Camera/streams are already torn down
    // above, though, so this device instance won't resume streaming until the next reboot; that's
    // an acceptable, rare failure mode (bad upload) rather than one worth building a live
    // camera-restart path for.
    ESP_LOGE(TAG, "OTA upload: flash failed (%s) — rebooting to restore normal operation", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* POST /setup/ota — raw firmware binary body (the page's JS uses fetch() with a File as the
 * body, not a <form>, since multipart file-upload parsing has no simple ESP-IDF httpd helper). */
static esp_err_t setup_ota_upload_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > OTA_UPLOAD_MAX_SIZE) {
        ESP_LOGE(TAG, "OTA upload: missing or implausible size %d", total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or oversized upload");
        return ESP_FAIL;
    }

    uint8_t *fw_buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fw_buf) {
        ESP_LOGE(TAG, "OTA upload: failed to alloc %d bytes PSRAM", total);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, (char *)(fw_buf + received), total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "OTA upload: recv failed (%d) at %d/%d bytes", r, received, total);
            heap_caps_free(fw_buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload interrupted");
            return ESP_FAIL;
        }
        received += r;
    }
    ESP_LOGI(TAG, "OTA upload: received %d bytes", received);

    // Fail fast on an obviously-wrong file (wrong board's image, some other .bin, HTML from a
    // redirected download, etc.) before ever promising the browser a reboot is happening.
    if (fw_buf[0] != 0xE9) {
        ESP_LOGE(TAG, "OTA upload: bad magic byte 0x%02x — not a valid firmware .bin", fw_buf[0]);
        heap_caps_free(fw_buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a valid firmware .bin (bad image header)");
        return ESP_FAIL;
    }

    ota_upload_ctx_t *ctx = malloc(sizeof(ota_upload_ctx_t));
    if (!ctx) {
        heap_caps_free(fw_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    ctx->fw_buf = fw_buf;
    ctx->fw_len = received;

    static const char *resp_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h2>Upload received. Flashing and rebooting...</h2>"
        "<p>Do not power off the camera.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, strlen(resp_html));

    /* Deferred: stop streams/camera, flash, reboot -- after the HTTP response is fully sent */
    xTaskCreate(ota_upload_finish_task, "ota_upload", 4096, ctx, 5, NULL);

    return ESP_OK;
}

/* POST /setup/image — brightness/contrast/saturation/wb_mode/exposure only. Applies live via the
 * same sensor register writes MQTT uses (no camera reinit needed) and updates config_mgr's
 * in-memory state, but deliberately does NOT call config_mgr_save() or reboot: a flash write must
 * never happen while the camera's DMA/capture pipeline is running (see config_mgr.h), and unlike
 * cam_res/jpeg_qual these settings don't require esp_camera_init() to run again. The value is
 * still included in the next full "Save & Restart" (from here or a resolution/quality change). */
static esp_err_t setup_image_post_handler(httpd_req_t *req)
{
#define IMG_BODY_MAX 128
    char body[IMG_BODY_MAX + 1];
    int total = req->content_len;
    if (total > IMG_BODY_MAX) total = IMG_BODY_MAX;

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        received += r;
    }
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char brightness_s[8] = {0}, contrast_s[8] = {0}, saturation_s[8] = {0}, wb_mode_s[8] = {0}, exposure_s[8] = {0};
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair;
            const char *val = eq + 1;
            if (strcmp(key, "brightness") == 0) strlcpy(brightness_s, val, sizeof(brightness_s));
            else if (strcmp(key, "contrast")   == 0) strlcpy(contrast_s,   val, sizeof(contrast_s));
            else if (strcmp(key, "saturation") == 0) strlcpy(saturation_s, val, sizeof(saturation_s));
            else if (strcmp(key, "wb_mode")    == 0) strlcpy(wb_mode_s,    val, sizeof(wb_mode_s));
            else if (strcmp(key, "exposure")   == 0) strlcpy(exposure_s,   val, sizeof(exposure_s));
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    int brightness = atoi(brightness_s);
    int contrast   = atoi(contrast_s);
    int saturation = atoi(saturation_s);
    int wb_mode    = atoi(wb_mode_s);
    // Absent from the form on the non-OV3660 board (and defaults to "0" = neutral if the OV3660
    // form field was simply left untouched), so atoi("") = 0 is always in-range here regardless.
    int exposure   = atoi(exposure_s);
#if CONFIG_UNITCAMS3_BOARD_OV3660
    if (brightness < -3 || brightness > 3 || contrast < -3 || contrast > 3 ||
        saturation < -4 || saturation > 4 || wb_mode < 0 || wb_mode > 4 ||
        exposure < -5 || exposure > 5) {
#else
    if (brightness < 0 || brightness > 8 || contrast < 0 || contrast > 6 ||
        saturation < 0 || saturation > 6 || wb_mode < 0 || wb_mode > 4) {
#endif
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value out of range");
        return ESP_FAIL;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
        return ESP_FAIL;
    }
    if (s->set_brightness) s->set_brightness(s, brightness);
    if (s->set_contrast) s->set_contrast(s, contrast);
    if (s->set_saturation) s->set_saturation(s, saturation);
    if (s->set_wb_mode) s->set_wb_mode(s, wb_mode);
    if (s->set_ae_level) s->set_ae_level(s, exposure);
    config_mgr_set_brightness((int8_t)brightness);
    config_mgr_set_contrast((int8_t)contrast);
    config_mgr_set_saturation((int8_t)saturation);
    config_mgr_set_wb_mode((uint8_t)wb_mode);
    config_mgr_set_exposure((int8_t)exposure);

    ESP_LOGI(TAG, "Applied image settings live (not saved): brightness=%d contrast=%d saturation=%d wb_mode=%d exposure=%d",
             brightness, contrast, saturation, wb_mode, exposure);

    static const char *resp_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2;url=/setup'></head><body>"
        "<h2>Applied</h2><p>Not saved -- reverts on next restart unless you also "
        "use Save &amp; Restart. Returning to setup...</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, resp_html, strlen(resp_html));
#undef IMG_BODY_MAX
}

/* POST /setup — parse form body, update config, schedule restart */
static esp_err_t setup_post_handler(httpd_req_t *req)
{
#define SETUP_BODY_MAX 1024
    char body[SETUP_BODY_MAX + 1];
    int total = req->content_len;
    if (total > SETUP_BODY_MAX) total = SETUP_BODY_MAX;

    /* httpd_req_recv() returns whatever a single socket read yields, which can
     * be less than content_len when the body spans multiple TCP segments. Loop
     * until we have the whole (capped) body so form fields are never truncated. */
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   // transient — retry
        if (r <= 0) break;                            // connection closed/error
        received += r;
    }
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Parse key=value&... pairs */
    char mqtt_url[128]  = {0};
    char mqtt_user[64]  = {0};
    char mqtt_pass[64]  = {0};
    char device_id[32]  = {0};
    char ota_token[64]  = {0};
    char cd_token[64]   = {0};
    bool mqtt_en        = false;
    bool neopixel_en    = false;
    char neopixel_cnt_s[8] = {0};
    char cam_res_s[8]   = {0};
    char jpeg_qual_s[8] = {0};
    char brightness_s[8] = {0};
    char contrast_s[8]   = {0};
    char saturation_s[8] = {0};
    char wb_mode_s[8]    = {0};
    char exposure_s[8]   = {0};

    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair;
            const char *val = eq + 1;
            char decoded[128];
            url_decode(decoded, val, sizeof(decoded));

            if (strcmp(key, "mqtt_url")  == 0) strlcpy(mqtt_url,    decoded, sizeof(mqtt_url));
            else if (strcmp(key, "mqtt_user") == 0) strlcpy(mqtt_user,   decoded, sizeof(mqtt_user));
            else if (strcmp(key, "mqtt_pass") == 0) strlcpy(mqtt_pass,   decoded, sizeof(mqtt_pass));
            else if (strcmp(key, "device_id") == 0) strlcpy(device_id,   decoded, sizeof(device_id));
            else if (strcmp(key, "ota_token") == 0) strlcpy(ota_token,   decoded, sizeof(ota_token));
            else if (strcmp(key, "cd_token")  == 0) strlcpy(cd_token,    decoded, sizeof(cd_token));
            else if (strcmp(key, "mqtt_en")   == 0) mqtt_en = (decoded[0] == '1');
            else if (strcmp(key, "neopixel_en") == 0) neopixel_en = (decoded[0] == '1');
            else if (strcmp(key, "neopixel_cnt") == 0) strlcpy(neopixel_cnt_s, decoded, sizeof(neopixel_cnt_s));
            else if (strcmp(key, "cam_res")   == 0) strlcpy(cam_res_s,   decoded, sizeof(cam_res_s));
            else if (strcmp(key, "jpeg_qual") == 0) strlcpy(jpeg_qual_s, decoded, sizeof(jpeg_qual_s));
            else if (strcmp(key, "brightness") == 0) strlcpy(brightness_s, decoded, sizeof(brightness_s));
            else if (strcmp(key, "contrast")   == 0) strlcpy(contrast_s,   decoded, sizeof(contrast_s));
            else if (strcmp(key, "saturation") == 0) strlcpy(saturation_s, decoded, sizeof(saturation_s));
            else if (strcmp(key, "wb_mode")    == 0) strlcpy(wb_mode_s,    decoded, sizeof(wb_mode_s));
            else if (strcmp(key, "exposure")   == 0) strlcpy(exposure_s,   decoded, sizeof(exposure_s));
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    /* Validate */
    if (device_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "device_id must not be empty");
        return ESP_FAIL;
    }
    int jpeg_qual = atoi(jpeg_qual_s);
    if (jpeg_qual < 1 || jpeg_qual > 63) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "jpeg_qual must be 1-63");
        return ESP_FAIL;
    }
    int cam_res = atoi(cam_res_s);
    if (cam_res != 6 && cam_res != 10 && cam_res != 13 && cam_res != 15) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cam_res");
        return ESP_FAIL;
    }
    // Range depends on the active sensor -- see config_mgr.h.
#if CONFIG_UNITCAMS3_BOARD_OV3660
    int brightness = atoi(brightness_s);
    if (brightness < -3 || brightness > 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "brightness must be -3 to 3");
        return ESP_FAIL;
    }
    int contrast = atoi(contrast_s);
    if (contrast < -3 || contrast > 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "contrast must be -3 to 3");
        return ESP_FAIL;
    }
    int saturation = atoi(saturation_s);
    if (saturation < -4 || saturation > 4) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "saturation must be -4 to 4");
        return ESP_FAIL;
    }
#else
    int brightness = atoi(brightness_s);
    if (brightness < 0 || brightness > 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "brightness must be 0-8");
        return ESP_FAIL;
    }
    int contrast = atoi(contrast_s);
    if (contrast < 0 || contrast > 6) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "contrast must be 0-6");
        return ESP_FAIL;
    }
    int saturation = atoi(saturation_s);
    if (saturation < 0 || saturation > 6) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "saturation must be 0-6");
        return ESP_FAIL;
    }
#endif
    int wb_mode = atoi(wb_mode_s);
    if (wb_mode < 0 || wb_mode > 4) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wb_mode must be 0-4");
        return ESP_FAIL;
    }
    // Absent from the form on the non-OV3660 board, so atoi("") = 0 (neutral) is always in-range.
    int exposure = atoi(exposure_s);
    if (exposure < -5 || exposure > 5) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "exposure must be -5 to 5");
        return ESP_FAIL;
    }
    int neopixel_cnt = atoi(neopixel_cnt_s);
    if (neopixel_cnt < 1 || neopixel_cnt > 300) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "neopixel_cnt must be 1-300");
        return ESP_FAIL;
    }

    /* Apply to in-memory config */
    config_mgr_set_mqtt_url(mqtt_url);
    config_mgr_set_mqtt_user(mqtt_user);
    config_mgr_set_mqtt_pass(mqtt_pass);
    config_mgr_set_device_id(device_id);
    if (ota_token[0]) config_mgr_set_ota_token(ota_token);
    if (cd_token[0])  config_mgr_set_coredump_token(cd_token);
    config_mgr_set_mqtt_enabled(mqtt_en);
    config_mgr_set_neopixel_enabled(neopixel_en);
    config_mgr_set_neopixel_count((uint16_t)neopixel_cnt);
    config_mgr_set_cam_resolution((uint8_t)cam_res);
    config_mgr_set_jpeg_quality((uint8_t)jpeg_qual);
    config_mgr_set_brightness((int8_t)brightness);
    config_mgr_set_contrast((int8_t)contrast);
    config_mgr_set_saturation((int8_t)saturation);
    config_mgr_set_wb_mode((uint8_t)wb_mode);
    config_mgr_set_exposure((int8_t)exposure);

    // Apply brightness/contrast/saturation/wb_mode/exposure immediately (no camera reinit needed --
    // same live sensor register writes mqtt_mgr already does for these). cam_res/jpeg_qual
    // still need the reboot below since they require re-running esp_camera_init().
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->set_brightness) s->set_brightness(s, brightness);
        if (s->set_contrast) s->set_contrast(s, contrast);
        if (s->set_saturation) s->set_saturation(s, saturation);
        if (s->set_wb_mode) s->set_wb_mode(s, wb_mode);
        if (s->set_ae_level) s->set_ae_level(s, exposure);
    }

    ESP_LOGI(TAG, "Setup POST: url=%s user=%s dev=%s mqtt_en=%d res=%d qual=%d",
             mqtt_url, mqtt_user, device_id, mqtt_en, cam_res, jpeg_qual);

    /* Send response before restarting */
    static const char *resp_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h2>Saved. Rebooting...</h2>"
        "<p>Returning to setup in <span id='c'>30</span>s...</p>"
        "<script>var t=30;function tick(){document.getElementById('c').textContent=t;"
        "if(--t<0){window.location='/setup';return;}setTimeout(tick,1000);}"
        "setTimeout(tick,1000);</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, strlen(resp_html));

    /* Deferred restart so the HTTP response is fully sent first */
    xTaskCreate(setup_restart_task, "setup_rst", 4096, NULL, 5, NULL);

    return ESP_OK;
#undef SETUP_BODY_MAX
}

uint8_t http_server_get_active_streams(void)
{
    if (s_clients_mutex == NULL) {
        return 0;
    }
    uint8_t count = 0;
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_stream_clients[i].active) {
            count++;
        }
    }
    xSemaphoreGive(s_clients_mutex);
    return count;
}

esp_err_t start_stream_server(void)
{
    // Start broadcaster task if not already running
    if (!s_broadcaster_task_handle) {
        xTaskCreatePinnedToCore(mjpeg_broadcaster_task, "mjpeg_broad", 8192, NULL, 5, &s_broadcaster_task_handle, 1);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;             // DIFFERENT PORT
    config.ctrl_port = 32767;            // DIFFERENT CONTROL PORT
    config.core_id = 1;                  // Run on Core 1
    config.stack_size = 8192;
    config.max_uri_handlers = 5;
    config.max_open_sockets = 5;         
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting Stream Server on port: '%d'", config.server_port);

    if (httpd_start(&s_stream_server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_stream_server, &stream_uri);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Error starting stream server!");
    return ESP_FAIL;
}

httpd_handle_t stream_server_get_handle(void)
{
    return s_stream_server;
}

esp_err_t start_http_server(void)
{
    s_boot_time_us = esp_timer_get_time();

    if (!s_clients_mutex) s_clients_mutex = xSemaphoreCreateMutex();
    if (!s_broadcast_mutex) s_broadcast_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.stack_size = 8192;
    config.max_uri_handlers = 18; // 8 original + /snapshot + /stream (plugin) + /setup/image + /setup/ota + /api/neopixel (GET+POST) + /api/led (GET+POST)
    config.max_open_sockets = 10;
    config.recv_wait_timeout = 5;   // 5s recv timeout to prevent WDT panics on network stall
    config.send_wait_timeout = 5;   // 5s send timeout to prevent WDT panics on network stall
    config.lru_purge_enable = true;  // Purge least-recently-used connections

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d' (Core: %d)", config.server_port, config.core_id);

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t capture_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = capture_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &capture_uri);

        // /snapshot: same handler as "/", registered under the path the Duet Tool Align
        // DWC plugin (and duet-webcam-bridge before it) expects from a camera bridge.
        httpd_uri_t snapshot_uri = {
            .uri       = "/snapshot",
            .method    = HTTP_GET,
            .handler   = capture_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &snapshot_uri);

        // /stream on port 80 too: stream_handler hands off to an async worker task
        // immediately (see mjpeg_client_worker_task) rather than blocking the httpd
        // task, so it's safe to serve alongside the routes above from one origin --
        // the plugin needs /stream and /snapshot on the same bridgeUrl/port.
        httpd_uri_t stream_uri_p80 = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &stream_uri_p80);

        httpd_uri_t health_uri = {
            .uri       = "/health",
            .method    = HTTP_GET,
            .handler   = health_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &health_uri);

        httpd_uri_t stats_uri = {
            .uri       = "/stats",
            .method    = HTTP_GET,
            .handler   = stats_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &stats_uri);

        httpd_uri_t coredump_uri = {
            .uri       = "/api/coredump",
            .method    = HTTP_GET,
            .handler   = coredump_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &coredump_uri);

        httpd_uri_t logs_uri = {
            .uri       = "/api/logs",
            .method    = HTTP_GET,
            .handler   = logs_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &logs_uri);

        httpd_uri_t neopixel_get_uri = {
            .uri       = "/api/neopixel",
            .method    = HTTP_GET,
            .handler   = neopixel_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &neopixel_get_uri);

        httpd_uri_t neopixel_post_uri = {
            .uri       = "/api/neopixel",
            .method    = HTTP_POST,
            .handler   = neopixel_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &neopixel_post_uri);

        httpd_uri_t led_get_uri = {
            .uri       = "/api/led",
            .method    = HTTP_GET,
            .handler   = led_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &led_get_uri);

        httpd_uri_t led_post_uri = {
            .uri       = "/api/led",
            .method    = HTTP_POST,
            .handler   = led_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &led_post_uri);

        httpd_uri_t setup_get_uri = {
            .uri       = "/setup",
            .method    = HTTP_GET,
            .handler   = setup_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_get_uri);

        httpd_uri_t setup_post_uri = {
            .uri       = "/setup",
            .method    = HTTP_POST,
            .handler   = setup_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_post_uri);

        httpd_uri_t setup_image_uri = {
            .uri       = "/setup/image",
            .method    = HTTP_POST,
            .handler   = setup_image_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_image_uri);

        httpd_uri_t factory_reset_uri = {
            .uri       = "/setup/factory-reset",
            .method    = HTTP_POST,
            .handler   = factory_reset_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &factory_reset_uri);

        httpd_uri_t setup_ota_uri = {
            .uri       = "/setup/ota",
            .method    = HTTP_POST,
            .handler   = setup_ota_upload_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_ota_uri);

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

httpd_handle_t http_server_get_handle(void)
{
    return s_server;
}

void http_server_stop(void)
{
    s_ota_pending = true;
    vTaskDelay(pdMS_TO_TICKS(500)); // Give tasks time to notice

    if (s_server) {
        ESP_LOGW(TAG, "Stopping main HTTP server (Port 80)...");
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_stream_server) {
        ESP_LOGW(TAG, "Stopping stream HTTP server (Port 81)...");
        httpd_stop(s_stream_server);
        s_stream_server = NULL;
    }

    // Mutexes and broadcaster handle stay for next start, or can be cleaned if permanent stop
}

void http_server_signal_stop(void)
{
    ESP_LOGW(TAG, "Signaling HTTP streams to stop...");
    s_ota_pending = true;
}

void http_server_prepare_ota(void)
{
    ESP_LOGW(TAG, "OTA requested: signaling streams to stop...");
    s_ota_pending = true;

    // Wait for broadcaster task to exit
    int timeout = 50; // 5 seconds (50 * 100ms)
    while (s_broadcaster_task_handle != NULL && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_broadcaster_task_handle != NULL) {
        ESP_LOGE(TAG, "Broadcaster task failed to exit, deleting manually");
        vTaskDelete(s_broadcaster_task_handle);
        s_broadcaster_task_handle = NULL;
    }

    // Wait for client tasks to exit
    timeout = 20; // 2 seconds
    bool all_gone;
    do {
        all_gone = true;
        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (s_stream_clients[i].active) {
                all_gone = false;
                break;
            }
        }
        xSemaphoreGive(s_clients_mutex);
        if (!all_gone) vTaskDelay(pdMS_TO_TICKS(100));
    } while (!all_gone && timeout-- > 0);

    // Drain all pending camera frames before deinit.
    ESP_LOGW(TAG, "Draining camera frame buffers...");
    for (int i = 0; i < 10; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        esp_camera_fb_return(fb);
    }
    // One more delay to let cam_task settle back to queue-wait state
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGW(TAG, "Deinitializing camera for OTA...");
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Camera stopped. Flash is safe for OTA writes.");
}
