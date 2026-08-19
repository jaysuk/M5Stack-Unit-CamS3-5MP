if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(WIFI_SRC "wifi_v6.c")
    set(PROV_COMPONENT "network_provisioning")
    message(STATUS "main: using network_provisioning (IDF ${IDF_VERSION_MAJOR}.x)")
else()
    set(WIFI_SRC "wifi.c")
    set(PROV_COMPONENT "wifi_provisioning")
    message(STATUS "main: using wifi_provisioning (IDF ${IDF_VERSION_MAJOR}.x)")
endif()

idf_component_register(
    SRCS "main.c" ${WIFI_SRC}
    INCLUDE_DIRS "."
    REQUIRES hw_validate esp32-camera jpeg_validate esp_wifi nvs_flash esp_http_server esp_netif esp_event esp_timer frame_pool mqtt_mgr recovery_mgr ota_mgr app_update http_server ${PROV_COMPONENT} bt config_mgr esp_driver_gpio mdns log_buf neopixel_mgr
)
