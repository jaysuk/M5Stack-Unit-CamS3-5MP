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

/* Range/neutral point depends on the active sensor (CONFIG_UNITCAMS3_BOARD_*) --
 * see the CONFIG_UNITCAMS3_BOARD_OV3660 #if in config_mgr.c for both scales.
 * PY260/mega_ccm uses its own register-index convention (0-8/0-6/0-6, biased
 * so N/2 = neutral); OV3660 uses the signed range its driver expects natively
 * (-3..3/-3..3/-4..4, 0 = neutral). Callers must not assume a fixed range. */
int8_t      config_mgr_get_brightness(void);
int8_t      config_mgr_get_contrast(void);
int8_t      config_mgr_get_saturation(void);
uint8_t     config_mgr_get_wb_mode(void);      /* 0=Auto, 1-4 = per-sensor preset order (labels differ, see http_server.c wb_names) */

/* AE level / exposure compensation: -5..5, 0=neutral, same range on both boards. Only
 * ov3660.c implements set_ae_level (a genuine EV-style bias on top of AEC, not a manual
 * exposure override) -- mega_ccm.c (PY260) wires it to a no-op, so this has no effect there. */
int8_t      config_mgr_get_exposure(void);

/* NeoPixel ring: "enabled" is the /setup master switch (whether the hardware is even wired
 * up) -- gates neopixel_mgr_init() touching the RMT peripheral/GPIO at all, and is what the
 * duet-tool-align plugin checks before showing any light control. "on"/"brightness" are the
 * live state, white only (see neopixel_mgr.h). "count" is the number of LEDs in the ring/strip;
 * CONFIG_UNITCAMS3_NEOPIXEL_COUNT only seeds its initial value now -- like "enabled", a changed
 * count only takes effect on the next neopixel_mgr_init() (i.e. after Save & Restart), since the
 * led_strip driver's LED count is fixed for the life of the RMT device it creates. */
bool        config_mgr_is_neopixel_enabled(void);
bool        config_mgr_get_neopixel_on(void);
uint8_t     config_mgr_get_neopixel_brightness(void);
uint16_t    config_mgr_get_neopixel_count(void);

/* Onboard status LED, a single WS2812 (see led_mgr.h). Always available -- unlike the NeoPixel ring there's no
 * "enabled" gate, since this LED is built into the board rather than something the user wires
 * up. Persisted so it restores its last state across reboots, same as neopixel_on. */
bool        config_mgr_get_led_on(void);

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
void config_mgr_set_brightness(int8_t v);
void config_mgr_set_contrast(int8_t v);
void config_mgr_set_saturation(int8_t v);
void config_mgr_set_wb_mode(uint8_t v);
void config_mgr_set_exposure(int8_t v);
void config_mgr_set_neopixel_enabled(bool v);
void config_mgr_set_neopixel_on(bool v);
void config_mgr_set_neopixel_brightness(uint8_t v);
void config_mgr_set_neopixel_count(uint16_t v);
void config_mgr_set_led_on(bool v);

/**
 * @brief Write all fields to NVS.
 *        MUST only be called after esp_camera_deinit() — any flash write
 *        disables the OPI PSRAM cache and will fault if camera DMA is running.
 */
esp_err_t config_mgr_save(void);

#ifdef __cplusplus
}
#endif
