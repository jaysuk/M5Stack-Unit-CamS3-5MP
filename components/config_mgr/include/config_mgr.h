#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load config from NVS, falling back to Kconfig defaults.
 *        Read-only NVS access — safe to call at any boot stage.
 */
esp_err_t config_mgr_init(void);

/* Getters */
const char *config_mgr_get_mqtt_url(void);
const char *config_mgr_get_mqtt_user(void);
const char *config_mgr_get_mqtt_pass(void);
const char *config_mgr_get_device_id(void);
bool        config_mgr_is_mqtt_enabled(void);
uint8_t     config_mgr_get_cam_resolution(void);   /* framesize_t value */
uint8_t     config_mgr_get_jpeg_quality(void);
const char *config_mgr_get_ota_token(void);
const char *config_mgr_get_coredump_token(void);

/* PY260/mega_ccm sensor register indices -- NOT the generic OV-sensor -2..2
 * convention. See components/esp32-camera/sensors/mega_ccm.c set_brightness/
 * set_contrast/set_saturation/set_wb_mode for the authoritative mapping. */
uint8_t     config_mgr_get_brightness(void);   /* 0-8, 4 = neutral */
uint8_t     config_mgr_get_contrast(void);     /* 0-6, 3 = neutral */
uint8_t     config_mgr_get_saturation(void);   /* 0-6, 3 = neutral */
uint8_t     config_mgr_get_wb_mode(void);      /* 0=Auto 1=Sunny 2=Office 3=Cloudy 4=Home */

/* Setters (update in-memory state only — call config_mgr_save() to persist) */
void config_mgr_set_mqtt_url(const char *v);
void config_mgr_set_mqtt_user(const char *v);
void config_mgr_set_mqtt_pass(const char *v);
void config_mgr_set_device_id(const char *v);
void config_mgr_set_mqtt_enabled(bool v);
void config_mgr_set_cam_resolution(uint8_t v);
void config_mgr_set_jpeg_quality(uint8_t v);
void config_mgr_set_ota_token(const char *v);
void config_mgr_set_coredump_token(const char *v);
void config_mgr_set_brightness(uint8_t v);
void config_mgr_set_contrast(uint8_t v);
void config_mgr_set_saturation(uint8_t v);
void config_mgr_set_wb_mode(uint8_t v);

/**
 * @brief Write all fields to NVS.
 *        MUST only be called after esp_camera_deinit() — any flash write
 *        disables the OPI PSRAM cache and will fault if camera DMA is running.
 */
esp_err_t config_mgr_save(void);

#ifdef __cplusplus
}
#endif
