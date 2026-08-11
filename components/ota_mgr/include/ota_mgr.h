#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize OTA manager.
 *
 * Registers the URL-based OTA callback with mqtt_mgr.
 * Trigger OTA by publishing a firmware URL to unitcams3/ota/set.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ota_mgr_init(void);

/**
 * @brief Save OTA URL and optional SHA-256 to RTC RAM and reboot immediately.
 *
 * Called from MQTT on receipt of unitcams3/ota/set.
 * Saves the URL and hash to RTC RAM, then calls esp_restart().
 * On the next boot, ota_mgr_run_pending() picks up the URL and
 * runs the OTA before the camera is ever initialized.
 *
 * @param url HTTP URL pointing to the firmware binary
 * @param sha256_hex 64-char hex string of the binary's SHA-256 (optional, may be NULL)
 * @return Does not return on success (reboots). Returns error code on failure.
 */
esp_err_t ota_mgr_start_url(const char *url, const char *sha256_hex);

/**
 * @brief Check NVS for a pending OTA URL and run it if found.
 *
 * Must be called from main() AFTER Wi-Fi connects but BEFORE
 * esp_camera_init(). If a pending URL is found, downloads and
 * flashes the firmware, then reboots into the new image.
 * Does not return if OTA is pending (reboots on success or failure).
 * Returns ESP_OK immediately if no pending OTA is found.
 *
 * @return ESP_OK if no pending OTA (normal boot continues)
 */
esp_err_t ota_mgr_run_pending(void);

/**
 * @brief Flash a firmware image already sitting in a PSRAM buffer (e.g. a direct HTTP upload).
 *
 * Validates the ESP32 image magic byte and, if provided, a SHA-256 hash, then stops Wi-Fi and
 * writes the image to the inactive OTA partition, setting it as the next boot partition. Does
 * NOT reboot -- the caller decides when, so it can finish sending an HTTP response first.
 *
 * The camera must already be deinitialized (esp_camera_deinit()) and any streaming stopped
 * before calling this: esp_ota_write() disables the OPI PSRAM cache, and any concurrent PSRAM
 * access (camera DMA, or a Wi-Fi/lwIP ISR after esp_wifi_stop() is called internally here but
 * before it's fully quiesced) crashes with a double exception. See ota_mgr.c's cache-disable
 * safety notes and http_server_prepare_ota(), which performs exactly that shutdown sequence.
 *
 * On failure AFTER Wi-Fi has already been stopped, this function reboots on its own rather than
 * returning, matching ota_mgr_run_pending()'s failure handling -- there's no safe way to resume
 * normal operation with Wi-Fi torn down mid-request.
 *
 * @param fw_buf PSRAM buffer containing the complete firmware image
 * @param fw_len Length of fw_buf in bytes
 * @param sha256_hex Optional 64-char hex SHA-256 to verify against, or NULL to skip
 * @return ESP_OK on success (new boot partition set, safe to reboot); does not return on a
 *         post-Wi-Fi-stop failure (reboots instead); returns an error code for any failure
 *         before that point (current firmware keeps running)
 */
esp_err_t ota_mgr_flash_from_buffer(uint8_t *fw_buf, int fw_len, const char *sha256_hex);

#ifdef __cplusplus
}
#endif
