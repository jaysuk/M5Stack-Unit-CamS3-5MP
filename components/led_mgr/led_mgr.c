#include "led_mgr.h"
#include "config_mgr.h"
#include "driver/gpio.h"

#define LED_GPIO 14

static bool s_on = false;

esp_err_t led_mgr_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
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
    gpio_set_level(LED_GPIO, on ? 1 : 0);
    config_mgr_set_led_on(on);
}

void led_mgr_set_level(int level)
{
    led_mgr_set(level != 0);
}
