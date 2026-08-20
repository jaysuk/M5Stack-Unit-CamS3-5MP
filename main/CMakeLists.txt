idf_component_register(
    SRCS "main.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES hw_validate esp32-camera jpeg_validate esp_wifi nvs_flash esp_http_server esp_netif esp_event esp_timer frame_pool mqtt_mgr recovery_mgr ota_mgr app_update http_server config_mgr esp_driver_gpio mdns log_buf neopixel_mgr led_mgr wifi_provision
)
