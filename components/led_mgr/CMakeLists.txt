idf_component_register(SRCS "led_mgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES config_mgr esp_driver_gpio)
