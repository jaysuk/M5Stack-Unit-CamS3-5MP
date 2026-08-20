#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the onboard status LED (a single WS2812, see CONFIG_UNITCAMS3_LED_PIN) and
 *        apply the persisted on/off state.
 *
 * Driven via the same led_strip/RMT backend as the NeoPixel ring, just a single pixel -- this
 * board's "status LED" is itself addressable, not a plain GPIO LED. White only, fixed brightness
 * (see led_mgr.c) -- the ask here is on/off, not a dimmer or color picker.
 *
 * Call once at startup, after config_mgr_init() -- needs the persisted state to apply.
 */
esp_err_t led_mgr_init(void);

/** Current on/off state. */
bool led_mgr_get(void);

/**
 * @brief Set on/off immediately.
 *
 * Updates config_mgr's in-memory state (not NVS) so the next Save & Restart also persists it --
 * same live-apply/no-immediate-flash-write pattern as neopixel_mgr_set_state(). Safe to call
 * while the camera's DMA pipeline is running.
 */
void led_mgr_set(bool on);

/** int-level shim matching mqtt_mgr_register_led_callback's `void (*)(int level)` signature. */
void led_mgr_set_level(int level);

#ifdef __cplusplus
}
#endif
