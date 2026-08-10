# ESP32-S3 only, MEGA_CCM (PY260) + OV3660, JPEG only
# Forked from M5Stack UnitCamS3-UserDemo (unitcams3-5mp branch)

# Select SCCB implementation based on ESP-IDF major version:
#   IDF 5.x  → driver/sccb.c            (legacy driver/i2c.h API, deprecated in 5.x)
#   IDF 6.0+ → driver/sccb_i2c_master.c (new driver/i2c_master.h API, legacy removed in 7.0)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(SCCB_SRC "driver/sccb_i2c_master.c")
    message(STATUS "esp32-camera: SCCB using new I2C master API (IDF ${IDF_VERSION_MAJOR}.x)")
else()
    set(SCCB_SRC "driver/sccb.c")
    message(STATUS "esp32-camera: SCCB using legacy I2C API (IDF ${IDF_VERSION_MAJOR}.x)")
endif()

set(COMPONENT_SRCS
    conversions/esp_jpg_decode.c
    driver/esp_camera.c
    driver/cam_hal.c
    ${SCCB_SRC}
    driver/sensor.c
    sensors/mega_ccm.c
    sensors/ov3660.c
    target/esp32s3/ll_cam.c
)

set(COMPONENT_ADD_INCLUDEDIRS
    driver/include
    conversions/include
)

set(COMPONENT_PRIV_INCLUDEDIRS
    driver/private_include
    sensors/private_include
    target/private_include
)

set(COMPONENT_REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_ledc)

# IDF 6+: gdma_periph.h moved from soc/ to hal/ in esp_hal_dma component
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(GDMA_PERIPH_COMPONENT esp_hal_dma)
else()
    set(GDMA_PERIPH_COMPONENT "")
endif()

set(COMPONENT_PRIV_REQUIRES freertos nvs_flash esp_timer esp_mm recovery_mgr esp_system ${GDMA_PERIPH_COMPONENT})

register_component()
