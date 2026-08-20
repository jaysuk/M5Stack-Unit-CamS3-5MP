#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the AP-based Wi-Fi setup portal until working credentials are captured.
 *
 * Starts a Wi-Fi AP (APSTA mode) advertising as "PROV_<device id>" (open by default -- see
 * CONFIG_UNITCAMS3_AP_PASSWORD -- the standard tradeoff for a one-time setup portal), a DNS
 * server that resolves every query to the device's own AP IP so phones/laptops auto-launch their
 * "sign in to network" captive-portal prompt, and a small HTTP portal on port 80: GET / serves a
 * page that lists nearby networks (via GET /wifi/scan) and lets the user pick one and enter a
 * password; POST /wifi/connect test-connects with the submitted credentials before accepting
 * them, so a typo'd password doesn't get persisted and leave the device stuck retrying forever.
 *
 * Blocks until a working SSID/password pair is found and verified. On return, esp_wifi already
 * has those credentials in its own NVS-backed config (WIFI_STORAGE_FLASH, the driver default) --
 * this function disconnects and tears the AP/portal/DNS back down before returning, it does not
 * hand back a live connection. The caller still needs to bring STA mode up normally afterwards.
 *
 * Must be called after esp_wifi_init() (needs the driver ready) and before esp_wifi_start()
 * (owns the mode/AP-STA transitions for the duration of the portal).
 */
esp_err_t wifi_provision_run(void);

#ifdef __cplusplus
}
#endif
