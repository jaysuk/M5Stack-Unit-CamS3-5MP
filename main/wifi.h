#pragma once

#include "esp_err.h"

/**
 * @brief Initializes NVS, Wi-Fi, and blocks until an IP is acquired.
 *
 * On first boot (no credentials in NVS): starts the AP-based setup portal (see
 * wifi_provision.h) -- the device advertises as "PROV_<device id>", connect to it and a
 * browser-based captive portal opens automatically to pick a network and enter its password.
 * Credentials are stored in NVS by the Wi-Fi driver itself.
 *
 * On subsequent boots: connects directly using stored credentials.
 */
esp_err_t wifi_init_sta(void);

typedef struct {
    int rssi;
    uint32_t disconnect_count;
    char ip[16];
} wifi_stats_t;

void wifi_get_stats(wifi_stats_t *stats);

/**
 * @brief Erase Wi-Fi credentials and reboot into the AP-based setup portal.
 *
 * Stores a magic value in RTC_NOINIT memory (survives esp_restart()),
 * signals a planned reboot (prevents boot-loop counter from tripping),
 * then calls esp_restart(). On the next boot, wifi_init_sta() detects
 * the magic and calls esp_wifi_restore() before the provisioning check,
 * causing the device to enter setup-AP mode (see wifi_provision.h).
 *
 * Safe to call while camera DMA is running: uses RTC SRAM only, no flash write.
 * The NVS erase happens on the next boot, before esp_camera_init().
 */
void wifi_start_reprovision(void);
