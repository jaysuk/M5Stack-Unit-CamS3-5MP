#include "config_mgr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_mgr";

#define NVS_NAMESPACE "app_cfg"

/* NVS key names */
#define KEY_MQTT_URL   "mqtt_url"
#define KEY_MQTT_USER  "mqtt_user"
#define KEY_MQTT_PASS  "mqtt_pass"
#define KEY_DEVICE_ID  "device_id"
#define KEY_MQTT_EN    "mqtt_en"
#define KEY_CAM_RES    "cam_res"
#define KEY_JPEG_QUAL  "jpeg_qual"
#define KEY_OTA_TOKEN  "ota_token"
#define KEY_CD_TOKEN   "cd_token"
#define KEY_BRIGHTNESS "brightness"
#define KEY_CONTRAST   "contrast"
#define KEY_SATURATION "saturation"
#define KEY_WB_MODE    "wb_mode"
#define KEY_EXPOSURE   "exposure"
#define KEY_NEOPIXEL_EN  "neopixel_en"
#define KEY_NEOPIXEL_ON  "neopixel_on"
#define KEY_NEOPIXEL_BR  "neopixel_br"
#define KEY_NEOPIXEL_CNT "neopixel_cnt"
#define KEY_LED_ON       "led_on"

/* Field size limits (including null terminator) */
#define MQTT_URL_MAX   128
#define MQTT_USER_MAX  64
#define MQTT_PASS_MAX  64
#define DEVICE_ID_MAX  32
#define OTA_TOKEN_MAX  64
#define CD_TOKEN_MAX   64

/* Default framesize: FRAMESIZE_VGA = 10 */
#define DEFAULT_CAM_RES   10
#define DEFAULT_JPEG_QUAL 12

#if CONFIG_UNITCAMS3_BOARD_OV3660
/* ov3660.c takes brightness/contrast/saturation directly in its own signed
 * range -- 0 is neutral, no bias offset. */
#define DEFAULT_BRIGHTNESS 0
#define DEFAULT_CONTRAST   0
#define DEFAULT_SATURATION 0
#define BRIGHTNESS_MIN     (-3)
#define BRIGHTNESS_MAX     3
#define CONTRAST_MIN       (-3)
#define CONTRAST_MAX       3
#define SATURATION_MIN     (-4)
#define SATURATION_MAX     4
#else
/* mega_ccm.c register indices -- these are the sensor's neutral/"0" settings,
 * not 0. See config_mgr_get_brightness() etc. in the header. */
#define DEFAULT_BRIGHTNESS 4
#define DEFAULT_CONTRAST   3
#define DEFAULT_SATURATION 3
#define BRIGHTNESS_MIN     0
#define BRIGHTNESS_MAX     8
#define CONTRAST_MIN       0
#define CONTRAST_MAX       6
#define SATURATION_MIN     0
#define SATURATION_MAX     6
#endif
#define DEFAULT_WB_MODE    0

/* AE level (EV compensation): same range on both boards. Only ov3660.c implements
 * set_ae_level -- mega_ccm.c (PY260) wires it to a no-op, so range/default here just needs to be
 * something harmless to pass through unused on that board. */
#define DEFAULT_EXPOSURE   0
#define EXPOSURE_MIN       (-5)
#define EXPOSURE_MAX       5

/* NeoPixel ring: off and dim by default -- enabling it in /setup shouldn't itself pull a
 * surprise ~1A (16 WS2812 LEDs can each draw ~60mA at full white) or light up the ring until
 * the user explicitly turns it on. */
#define DEFAULT_NEOPIXEL_EN false
#define DEFAULT_NEOPIXEL_ON false
#define DEFAULT_NEOPIXEL_BRIGHTNESS 40
#define DEFAULT_NEOPIXEL_COUNT CONFIG_UNITCAMS3_NEOPIXEL_COUNT
#define NEOPIXEL_COUNT_MIN 1
#define NEOPIXEL_COUNT_MAX 300

/* Onboard LED: off by default, same reasoning as the NeoPixel ring -- shouldn't light up on
 * its own the first time this firmware boots with LED control added. */
#define DEFAULT_LED_ON false

/* In-memory config state */
static char s_mqtt_url[MQTT_URL_MAX];
static char s_mqtt_user[MQTT_USER_MAX];
static char s_mqtt_pass[MQTT_PASS_MAX];
static char s_device_id[DEVICE_ID_MAX];
static char s_ota_token[OTA_TOKEN_MAX];
static char s_cd_token[CD_TOKEN_MAX];
static bool    s_mqtt_en;
static uint8_t s_cam_res;
static uint8_t s_jpeg_qual;
static int8_t  s_brightness;
static int8_t  s_contrast;
static int8_t  s_saturation;
static uint8_t s_wb_mode;
static int8_t  s_exposure;
static bool     s_neopixel_en;
static bool     s_neopixel_on;
static uint8_t  s_neopixel_brightness;
static uint16_t s_neopixel_count;
static bool     s_led_on;

static bool s_initialized = false;

/* Load a string key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_size, const char *fallback)
{
    size_t len = dst_size;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(dst, fallback ? fallback : "", dst_size);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str(%s) err=0x%x, using fallback", key, err);
        strlcpy(dst, fallback ? fallback : "", dst_size);
    }
}

/* PY260-supported framesizes only; excludes SVGA(11) and XGA(12) which
 * mega_ccm.c does not handle. Shared by config_mgr_init() validation and the
 * cam_resolution setter so bad values cannot enter from any call site. */
static bool cam_res_is_valid(uint8_t v)
{
    static const uint8_t valid_res[] = {0, 1, 6, 7, 10, 13, 15, 16, 24};
    for (size_t i = 0; i < sizeof(valid_res); i++) {
        if (v == valid_res[i]) return true;
    }
    return false;
}

/* Load a u8 key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t fallback)
{
    esp_err_t err = nvs_get_u8(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = fallback;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8(%s) err=0x%x, using fallback", key, err);
        *dst = fallback;
    }
}

/* Load a signed i8 key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_i8(nvs_handle_t h, const char *key, int8_t *dst, int8_t fallback)
{
    esp_err_t err = nvs_get_i8(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = fallback;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_i8(%s) err=0x%x, using fallback", key, err);
        *dst = fallback;
    }
}

/* Load a u16 key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst, uint16_t fallback)
{
    esp_err_t err = nvs_get_u16(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = fallback;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u16(%s) err=0x%x, using fallback", key, err);
        *dst = fallback;
    }
}

esp_err_t config_mgr_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open(%s) err=0x%x — using all Kconfig defaults", NVS_NAMESPACE, err);
    }

    bool nvs_ok = (err == ESP_OK);
    nvs_handle_t dummy = 0;
    nvs_handle_t nh = nvs_ok ? h : dummy;

    /* String fields with Kconfig fallbacks */
    if (nvs_ok) {
        load_str(nh, KEY_MQTT_URL,  s_mqtt_url,  sizeof(s_mqtt_url),  CONFIG_UNITCAMS3_MQTT_BROKER_URL);
        load_str(nh, KEY_MQTT_USER, s_mqtt_user, sizeof(s_mqtt_user), CONFIG_UNITCAMS3_MQTT_USER);
        load_str(nh, KEY_MQTT_PASS, s_mqtt_pass, sizeof(s_mqtt_pass), CONFIG_UNITCAMS3_MQTT_PASS);
        load_str(nh, KEY_DEVICE_ID, s_device_id, sizeof(s_device_id), CONFIG_UNITCAMS3_DEVICE_ID);
        load_str(nh, KEY_OTA_TOKEN, s_ota_token, sizeof(s_ota_token), CONFIG_UNITCAMS3_OTA_TOKEN);
        load_str(nh, KEY_CD_TOKEN,  s_cd_token,  sizeof(s_cd_token),  CONFIG_UNITCAMS3_COREDUMP_TOKEN);
    } else {
        strlcpy(s_mqtt_url,  CONFIG_UNITCAMS3_MQTT_BROKER_URL, sizeof(s_mqtt_url));
        strlcpy(s_mqtt_user, CONFIG_UNITCAMS3_MQTT_USER,       sizeof(s_mqtt_user));
        strlcpy(s_mqtt_pass, CONFIG_UNITCAMS3_MQTT_PASS,       sizeof(s_mqtt_pass));
        strlcpy(s_device_id, CONFIG_UNITCAMS3_DEVICE_ID,       sizeof(s_device_id));
        strlcpy(s_ota_token, CONFIG_UNITCAMS3_OTA_TOKEN,       sizeof(s_ota_token));
        strlcpy(s_cd_token,  CONFIG_UNITCAMS3_COREDUMP_TOKEN,  sizeof(s_cd_token));
        s_mqtt_en  = true;
        s_cam_res  = DEFAULT_CAM_RES;
        s_jpeg_qual = DEFAULT_JPEG_QUAL;
        s_brightness = DEFAULT_BRIGHTNESS;
        s_contrast   = DEFAULT_CONTRAST;
        s_saturation = DEFAULT_SATURATION;
        s_wb_mode    = DEFAULT_WB_MODE;
        s_exposure   = DEFAULT_EXPOSURE;
        s_neopixel_en = DEFAULT_NEOPIXEL_EN;
        s_neopixel_on = DEFAULT_NEOPIXEL_ON;
        s_neopixel_brightness = DEFAULT_NEOPIXEL_BRIGHTNESS;
        s_neopixel_count = DEFAULT_NEOPIXEL_COUNT;
        s_led_on = DEFAULT_LED_ON;
        s_initialized = true;
        return ESP_OK;
    }

    /* Numeric fields */
    uint8_t mqtt_en_u8 = 1;
    load_u8(nh, KEY_MQTT_EN,   &mqtt_en_u8,   1);
    load_u8(nh, KEY_CAM_RES,   &s_cam_res,    DEFAULT_CAM_RES);
    load_u8(nh, KEY_JPEG_QUAL, &s_jpeg_qual,  DEFAULT_JPEG_QUAL);
    load_i8(nh, KEY_BRIGHTNESS, &s_brightness, DEFAULT_BRIGHTNESS);
    load_i8(nh, KEY_CONTRAST,   &s_contrast,   DEFAULT_CONTRAST);
    load_i8(nh, KEY_SATURATION, &s_saturation, DEFAULT_SATURATION);
    load_u8(nh, KEY_WB_MODE,    &s_wb_mode,    DEFAULT_WB_MODE);
    load_i8(nh, KEY_EXPOSURE,   &s_exposure,   DEFAULT_EXPOSURE);
    uint8_t neopixel_en_u8 = DEFAULT_NEOPIXEL_EN ? 1 : 0;
    uint8_t neopixel_on_u8 = DEFAULT_NEOPIXEL_ON ? 1 : 0;
    load_u8(nh, KEY_NEOPIXEL_EN, &neopixel_en_u8, DEFAULT_NEOPIXEL_EN ? 1 : 0);
    load_u8(nh, KEY_NEOPIXEL_ON, &neopixel_on_u8, DEFAULT_NEOPIXEL_ON ? 1 : 0);
    load_u8(nh, KEY_NEOPIXEL_BR, &s_neopixel_brightness, DEFAULT_NEOPIXEL_BRIGHTNESS);
    load_u16(nh, KEY_NEOPIXEL_CNT, &s_neopixel_count, DEFAULT_NEOPIXEL_COUNT);
    uint8_t led_on_u8 = DEFAULT_LED_ON ? 1 : 0;
    load_u8(nh, KEY_LED_ON, &led_on_u8, DEFAULT_LED_ON ? 1 : 0);
    s_led_on = (led_on_u8 != 0);
    s_neopixel_en = (neopixel_en_u8 != 0);
    s_neopixel_on = (neopixel_on_u8 != 0);
    s_mqtt_en = (mqtt_en_u8 != 0);
    if (s_neopixel_count < NEOPIXEL_COUNT_MIN || s_neopixel_count > NEOPIXEL_COUNT_MAX) {
        ESP_LOGW(TAG, "neopixel_count=%u out of range (%u-%u) — resetting to default (%u)",
                 s_neopixel_count, NEOPIXEL_COUNT_MIN, NEOPIXEL_COUNT_MAX, DEFAULT_NEOPIXEL_COUNT);
        s_neopixel_count = DEFAULT_NEOPIXEL_COUNT;
    }
    /* Also catches a value persisted under the other board's range (e.g. NVS
     * carried over from a PY260 build after switching to OV3660) -- reset to
     * neutral rather than pass a stale out-of-range value to the sensor. */
    if (s_brightness < BRIGHTNESS_MIN || s_brightness > BRIGHTNESS_MAX) s_brightness = DEFAULT_BRIGHTNESS;
    if (s_contrast   < CONTRAST_MIN   || s_contrast   > CONTRAST_MAX)   s_contrast   = DEFAULT_CONTRAST;
    if (s_saturation < SATURATION_MIN || s_saturation > SATURATION_MAX) s_saturation = DEFAULT_SATURATION;
    if (s_wb_mode    > 4) s_wb_mode    = DEFAULT_WB_MODE;
    if (s_exposure   < EXPOSURE_MIN   || s_exposure   > EXPOSURE_MAX)   s_exposure   = DEFAULT_EXPOSURE;

    /* Validate cam_res against PY260-supported framesizes.
     * Reject values that mega_ccm.c does not handle — previously stored
     * values (e.g. 8=CIF, 9=HVGA from before the enum fix) would cause
     * esp_camera_init() to fail. Reset to default if invalid. */
    if (!cam_res_is_valid(s_cam_res)) {
        ESP_LOGW(TAG, "cam_res=%u not supported by PY260 — resetting to VGA (%u)",
                 s_cam_res, DEFAULT_CAM_RES);
        s_cam_res = DEFAULT_CAM_RES;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: url=%s user=%s dev=%s mqtt_en=%d res=%u qual=%u",
             s_mqtt_url, s_mqtt_user, s_device_id, s_mqtt_en, s_cam_res, s_jpeg_qual);

    s_initialized = true;
    return ESP_OK;
}

/* --- Getters --- */

const char *config_mgr_get_mqtt_url(void)      { return s_mqtt_url; }
const char *config_mgr_get_mqtt_user(void)     { return s_mqtt_user; }
const char *config_mgr_get_mqtt_pass(void)     { return s_mqtt_pass; }
const char *config_mgr_get_device_id(void)     { return s_device_id; }
bool        config_mgr_is_mqtt_enabled(void)   { return s_mqtt_en; }
uint8_t     config_mgr_get_cam_resolution(void){ return s_cam_res; }
uint8_t     config_mgr_get_jpeg_quality(void)  { return s_jpeg_qual; }
const char *config_mgr_get_ota_token(void)     { return s_ota_token; }
const char *config_mgr_get_coredump_token(void) { return s_cd_token; }
int8_t      config_mgr_get_brightness(void)    { return s_brightness; }
int8_t      config_mgr_get_contrast(void)      { return s_contrast; }
int8_t      config_mgr_get_saturation(void)    { return s_saturation; }
uint8_t     config_mgr_get_wb_mode(void)       { return s_wb_mode; }
int8_t      config_mgr_get_exposure(void)      { return s_exposure; }
bool        config_mgr_is_neopixel_enabled(void)      { return s_neopixel_en; }
bool        config_mgr_get_neopixel_on(void)           { return s_neopixel_on; }
uint8_t     config_mgr_get_neopixel_brightness(void)   { return s_neopixel_brightness; }
uint16_t    config_mgr_get_neopixel_count(void)        { return s_neopixel_count; }
bool        config_mgr_get_led_on(void)                { return s_led_on; }

/* --- Setters (update in-memory only) --- */

void config_mgr_set_mqtt_url(const char *v)      { strlcpy(s_mqtt_url,  v, sizeof(s_mqtt_url)); }
void config_mgr_set_mqtt_user(const char *v)     { strlcpy(s_mqtt_user, v, sizeof(s_mqtt_user)); }
void config_mgr_set_mqtt_pass(const char *v)     { strlcpy(s_mqtt_pass, v, sizeof(s_mqtt_pass)); }
void config_mgr_set_device_id(const char *v)     { strlcpy(s_device_id, v, sizeof(s_device_id)); }
void config_mgr_set_mqtt_enabled(bool v)         { s_mqtt_en = v; }
void config_mgr_set_cam_resolution(uint8_t v)
{
    if (!cam_res_is_valid(v)) {
        ESP_LOGW(TAG, "set_cam_resolution: %u not PY260-supported — ignoring", v);
        return;
    }
    s_cam_res = v;
}
void config_mgr_set_jpeg_quality(uint8_t v)
{
    if (v < 1 || v > 63) {
        ESP_LOGW(TAG, "set_jpeg_quality: %u out of range (1-63) — ignoring", v);
        return;
    }
    s_jpeg_qual = v;
}
void config_mgr_set_ota_token(const char *v)     { strlcpy(s_ota_token, v, sizeof(s_ota_token)); }
void config_mgr_set_coredump_token(const char *v) { strlcpy(s_cd_token,  v, sizeof(s_cd_token)); }
void config_mgr_set_brightness(int8_t v)
{
    if (v < BRIGHTNESS_MIN || v > BRIGHTNESS_MAX) {
        ESP_LOGW(TAG, "set_brightness: %d out of range (%d..%d) — ignoring", v, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        return;
    }
    s_brightness = v;
}
void config_mgr_set_contrast(int8_t v)
{
    if (v < CONTRAST_MIN || v > CONTRAST_MAX) {
        ESP_LOGW(TAG, "set_contrast: %d out of range (%d..%d) — ignoring", v, CONTRAST_MIN, CONTRAST_MAX);
        return;
    }
    s_contrast = v;
}
void config_mgr_set_saturation(int8_t v)
{
    if (v < SATURATION_MIN || v > SATURATION_MAX) {
        ESP_LOGW(TAG, "set_saturation: %d out of range (%d..%d) — ignoring", v, SATURATION_MIN, SATURATION_MAX);
        return;
    }
    s_saturation = v;
}
void config_mgr_set_wb_mode(uint8_t v)
{
    if (v > 4) { ESP_LOGW(TAG, "set_wb_mode: %u out of range (0-4) — ignoring", v); return; }
    s_wb_mode = v;
}
void config_mgr_set_exposure(int8_t v)
{
    if (v < EXPOSURE_MIN || v > EXPOSURE_MAX) {
        ESP_LOGW(TAG, "set_exposure: %d out of range (%d..%d) — ignoring", v, EXPOSURE_MIN, EXPOSURE_MAX);
        return;
    }
    s_exposure = v;
}
void config_mgr_set_neopixel_enabled(bool v)     { s_neopixel_en = v; }
void config_mgr_set_neopixel_on(bool v)          { s_neopixel_on = v; }
void config_mgr_set_neopixel_brightness(uint8_t v) { s_neopixel_brightness = v; }
void config_mgr_set_neopixel_count(uint16_t v)
{
    if (v < NEOPIXEL_COUNT_MIN || v > NEOPIXEL_COUNT_MAX) {
        ESP_LOGW(TAG, "set_neopixel_count: %u out of range (%u-%u) — ignoring", v, NEOPIXEL_COUNT_MIN, NEOPIXEL_COUNT_MAX);
        return;
    }
    s_neopixel_count = v;
}
void config_mgr_set_led_on(bool v)               { s_led_on = v; }

/* --- Persist to NVS --- */

esp_err_t config_mgr_save(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "config_mgr_save called before init");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(READWRITE) err=0x%x", err);
        return err;
    }

    /* Abort on the first failed write so a half-updated config is never
     * committed. nvs_set_* only stages values; commit makes them durable. */
    err = nvs_set_str(h, KEY_MQTT_URL,  s_mqtt_url);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_USER, s_mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_PASS, s_mqtt_pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_DEVICE_ID, s_device_id);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_OTA_TOKEN, s_ota_token);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CD_TOKEN,  s_cd_token);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_MQTT_EN,   s_mqtt_en ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_CAM_RES,   s_cam_res);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_JPEG_QUAL, s_jpeg_qual);
    if (err == ESP_OK) err = nvs_set_i8(h,  KEY_BRIGHTNESS, s_brightness);
    if (err == ESP_OK) err = nvs_set_i8(h,  KEY_CONTRAST,   s_contrast);
    if (err == ESP_OK) err = nvs_set_i8(h,  KEY_SATURATION, s_saturation);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_WB_MODE,    s_wb_mode);
    if (err == ESP_OK) err = nvs_set_i8(h,  KEY_EXPOSURE,   s_exposure);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_NEOPIXEL_EN, s_neopixel_en ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_NEOPIXEL_ON, s_neopixel_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_NEOPIXEL_BR, s_neopixel_brightness);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_NEOPIXEL_CNT, s_neopixel_count);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_LED_ON, s_led_on ? 1 : 0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set failed err=0x%x — aborting save without commit", err);
        nvs_close(h);
        return err;
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit err=0x%x", err);
    } else {
        ESP_LOGI(TAG, "Config saved to NVS");
    }
    return err;
}
