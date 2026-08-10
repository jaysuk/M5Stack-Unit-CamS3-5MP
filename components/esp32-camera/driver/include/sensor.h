/*
 * Sensor abstraction layer.
 * Stripped to MEGA_CCM (PY260) + JPEG-only for UnitCamS3-5MP.
 *
 * Original: OpenMV project, Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * Licensed under the MIT license.
 */
#ifndef __SENSOR_H__
#define __SENSOR_H__
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEGA_CCM_PID = 0x039E,
    OV3660_PID = 0x3660,
} camera_pid_t;

typedef enum {
    CAMERA_MEGA_CCM,
    CAMERA_OV3660,
    CAMERA_MODEL_MAX,
    CAMERA_NONE,
} camera_model_t;

typedef enum {
    MEGA_CCM_SCCB_ADDR = 0x1F,// 0x3E >> 1
    OV3660_SCCB_ADDR   = 0x3C,// 0x78 >> 1
} camera_sccb_addr_t;

/* JPEG is the only supported pixel format for this firmware.
 * Other formats kept as enum values for driver interface compat. */
typedef enum {
    PIXFORMAT_RGB565,    // UNSUPPORTED
    PIXFORMAT_YUV422,    // UNSUPPORTED
    PIXFORMAT_YUV420,    // UNSUPPORTED
    PIXFORMAT_GRAYSCALE, // UNSUPPORTED
    PIXFORMAT_JPEG,      // ** THE ONLY SUPPORTED FORMAT **
    PIXFORMAT_RGB888,    // UNSUPPORTED
    PIXFORMAT_RAW,       // UNSUPPORTED
    PIXFORMAT_RGB444,    // UNSUPPORTED
    PIXFORMAT_RGB555,    // UNSUPPORTED
} pixformat_t;

typedef enum {
    RST_PIN_LOW,
    RST_PIN_HIGHT,
} camera_test_t;

typedef enum {
    brightness_0,
    brightness_1,
    brightness_2,
    brightness_3,
    brightness_4,
    brightness_5,
    brightness_6,
    brightness_7,
    brightness_8,
} BRIGHTNESS_t;

typedef enum {
    contrast_0,
    contrast_1,
    contrast_2,
    contrast_3,
    contrast_4,
    contrast_5,
    contrast_6,
} CONTRAST_t;

typedef enum {
    saturation_0,
    saturation_1,
    saturation_2,
    saturation_3,
    saturation_4,
    saturation_5,
    saturation_6,
} SATURATION_t;

typedef enum {
    exposure_0,
    exposure_1,
    exposure_2,
    exposure_3,
    exposure_4,
    exposure_5,
    exposure_6,
} EXPOSURE_t;

typedef enum {
    Auto,
    sunny,
    office,
    cloudy,
    home,
} AWB_MODE;

typedef enum {
    normal,
    blueish,
    redish,
    BorW,
    sepia,
    negative,
    greenish,
} SPECIAL;

typedef enum {
    quality_high,
    quality_default,
    quality_low,
} IMAGE_QUALITY;

typedef enum {
    mirror_disable,
    mirror_enable,
} IMAGE_MIRROR;

typedef enum {
    flip_disable,
    flip_enable,
} IMAGE_FLIP;

typedef enum {
    AGC_Auto,
    AGC_Manual,
} AGC_MODE;

/* Full frame size enum kept for driver compat.
 * PY260 actually supports: QVGA, VGA, HD, UXGA, FHD, 5MP,
 * 96x96, 128x128, 320x320 via register commands. */
typedef enum {
    FRAMESIZE_96X96,    // 96x96
    FRAMESIZE_128x128,  // 128x128
    FRAMESIZE_QQVGA,    // 160x120
    FRAMESIZE_QCIF,     // 176x144
    FRAMESIZE_HQVGA,    // 240x176
    FRAMESIZE_240X240,  // 240x240
    FRAMESIZE_QVGA,     // 320x240   <-- PY260
    FRAMESIZE_320x320,  // 320x320   <-- PY260
    FRAMESIZE_CIF,      // 400x296
    FRAMESIZE_HVGA,     // 480x320
    FRAMESIZE_VGA,      // 640x480   <-- PY260 (default)
    FRAMESIZE_SVGA,     // 800x600
    FRAMESIZE_XGA,      // 1024x768
    FRAMESIZE_HD,       // 1280x720  <-- PY260
    FRAMESIZE_SXGA,     // 1280x1024
    FRAMESIZE_UXGA,     // 1600x1200 <-- PY260
    FRAMESIZE_FHD,      // 1920x1080 <-- PY260
    FRAMESIZE_P_HD,     // 720x1280
    FRAMESIZE_P_3MP,    // 864x1536
    FRAMESIZE_QXGA,     // 2048x1536
    FRAMESIZE_QHD,      // 2560x1440
    FRAMESIZE_WQXGA,    // 2560x1600
    FRAMESIZE_P_FHD,    // 1080x1920
    FRAMESIZE_QSXGA,    // 2560x1920
    FRAMESIZE_5MP,      // 2592x1944 <-- PY260
    FRAMESIZE_INVALID
} framesize_t;

typedef struct {
    const camera_model_t model;
    const char *name;
    const camera_sccb_addr_t sccb_addr;
    const camera_pid_t pid;
    const framesize_t max_size;
    const bool support_jpeg;
} camera_sensor_info_t;

typedef enum {
    ASPECT_RATIO_4X3,
    ASPECT_RATIO_3X2,
    ASPECT_RATIO_16X10,
    ASPECT_RATIO_5X3,
    ASPECT_RATIO_16X9,
    ASPECT_RATIO_21X9,
    ASPECT_RATIO_5X4,
    ASPECT_RATIO_1X1,
    ASPECT_RATIO_9X16
} aspect_ratio_t;

typedef enum {
    GAINCEILING_2X,
    GAINCEILING_4X,
    GAINCEILING_8X,
    GAINCEILING_16X,
    GAINCEILING_32X,
    GAINCEILING_64X,
    GAINCEILING_128X,
} gainceiling_t;

typedef struct {
        uint16_t max_width;
        uint16_t max_height;
        uint16_t start_x;
        uint16_t start_y;
        uint16_t end_x;
        uint16_t end_y;
        uint16_t offset_x;
        uint16_t offset_y;
        uint16_t total_x;
        uint16_t total_y;
} ratio_settings_t;

typedef struct {
        const uint16_t width;
        const uint16_t height;
        const aspect_ratio_t aspect_ratio;
} resolution_info_t;

extern const resolution_info_t resolution[];
extern const camera_sensor_info_t camera_sensor[];

typedef struct {
    uint8_t MIDH;
    uint8_t MIDL;
    uint16_t PID;
    uint8_t VER;
} sensor_id_t;

typedef struct {
    framesize_t framesize;
    bool scale;
    bool binning;
    uint8_t quality;
    int8_t brightness;
    int8_t contrast;
    int8_t saturation;
    int8_t sharpness;
    uint8_t denoise;
    uint8_t special_effect;
    uint8_t wb_mode;
    uint8_t AGC_mode;
    uint8_t awb;
    uint8_t awb_gain;
    uint8_t aec;
    uint8_t aec2;
    int8_t   ae_level;
    uint16_t aec_value;
    uint8_t agc;
    uint8_t agc_gain;
    uint8_t gainceiling;
    uint8_t bpc;
    uint8_t wpc;
    uint8_t raw_gma;
    uint8_t lenc;
    uint8_t hmirror;
    uint8_t vflip;
    uint8_t dcw;
    uint8_t colorbar;
} camera_status_t;

typedef struct _sensor sensor_t;
typedef struct _sensor {
    sensor_id_t id;
    uint8_t  slv_addr;
    pixformat_t pixformat;
    camera_status_t status;
    int xclk_freq_hz;

    int  (*set_Camera_rest)     (sensor_t *sensor, int level);
    int  (*set_pixformat)       (sensor_t *sensor, pixformat_t pixformat);
    int  (*set_framesize)       (sensor_t *sensor, framesize_t framesize);
    int  (*set_brightness)      (sensor_t *sensor, int level);
    int  (*set_contrast)        (sensor_t *sensor, int level);
    int  (*set_saturation)      (sensor_t *sensor, int level);
    int  (*set_exposure_ctrl)   (sensor_t *sensor, int enable);
    int  (*set_wb_mode)        (sensor_t *sensor, int mode);
    int  (*set_special_effect)  (sensor_t *sensor, int effect);
    int  (*set_quality)         (sensor_t *sensor, int quality);
    int  (*set_AGC_mode)        (sensor_t *sensor, int mode);
    int  (*set_agc_gain)        (sensor_t *sensor, int gain);
    int  (*set_mamual_exp_h)    (sensor_t *sensor, int level);
    int  (*set_mamual_exp_l)    (sensor_t *sensor, int level);

    int  (*init_status)         (sensor_t *sensor);
    int  (*reset)               (sensor_t *sensor);
    int  (*set_sharpness)       (sensor_t *sensor, int level);
    int  (*set_denoise)         (sensor_t *sensor, int level);
    int  (*set_gainceiling)     (sensor_t *sensor, gainceiling_t gainceiling);
    int  (*set_colorbar)        (sensor_t *sensor, int enable);
    int  (*set_whitebal)        (sensor_t *sensor, int enable);
    int  (*set_gain_ctrl)       (sensor_t *sensor, int enable);
    int  (*set_hmirror)         (sensor_t *sensor, int enable);
    int  (*set_vflip)           (sensor_t *sensor, int enable);
    int  (*set_aec2)            (sensor_t *sensor, int enable);
    int  (*set_awb_gain)        (sensor_t *sensor, int enable);
    int  (*set_aec_value)       (sensor_t *sensor, int gain);
    int  (*set_ae_level)        (sensor_t *sensor, int level);
    int  (*set_dcw)             (sensor_t *sensor, int enable);
    int  (*set_bpc)             (sensor_t *sensor, int enable);
    int  (*set_wpc)             (sensor_t *sensor, int enable);
    int  (*set_raw_gma)         (sensor_t *sensor, int enable);
    int  (*set_lenc)            (sensor_t *sensor, int enable);
    int  (*get_reg)             (sensor_t *sensor, int reg, int mask);
    int  (*set_reg)             (sensor_t *sensor, int reg, int mask, int value);
    int  (*set_res_raw)         (sensor_t *sensor, int startX, int startY, int endX, int endY, int offsetX, int offsetY, int totalX, int totalY, int outputX, int outputY, bool scale, bool binning);
    int  (*set_pll)             (sensor_t *sensor, int bypass, int mul, int sys, int root, int pre, int seld5, int pclken, int pclk);
    int  (*set_xclk)            (sensor_t *sensor, int timer, int xclk);
} sensor_t;

camera_sensor_info_t *esp_camera_sensor_get_info(sensor_id_t *id);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_H__ */
