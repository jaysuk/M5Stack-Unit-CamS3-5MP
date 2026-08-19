#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the NeoPixel (WS2812) ring driver, if enabled in config.
 *
 * No-op (returns ESP_OK without touching the RMT peripheral or GPIO) when
 * config_mgr_is_neopixel_enabled() is false -- boards without the ring wired up are
 * completely unaffected. When enabled, claims CONFIG_UNITCAMS3_NEOPIXEL_PIN via the RMT
 * driver and immediately applies the persisted on/off + brightness state, so the ring comes
 * back the way it was left across a reboot.
 *
 * Call once at startup, after config_mgr_init().
 */
esp_err_t neopixel_mgr_init(void);

/** True once neopixel_mgr_init() has successfully brought the ring up (enabled in config AND
 *  the driver initialized OK). Callers (the HTTP API) use this to report capability accurately
 *  even if the config flag is set but the hardware failed to init for some reason. */
bool neopixel_mgr_is_active(void);

/**
 * @brief Set on/off and brightness (0-255), applied to the ring immediately.
 *
 * White only, by design -- every LED is set to R=G=B=brightness, since that's the whole ask
 * (a work light, not an RGB effect). No-op (ESP_ERR_INVALID_STATE) if the ring isn't active.
 *
 * Updates config_mgr's in-memory on/brightness state on success but does NOT write NVS --
 * exactly like the image settings' "Apply Now" path, so this can be called freely while the
 * camera's DMA pipeline is running. Persisting the new state to survive a reboot goes through
 * the normal config_mgr_save() path (after esp_camera_deinit()), same as everything else.
 */
esp_err_t neopixel_mgr_set_state(bool on, uint8_t brightness);

#ifdef __cplusplus
}
#endif
