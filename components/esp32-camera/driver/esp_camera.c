// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
// Licensed under the Apache License, Version 2.0
//
// Stripped to MEGA_CCM (PY260) + OV3660, JPEG-only.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "time.h"
#include "sys/time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sensor.h"
#include "sccb.h"
#include "cam_hal.h"
#include "esp_camera.h"
#include "mega_ccm.h"
#include "ov3660.h"

#include "esp_log.h"
static const char *TAG = "camera";

typedef struct {
    sensor_t sensor;
    camera_fb_t fb;
} camera_state_t;

static const char *CAMERA_SENSOR_NVS_KEY = "sensor";
static const char *CAMERA_PIXFORMAT_NVS_KEY = "pixformat";
static camera_state_t *s_state = NULL;

/* ESP32-S3 LCD_CAM generates XCLK, no LEDC needed */
#define CAMERA_ENABLE_OUT_CLOCK(v)
#define CAMERA_DISABLE_OUT_CLOCK()

typedef struct {
    int (*detect)(int slv_addr, sensor_id_t *id);
    int (*init)(sensor_t *sensor);
} sensor_func_t;

static const sensor_func_t g_sensors[] = {
    {mega_ccm_detect, mega_ccm_init},
    {ov3660_detect, ov3660_init},
};

static esp_err_t camera_probe(const camera_config_t *config, camera_model_t *out_camera_model)
{
    esp_err_t ret = ESP_OK;
    *out_camera_model = CAMERA_NONE;
    if (s_state != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state = (camera_state_t *) calloc(1, sizeof(camera_state_t));
    if (!s_state) {
        return ESP_ERR_NO_MEM;
    }

    if (config->pin_xclk >= 0) {
        ESP_LOGD(TAG, "Enabling XCLK output");
        CAMERA_ENABLE_OUT_CLOCK(config);
    }

    if (config->pin_sccb_sda != -1) {
        ESP_LOGD(TAG, "Initializing SCCB");
        ret = SCCB_Init(config->pin_sccb_sda, config->pin_sccb_scl);
    } else {
        ESP_LOGD(TAG, "Using existing I2C port");
        ret = SCCB_Use_Port(config->sccb_i2c_port);
    }

    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "sccb init err");
        goto err;
    }

    if (config->pin_pwdn >= 0) {
        ESP_LOGD(TAG, "Resetting camera by power down line");
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << config->pin_pwdn;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);

        gpio_set_level(config->pin_pwdn, 1);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        // Note: on UnitCamS3-5MP, PWDN is hardware-tied to GND (pin_pwdn = -1),
        // so this branch is never taken.
    }

    if (config->pin_reset >= 0) {
        ESP_LOGD(TAG, "Resetting camera");
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << config->pin_reset;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);

        gpio_set_level(config->pin_reset, 0);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        gpio_set_level(config->pin_reset, 1);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    ESP_LOGD(TAG, "Searching for camera address");
    vTaskDelay(10 / portTICK_PERIOD_MS);

    uint8_t slv_addr = SCCB_Probe();

    if (slv_addr == 0) {
        ret = ESP_ERR_NOT_FOUND;
        goto err;
    }

    ESP_LOGI(TAG, "Detected camera at address=0x%02x", slv_addr);
    s_state->sensor.slv_addr = slv_addr;
    s_state->sensor.xclk_freq_hz = config->xclk_freq_hz;

    sensor_id_t *id = &s_state->sensor.id;
    for (size_t i = 0; i < sizeof(g_sensors) / sizeof(sensor_func_t); i++) {
        if (g_sensors[i].detect(slv_addr, id)) {
            camera_sensor_info_t *info = esp_camera_sensor_get_info(id);
            if (NULL != info) {
                *out_camera_model = info->model;
                ESP_LOGI(TAG, "Detected %s camera", info->name);
                g_sensors[i].init(&s_state->sensor);
                break;
            }
        }
    }

    if (CAMERA_NONE == *out_camera_model) {
        ESP_LOGE(TAG, "Detected camera not supported.");
        ret = ESP_ERR_NOT_SUPPORTED;
        goto err;
    }

    ESP_LOGI(TAG, "Camera PID=0x%02x VER=0x%02x MIDL=0x%02x MIDH=0x%02x",
             id->PID, id->VER, id->MIDH, id->MIDL);

    ESP_LOGD(TAG, "Doing SW reset of sensor");
    vTaskDelay(10 / portTICK_PERIOD_MS);

    return s_state->sensor.reset(&s_state->sensor);
err :
    CAMERA_DISABLE_OUT_CLOCK();
    return ret;
}

esp_err_t esp_camera_init(const camera_config_t *config)
{
    esp_err_t err;
    err = cam_init(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }

    camera_model_t camera_model = CAMERA_NONE;
    err = camera_probe(config, &camera_model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera probe failed with error 0x%x(%s)", err, esp_err_to_name(err));
        goto fail;
    }

    framesize_t frame_size = (framesize_t) config->frame_size;
    /* Use config format */
    pixformat_t pix_format = config->pixel_format;

    if (frame_size > camera_sensor[camera_model].max_size) {
        ESP_LOGW(TAG, "Frame size exceeds max for sensor, forcing to maximum");
        frame_size = camera_sensor[camera_model].max_size;
    }

    err = cam_config(config, frame_size, s_state->sensor.id.PID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera config failed with error 0x%x", err);
        goto fail;
    }

    s_state->sensor.pixformat = pix_format;
    s_state->sensor.status.framesize = frame_size;
    s_state->sensor.set_pixformat(&s_state->sensor, pix_format);
    if (s_state->sensor.set_framesize(&s_state->sensor, frame_size) != 0) {
        ESP_LOGE(TAG, "Failed to set frame size");
        err = ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE;
        goto fail;
    }

    cam_start();

    ESP_LOGI(TAG, "Camera initialized: JPEG mode, XCLK=%dMHz, framesize=%dx%d",
             config->xclk_freq_hz / 1000000,
             resolution[frame_size].width, resolution[frame_size].height);

    return ESP_OK;

fail:
    esp_camera_deinit();
    return err;
}

esp_err_t esp_camera_deinit()
{
    esp_err_t ret = cam_deinit();
    CAMERA_DISABLE_OUT_CLOCK();
    if (s_state) {
        SCCB_Deinit();
        free(s_state);
        s_state = NULL;
    }
    return ret;
}

#define FB_GET_TIMEOUT (4000 / portTICK_PERIOD_MS)

camera_fb_t *esp_camera_fb_get()
{
    if (s_state == NULL) {
        return NULL;
    }
    camera_fb_t *fb = cam_take(FB_GET_TIMEOUT);
    if (fb) {
        fb->width = resolution[s_state->sensor.status.framesize].width;
        fb->height = resolution[s_state->sensor.status.framesize].height;
        fb->format = s_state->sensor.pixformat;
    }
    return fb;
}

void esp_camera_fb_return(camera_fb_t *fb)
{
    if (s_state == NULL) {
        return;
    }
    cam_give(fb);
}

sensor_t *esp_camera_sensor_get()
{
    if (s_state == NULL) {
        return NULL;
    }
    return &s_state->sensor;
}

esp_err_t esp_camera_save_to_nvs(const char *key)
{
#if ESP_IDF_VERSION_MAJOR > 3
    nvs_handle_t handle;
#else
    nvs_handle handle;
#endif
    esp_err_t ret = nvs_open(key, NVS_READWRITE, &handle);

    if (ret == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s != NULL) {
            ret = nvs_set_blob(handle, CAMERA_SENSOR_NVS_KEY, &s->status, sizeof(camera_status_t));
            if (ret == ESP_OK) {
                uint8_t pf = s->pixformat;
                ret = nvs_set_u8(handle, CAMERA_PIXFORMAT_NVS_KEY, pf);
            }
            nvs_close(handle);
            return ret;
        } else {
            nvs_close(handle);
            return ESP_ERR_CAMERA_NOT_DETECTED;
        }
    } else {
        return ret;
    }
}

esp_err_t esp_camera_load_from_nvs(const char *key)
{
#if ESP_IDF_VERSION_MAJOR > 3
    nvs_handle_t handle;
#else
    nvs_handle handle;
#endif

    esp_err_t ret = nvs_open(key, NVS_READONLY, &handle);

    if (ret == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        camera_status_t st;
        if (s != NULL) {
            size_t size = sizeof(camera_status_t);
            ret = nvs_get_blob(handle, CAMERA_SENSOR_NVS_KEY, &st, &size);
            if (ret == ESP_OK) {
                s->set_brightness(s, st.brightness);
                s->set_contrast(s, st.contrast);
                s->set_saturation(s, st.saturation);
                s->set_special_effect(s, st.special_effect);
                s->set_wb_mode(s, st.wb_mode);
                s->set_hmirror(s, st.hmirror);
                s->set_vflip(s, st.vflip);
                s->set_framesize(s, st.framesize);
                s->set_quality(s, st.quality);
            }
            /* Always force JPEG regardless of NVS */
            s->set_pixformat(s, PIXFORMAT_JPEG);
        } else {
            nvs_close(handle);
            return ESP_ERR_CAMERA_NOT_DETECTED;
        }
        nvs_close(handle);
        return ret;
    } else {
        ESP_LOGW(TAG, "Error (%d) opening nvs key \"%s\"", ret, key);
        return ret;
    }
}

void esp_camera_return_all(void) {
    if (s_state == NULL) {
        return;
    }
    cam_give_all();
}
