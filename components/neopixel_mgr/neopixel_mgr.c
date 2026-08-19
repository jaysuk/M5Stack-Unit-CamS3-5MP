#include "neopixel_mgr.h"
#include "config_mgr.h"
#include "led_strip.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "neopixel_mgr";

static led_strip_handle_t s_strip = NULL;
static bool s_active = false;

// White only: every LED set to R=G=B=brightness. Not exposed to any caller as separate channels
// since the only ask is "on/off + how bright", not colour.
static esp_err_t apply(bool on, uint8_t brightness)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!on || brightness == 0) {
        esp_err_t err = led_strip_clear(s_strip);
        return err;
    }
    for (int i = 0; i < CONFIG_UNITCAMS3_NEOPIXEL_COUNT; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, i, brightness, brightness, brightness);
        if (err != ESP_OK) {
            return err;
        }
    }
    return led_strip_refresh(s_strip);
}

esp_err_t neopixel_mgr_init(void)
{
    if (!config_mgr_is_neopixel_enabled()) {
        ESP_LOGI(TAG, "NeoPixel ring not enabled in config -- skipping init (RMT/GPIO untouched)");
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_UNITCAMS3_NEOPIXEL_PIN,
        .max_leds = CONFIG_UNITCAMS3_NEOPIXEL_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz, the led_strip example's standard WS2812 rate
        .flags.with_dma = false,           // a 16-LED ring is nowhere near needing DMA
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device (GPIO%d, %d LEDs) failed: %s",
                 CONFIG_UNITCAMS3_NEOPIXEL_PIN, CONFIG_UNITCAMS3_NEOPIXEL_COUNT, esp_err_to_name(err));
        return err;
    }

    s_active = true;
    err = apply(config_mgr_get_neopixel_on(), config_mgr_get_neopixel_brightness());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Applying saved NeoPixel state failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "NeoPixel ring ready: GPIO%d, %d LEDs, on=%d brightness=%d",
             CONFIG_UNITCAMS3_NEOPIXEL_PIN, CONFIG_UNITCAMS3_NEOPIXEL_COUNT,
             config_mgr_get_neopixel_on(), config_mgr_get_neopixel_brightness());
    return ESP_OK;
}

bool neopixel_mgr_is_active(void)
{
    return s_active;
}

esp_err_t neopixel_mgr_set_state(bool on, uint8_t brightness)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = apply(on, brightness);
    if (err == ESP_OK) {
        config_mgr_set_neopixel_on(on);
        config_mgr_set_neopixel_brightness(brightness);
    }
    return err;
}
