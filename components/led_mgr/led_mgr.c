#include "led_mgr.h"
#include "config_mgr.h"
#include "led_strip.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "led_mgr";

// Full white -- this is a single small status LED, not a work light, so there's no brightness
// control to tune: on means clearly visible.
#define LED_BRIGHTNESS 255

static led_strip_handle_t s_strip = NULL;
static bool s_on = false;

static esp_err_t apply(bool on)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!on) {
        return led_strip_clear(s_strip);
    }
    esp_err_t err = led_strip_set_pixel(s_strip, 0, LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS);
    if (err != ESP_OK) {
        return err;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_mgr_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_UNITCAMS3_LED_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz, same as neopixel_mgr's WS2812 rate
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device (GPIO%d) failed: %s", CONFIG_UNITCAMS3_LED_PIN, esp_err_to_name(err));
        return err;
    }

    led_mgr_set(config_mgr_get_led_on());
    return ESP_OK;
}

bool led_mgr_get(void)
{
    return s_on;
}

void led_mgr_set(bool on)
{
    s_on = on;
    esp_err_t err = apply(on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply LED state: %s", esp_err_to_name(err));
    }
    config_mgr_set_led_on(on);
}

void led_mgr_set_level(int level)
{
    led_mgr_set(level != 0);
}
