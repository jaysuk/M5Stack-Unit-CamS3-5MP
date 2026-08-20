idf_component_register(SRCS "http_server.c"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_http_server esp32-camera jpeg_validate frame_pool esp_partition esp_timer esp_system config_mgr recovery_mgr esp_netif nvs_flash log_buf ota_mgr neopixel_mgr led_mgr
                        PRIV_REQUIRES main)
