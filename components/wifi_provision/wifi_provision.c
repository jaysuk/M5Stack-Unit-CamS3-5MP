#include "wifi_provision.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "wifi_provision";

#define AP_IP_ADDR "192.168.4.1" // esp_netif_create_default_wifi_ap()'s default AP IP
#define SCAN_MAX_APS 20

static EventGroupHandle_t s_result_group;
#define RESULT_CONNECTED_BIT BIT0
#define RESULT_FAILED_BIT    BIT1

static httpd_handle_t s_portal_server = NULL;
static volatile bool s_dns_running = false;
static TaskHandle_t s_dns_task = NULL;

// ---------------------------------------------------------------------------
// Minimal DNS server: answers every A-record query with our own AP IP, so a
// phone/laptop connecting to the setup AP sees "no real internet" and pops its
// captive-portal sign-in prompt. Not a general-purpose resolver -- it doesn't
// need to be, it only has to satisfy the OS's own captive-portal probe.
// ---------------------------------------------------------------------------
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket() failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t ap_ip = inet_addr(AP_IP_ADDR);

    while (s_dns_running) {
        uint8_t req[512];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from, &fromlen);
        if (len < 12) {
            continue; // too short to be a DNS header, or timed out (len < 0)
        }

        // Walk the question name (length-prefixed labels, no compression -- the first
        // question never uses a pointer) to find where it ends.
        int qend = 12;
        while (qend < len && req[qend] != 0) {
            qend += req[qend] + 1;
            if (qend >= (int)sizeof(req) - 16) {
                qend = -1; // malformed/oversized -- bail
                break;
            }
        }
        if (qend < 0 || qend >= len) {
            continue;
        }
        qend += 5; // null label terminator (1) + QTYPE (2) + QCLASS (2)
        if (qend > len || qend > (int)sizeof(req) - 16) {
            continue;
        }

        uint8_t resp[528];
        memcpy(resp, req, qend);
        resp[2] = 0x81; // QR=1 (response), Opcode=0, AA=1, TC=0, RD=1 (echoed)
        resp[3] = 0x80; // RA=1, rest 0
        resp[6] = 0x00; resp[7] = 0x01; // ANCOUNT = 1

        int pos = qend;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C; // name: pointer to the question at offset 12
        resp[pos++] = 0x00; resp[pos++] = 0x01; // TYPE A
        resp[pos++] = 0x00; resp[pos++] = 0x01; // CLASS IN
        resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x3C; // TTL 60s
        resp[pos++] = 0x00; resp[pos++] = 0x04; // RDLENGTH = 4
        memcpy(resp + pos, &ap_ip, 4);
        pos += 4;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&from, fromlen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Portal event handling: only cares about the outcome of a connect attempt
// triggered from wifi_connect_handler() below. Registered/unregistered for
// the lifetime of wifi_provision_run() only.
// ---------------------------------------------------------------------------
static void result_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_result_group, RESULT_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_result_group, RESULT_FAILED_BIT);
    }
}

// ---------------------------------------------------------------------------
// HTTP portal
// ---------------------------------------------------------------------------

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:32px auto;padding:0 16px}"
    "label{display:block;margin-top:14px;font-weight:bold}"
    "select,input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;font-size:1em}"
    "input[type=submit]{margin-top:20px;padding:10px 24px;background:#2563eb;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:1em}"
    "input[type=submit]:disabled{background:#93c5fd}"
    "#status{margin-top:12px;font-size:0.9em}"
    "</style></head><body>"
    "<h2>Camera Wi-Fi Setup</h2>"
    "<p style='color:#6b7280;font-size:0.9em'>Pick your Wi-Fi network and enter its password. "
    "The camera will connect and this setup network will disappear.</p>"
    "<label>Network</label>"
    "<select id='ssid_sel'><option value=''>Scanning&hellip;</option></select>"
    "<label>Or enter SSID manually (for hidden networks)</label>"
    "<input type='text' id='ssid_manual' placeholder='Leave blank to use the dropdown above'>"
    "<label>Password</label>"
    "<input type='password' id='pass'>"
    "<input type='submit' id='btn' value='Connect' onclick='doConnect()'>"
    "<p id='status'></p>"
    "<script>"
    "function loadScan(){"
    "fetch('/wifi/scan').then(function(r){return r.json();}).then(function(list){"
    "var sel=document.getElementById('ssid_sel');sel.innerHTML='';"
    "if(list.length===0){sel.innerHTML=\"<option value=''>No networks found -- rescan or enter manually</option>\";return;}"
    "list.forEach(function(ap){"
    "var o=document.createElement('option');o.value=ap.ssid;"
    "o.textContent=ap.ssid+' ('+ap.rssi+' dBm)'+(ap.secure?' \\uD83D\\uDD12':'');"
    "sel.appendChild(o);});"
    "}).catch(function(){"
    "document.getElementById('ssid_sel').innerHTML=\"<option value=''>Scan failed -- enter SSID manually</option>\";"
    "});}"
    "function doConnect(){"
    "var ssid=document.getElementById('ssid_manual').value||document.getElementById('ssid_sel').value;"
    "var pass=document.getElementById('pass').value;"
    "var st=document.getElementById('status');"
    "if(!ssid){st.textContent='Choose or enter a network name first.';return;}"
    "document.getElementById('btn').disabled=true;"
    "st.textContent='Connecting to '+ssid+'\\u2026 (this can take up to 15s)';"
    "fetch('/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)}).then(function(r){"
    "return r.json().then(function(j){return {ok:r.ok,j:j};});"
    "}).then(function(res){"
    "if(res.ok&&res.j.ok){st.textContent='Connected! The camera is rebooting onto your network.';}"
    "else{st.textContent=(res.j&&res.j.error)?res.j.error:'Connection failed.';document.getElementById('btn').disabled=false;}"
    "}).catch(function(e){st.textContent='Request failed: '+e.message;document.getElementById('btn').disabled=false;});}"
    "loadScan();"
    "</script>"
    "</body></html>";

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
}

// Common captive-portal probe paths (Android/iOS/Windows) -- redirect them all to "/" so the OS's
// "sign in to network" prompt opens the portal instead of reporting a dead-end/no-internet AP.
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_ADDR "/");
    return httpd_resp_send(req, NULL, 0);
}

// Minimal in-place '%XX'/'+' URL-decoder, same approach as http_server.c's url_decode().
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

// JSON-escape an SSID for /wifi/scan's response -- SSIDs are arbitrary bytes up to 32 chars, so
// this can't assume they're already JSON-safe.
static void json_escape(char *dst, const char *src, size_t src_len, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && src[si] != '\0' && di + 2 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            di += snprintf(dst + di, dst_size - di, "\\u%04x", c);
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

// GET /wifi/scan -- blocking scan, returns nearby APs as a JSON array, strongest-signal
// duplicate SSIDs collapsed to one entry, hidden (empty-SSID) networks omitted since there's
// nothing useful to show for them in a picker (still connectable via the manual SSID field).
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t num = SCAN_MAX_APS;
    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    if (!records) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&num, records);

    size_t bufsize = 4096;
    char *buf = malloc(bufsize);
    if (!buf) {
        free(records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = 0;
    buf[pos++] = '[';
    bool first = true;
    for (int i = 0; i < num; i++) {
        size_t ssid_len = strnlen((const char *)records[i].ssid, sizeof(records[i].ssid));
        if (ssid_len == 0) {
            continue; // hidden network -- nothing to show
        }
        // Dedupe: skip if an earlier (== stronger, esp_wifi sorts by RSSI) entry already had
        // this exact SSID.
        bool dup = false;
        for (int j = 0; j < i; j++) {
            size_t jlen = strnlen((const char *)records[j].ssid, sizeof(records[j].ssid));
            if (jlen == ssid_len && memcmp(records[i].ssid, records[j].ssid, ssid_len) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        char esc[96];
        json_escape(esc, (const char *)records[i].ssid, ssid_len, sizeof(esc));
        int n = snprintf(buf + pos, bufsize - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                          first ? "" : ",", esc, (int)records[i].rssi,
                          records[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
        if (n < 0 || pos + n >= (int)bufsize - 2) {
            break; // out of room -- stop rather than overflow
        }
        pos += n;
        first = false;
    }
    buf[pos++] = ']';

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, buf, pos);
    free(buf);
    free(records);
    return send_err;
}

// POST /wifi/connect -- form body ssid=...&password=... . Test-connects before reporting
// success, so a wrong password or an out-of-range AP doesn't get persisted and leave the device
// stuck retrying forever on every future boot.
static esp_err_t wifi_connect_handler(httpd_req_t *req)
{
#define CONNECT_BODY_MAX 160
    char body[CONNECT_BODY_MAX + 1];
    int total = req->content_len;
    if (total > CONNECT_BODY_MAX) {
        total = CONNECT_BODY_MAX;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        received += r;
    }
    body[received > 0 ? received : 0] = '\0';

    char ssid_raw[64] = {0}, pass_raw[64] = {0};
    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(pair, "ssid") == 0) {
                url_decode(ssid_raw, eq + 1, sizeof(ssid_raw));
            } else if (strcmp(pair, "password") == 0) {
                url_decode(pass_raw, eq + 1, sizeof(pass_raw));
            }
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    httpd_resp_set_type(req, "application/json");
    if (ssid_raw[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"SSID must not be empty\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Test-connecting to SSID: %s", ssid_raw);

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid_raw, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass_raw, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = pass_raw[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_result_group, RESULT_CONNECTED_BIT | RESULT_FAILED_BIT);
    esp_wifi_disconnect(); // drop any prior test-connect attempt first
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"Could not start connection attempt\"}", HTTPD_RESP_USE_STRLEN);
    }

    EventBits_t bits = xEventGroupWaitBits(s_result_group, RESULT_CONNECTED_BIT | RESULT_FAILED_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & RESULT_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Test-connect succeeded -- credentials verified and saved");
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGW(TAG, "Test-connect to %s failed or timed out", ssid_raw);
    esp_wifi_disconnect();
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"Could not connect -- check the password and try again.\"}", HTTPD_RESP_USE_STRLEN);
#undef CONNECT_BODY_MAX
}

esp_err_t wifi_provision_run(void)
{
    ESP_LOGI(TAG, "No Wi-Fi credentials -- starting setup AP \"PROV_%s\"", CONFIG_UNITCAMS3_DEVICE_ID);

    esp_netif_create_default_wifi_ap();

    s_result_group = xEventGroupCreate();
    esp_event_handler_instance_t ip_handler_inst, wifi_handler_inst;
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &result_event_handler, NULL, &ip_handler_inst);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &result_event_handler, NULL, &wifi_handler_inst);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    int ssid_len = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "PROV_%s", CONFIG_UNITCAMS3_DEVICE_ID);
    if (ssid_len > (int)sizeof(ap_cfg.ap.ssid)) {
        ssid_len = sizeof(ap_cfg.ap.ssid); // snprintf's return value ignores truncation -- clamp to what was actually written
    }
    ap_cfg.ap.ssid_len = ssid_len > 0 ? (uint8_t)ssid_len : 0;
    if (CONFIG_UNITCAMS3_AP_PASSWORD[0] != '\0') {
        strlcpy((char *)ap_cfg.ap.password, CONFIG_UNITCAMS3_AP_PASSWORD, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    s_dns_running = true;
    xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, 5, &s_dns_task);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = 80;
    http_cfg.max_uri_handlers = 8;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&s_portal_server, &http_cfg));

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = portal_root_handler };
    httpd_register_uri_handler(s_portal_server, &root_uri);
    httpd_uri_t scan_uri = { .uri = "/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler };
    httpd_register_uri_handler(s_portal_server, &scan_uri);
    httpd_uri_t connect_uri = { .uri = "/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler };
    httpd_register_uri_handler(s_portal_server, &connect_uri);
    // Catch-all for OS captive-portal probes (Android /generate_204, Apple /hotspot-detect.html,
    // Windows /ncsi.txt, etc.) and anything else -- send them all to the portal page.
    httpd_uri_t catchall_uri = { .uri = "/*", .method = HTTP_GET, .handler = portal_redirect_handler };
    httpd_register_uri_handler(s_portal_server, &catchall_uri);

    ESP_LOGI(TAG, "Setup portal ready at http://" AP_IP_ADDR "/ -- connect to \"PROV_%s\" and open a browser",
             CONFIG_UNITCAMS3_DEVICE_ID);

    xEventGroupWaitBits(s_result_group, RESULT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi provisioned -- tearing down setup portal");

    s_dns_running = false;
    // dns_hijack_task self-deletes within ~1s of its recv timeout once s_dns_running is false.
    vTaskDelay(pdMS_TO_TICKS(1200));

    httpd_stop(s_portal_server);
    s_portal_server = NULL;

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_inst);
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_handler_inst);
    vEventGroupDelete(s_result_group);
    s_result_group = NULL;

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);

    return ESP_OK;
}
