// WiFi: NVS-backed STA with multi-profile storage + SoftAP fallback.
// Up to MAX_PROFILES saved networks; boot tries each in turn before
// falling into provisioning.

#include "wifi.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "wifi";

#define NVS_NS         "wifi"
#define NVS_KEY_COUNT  "n"          // u8: number of saved profiles
// per-profile keys: "s<idx>" (str) and "p<idx>" (str)
#define MAX_PROFILES   5
#define NVS_KEY_HOST   "host"       // str: Mac mDNS hostname (e.g. "Ethan-MacBook-Air.local")

#define STA_CONNECTED_BIT BIT0
#define STA_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static bool s_connected = false;
static int  s_retry = 0;
static int  s_max_retry = 5;         // tweaked during profile iteration

// ============================================================
// SoftAP HTML — minimal, JS-free form (browser handles POST natively).
// A small async fetch fills the "Saved networks" list, but if that fails
// the form itself still works.
// ============================================================
static const char PROV_PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Meme WiFi</title>"
"<style>"
"body{font-family:-apple-system,sans-serif;max-width:420px;margin:30px auto;padding:0 16px;color:#222}"
"h1{font-size:20px}h2{font-size:15px;color:#444;margin-top:24px}"
"label{display:block;margin:12px 0 4px;font-size:13px;color:#666}"
"input{width:100%;padding:10px;font-size:16px;box-sizing:border-box;border:1px solid #ccc;border-radius:6px}"
"button{margin-top:18px;width:100%;padding:12px;font-size:16px;background:#0a84ff;color:#fff;border:0;border-radius:6px}"
".row{margin:4px 0;padding:8px 10px;background:#f4f4f6;border-radius:6px;font-size:14px}"
"</style></head><body>"
"<h1>Meme WiFi</h1>"
"<h2>Add a network</h2>"
"<form method='POST' action='/save'>"
"<label>SSID</label><input name='ssid' autocomplete='off' required>"
"<label>Password</label><input name='pass' type='password' autocomplete='off'>"
"<label>Mac hostname (e.g. Ethan-MacBook-Air.local)</label>"
"<input name='host' autocomplete='off' placeholder='leave blank to keep current'>"
"<button type='submit'>Save and connect</button>"
"</form>"
"<h2>Saved networks</h2><div id='saved'><div class='row' style='color:#888'>loading...</div></div>"
"<script>"
"fetch('/list').then(function(r){return r.json()}).then(function(j){"
"var d=document.getElementById('saved');"
"if(!j||!j.length){d.innerHTML='<div class=\"row\" style=\"color:#888\">(none yet)</div>';return}"
"d.innerHTML=j.map(function(s){return '<div class=\"row\">'+s+'</div>'}).join('');"
"}).catch(function(){"
"document.getElementById('saved').innerHTML='<div class=\"row\" style=\"color:#c33\">(failed to load)</div>';"
"});"
"</script></body></html>";

// ============================================================
// NVS helpers — multi-profile schema
// ============================================================

// v1 → v2 migration: copy legacy "ssid"/"pass" keys into slot 0.
static void nvs_migrate_v1(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t existing_n = 0;
    if (nvs_get_u8(h, NVS_KEY_COUNT, &existing_n) == ESP_OK) {
        // Already on v2 schema.
        nvs_close(h);
        return;
    }
    char ssid[33] = {0}, pass[65] = {0};
    size_t s = sizeof(ssid), p = sizeof(pass);
    if (nvs_get_str(h, "ssid", ssid, &s) == ESP_OK &&
        nvs_get_str(h, "pass", pass, &p) == ESP_OK && s > 1) {
        nvs_set_str(h, "s0", ssid);
        nvs_set_str(h, "p0", pass);
        nvs_set_u8(h, NVS_KEY_COUNT, 1);
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        ESP_LOGI(TAG, "migrated legacy creds → slot 0 (ssid='%s')", ssid);
    } else {
        // No legacy creds either — first boot, leave count unset.
    }
    nvs_close(h);
}

static int nvs_profile_count(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, NVS_KEY_COUNT, &n);
    nvs_close(h);
    return n > MAX_PROFILES ? MAX_PROFILES : n;
}

static bool nvs_load_profile(int slot, char* ssid, size_t ssid_size,
                             char* pass, size_t pass_size) {
    if (slot < 0 || slot >= MAX_PROFILES) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key_s[8], key_p[8];
    snprintf(key_s, sizeof(key_s), "s%d", slot);
    snprintf(key_p, sizeof(key_p), "p%d", slot);
    size_t s = ssid_size, p = pass_size;
    esp_err_t r1 = nvs_get_str(h, key_s, ssid, &s);
    esp_err_t r2 = nvs_get_str(h, key_p, pass, &p);
    nvs_close(h);
    return r1 == ESP_OK && r2 == ESP_OK;
}

// Add a profile. If SSID already exists in the list, update its password
// in place. If the list is full, overwrite the oldest slot (slot 0).
static bool nvs_add_profile(const char* ssid, const char* pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    uint8_t n = 0;
    nvs_get_u8(h, NVS_KEY_COUNT, &n);
    if (n > MAX_PROFILES) n = MAX_PROFILES;

    int slot = -1;
    for (int i = 0; i < n; ++i) {
        char key_s[8]; snprintf(key_s, sizeof(key_s), "s%d", i);
        char existing[33] = {0};
        size_t sz = sizeof(existing);
        if (nvs_get_str(h, key_s, existing, &sz) == ESP_OK &&
            strcmp(existing, ssid) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        if (n < MAX_PROFILES) slot = n++;
        else                  slot = 0;  // list full → kick the oldest
    }

    char key_s[8], key_p[8];
    snprintf(key_s, sizeof(key_s), "s%d", slot);
    snprintf(key_p, sizeof(key_p), "p%d", slot);
    nvs_set_str(h, key_s, ssid);
    nvs_set_str(h, key_p, pass);
    nvs_set_u8(h, NVS_KEY_COUNT, n);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

// Delete the profile matching `ssid`. Compacts the list so slot indexes stay
// contiguous (no gaps), so the connect loop can iterate 0..count-1 directly.
static bool nvs_delete_profile(const char* ssid) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t n = 0;
    nvs_get_u8(h, NVS_KEY_COUNT, &n);
    if (n > MAX_PROFILES) n = MAX_PROFILES;

    int found = -1;
    for (int i = 0; i < n; ++i) {
        char key_s[8]; snprintf(key_s, sizeof(key_s), "s%d", i);
        char existing[33] = {0};
        size_t sz = sizeof(existing);
        if (nvs_get_str(h, key_s, existing, &sz) == ESP_OK &&
            strcmp(existing, ssid) == 0) {
            found = i; break;
        }
    }
    if (found < 0) { nvs_close(h); return false; }

    // Shift slots above `found` down by one.
    for (int i = found; i < n - 1; ++i) {
        char src_s[8], src_p[8], dst_s[8], dst_p[8];
        snprintf(src_s, sizeof(src_s), "s%d", i + 1);
        snprintf(src_p, sizeof(src_p), "p%d", i + 1);
        snprintf(dst_s, sizeof(dst_s), "s%d", i);
        snprintf(dst_p, sizeof(dst_p), "p%d", i);
        char tmp_s[33] = {0}, tmp_p[65] = {0};
        size_t ss = sizeof(tmp_s), ps = sizeof(tmp_p);
        if (nvs_get_str(h, src_s, tmp_s, &ss) == ESP_OK &&
            nvs_get_str(h, src_p, tmp_p, &ps) == ESP_OK) {
            nvs_set_str(h, dst_s, tmp_s);
            nvs_set_str(h, dst_p, tmp_p);
        }
    }
    // Wipe the now-unused tail slot.
    char tail_s[8], tail_p[8];
    snprintf(tail_s, sizeof(tail_s), "s%d", n - 1);
    snprintf(tail_p, sizeof(tail_p), "p%d", n - 1);
    nvs_erase_key(h, tail_s);
    nvs_erase_key(h, tail_p);
    nvs_set_u8(h, NVS_KEY_COUNT, n - 1);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

// ============================================================
// WiFi event handler
// ============================================================
static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (s_retry++ < s_max_retry) {
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_events, STA_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "GOT IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        xEventGroupSetBits(s_wifi_events, STA_CONNECTED_BIT);
    }
}

// ============================================================
// Init (once at boot)
// ============================================================
void wifi_init(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL, NULL);
    s_wifi_events = xEventGroupCreate();
}

bool wifi_is_connected(void) { return s_connected; }

bool wifi_get_mac_host(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = buf_size;
    esp_err_t r = nvs_get_str(h, NVS_KEY_HOST, buf, &sz);
    nvs_close(h);
    return r == ESP_OK && sz > 1;
}

static bool nvs_save_mac_host(const char* host) {
    if (!host || !host[0]) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, NVS_KEY_HOST, host);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

// ============================================================
// STA: iterate saved profiles, return on first success
// ============================================================
bool wifi_try_connect_from_nvs(int timeout_ms_per_profile) {
    nvs_migrate_v1();
    int n = nvs_profile_count();
    if (n == 0) {
        ESP_LOGW(TAG, "no saved WiFi profiles");
        return false;
    }
    ESP_LOGI(TAG, "trying %d saved profile(s)", n);

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    esp_wifi_set_mode(WIFI_MODE_STA);

    // Give each profile a couple of internal retries so a transient disconnect
    // during initial auth (common with busy APs) doesn't abort the attempt.
    s_max_retry = 2;
    bool started = false;

    for (int i = 0; i < n; ++i) {
        char ssid[33] = {0}, pass[65] = {0};
        if (!nvs_load_profile(i, ssid, sizeof(ssid), pass, sizeof(pass))) continue;
        ESP_LOGI(TAG, "  [%d] '%s'", i, ssid);

        wifi_config_t wc = {};
        strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);

        xEventGroupClearBits(s_wifi_events, STA_CONNECTED_BIT | STA_FAIL_BIT);
        s_retry = 0;

        if (!started) {
            esp_wifi_start();
            started = true;
        } else {
            esp_wifi_disconnect();
            esp_wifi_connect();
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
            STA_CONNECTED_BIT | STA_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(timeout_ms_per_profile));
        if (bits & STA_CONNECTED_BIT) {
            ESP_LOGI(TAG, "connected to '%s'", ssid);
            s_max_retry = 5;  // restore retries for steady-state disconnects
            return true;
        }
        ESP_LOGW(TAG, "  [%d] '%s' failed/timeout", i, ssid);
    }

    s_max_retry = 5;
    if (started) esp_wifi_stop();
    return false;
}

// ============================================================
// HTTP handlers
// ============================================================
static esp_err_t h_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PROV_PAGE, sizeof(PROV_PAGE) - 1);
}

static esp_err_t h_scan(httpd_req_t* req) {
    wifi_scan_config_t sc = {};
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t* recs = (wifi_ap_record_t*)calloc(n, sizeof(*recs));
    esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < n; ++i) {
        if (recs[i].ssid[0] == 0) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\"%.32s\"", i ? "," : "", recs[i].ssid);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    free(recs);
    return ESP_OK;
}

static esp_err_t h_list(httpd_req_t* req) {
    int count = nvs_profile_count();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; ++i) {
        char ssid[33] = {0}, pass[65] = {0};
        if (!nvs_load_profile(i, ssid, sizeof(ssid), pass, sizeof(pass))) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\"%.32s\"", i ? "," : "", ssid);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void url_decode(char* s) {
    char* o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; ++s; }
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else { *o++ = *s++; }
    }
    *o = 0;
}

static void delayed_restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_save(httpd_req_t* req) {
    char body[256] = {0};
    int total = req->content_len;
    if (total >= (int)sizeof(body)) total = sizeof(body) - 1;
    int r = httpd_req_recv(req, body, total);
    if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "empty body");
    }
    body[r] = 0;

    char ssid[33] = {0}, pass[65] = {0}, host[64] = {0};
    char* p = body;
    while (*p) {
        char* eq = strchr(p, '=');
        if (!eq) break;
        char* amp = strchr(eq, '&');
        size_t klen = eq - p;
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
        char key[16] = {0}, val[80] = {0};
        if (klen < sizeof(key)) { memcpy(key, p, klen); key[klen] = 0; }
        if (vlen < sizeof(val)) { memcpy(val, eq + 1, vlen); val[vlen] = 0; }
        url_decode(val);
        if (strcmp(key, "ssid") == 0)      strncpy(ssid, val, sizeof(ssid) - 1);
        else if (strcmp(key, "pass") == 0) strncpy(pass, val, sizeof(pass) - 1);
        else if (strcmp(key, "host") == 0) strncpy(host, val, sizeof(host) - 1);
        if (!amp) break;
        p = amp + 1;
    }

    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }
    ESP_LOGI(TAG, "saving ssid='%s' pass_len=%d host='%s'", ssid, (int)strlen(pass), host);
    nvs_add_profile(ssid, pass);
    if (host[0]) nvs_save_mac_host(host);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_del(httpd_req_t* req) {
    char query[80] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char ssid[33] = {0};
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK || !ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }
    url_decode(ssid);
    ESP_LOGI(TAG, "deleting ssid='%s'", ssid);
    nvs_delete_profile(ssid);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ============================================================
// SoftAP provisioning
// ============================================================
void wifi_start_provisioning(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Meme-%02X%02X", mac[4], mac[5]);

    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == NULL) {
        esp_netif_create_default_wifi_ap();
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t apc = {};
    strncpy((char*)apc.ap.ssid, ap_ssid, sizeof(apc.ap.ssid));
    apc.ap.ssid_len      = strlen(ap_ssid);
    apc.ap.channel       = 1;
    apc.ap.max_connection = 4;
    apc.ap.authmode      = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &apc);
    esp_wifi_start();

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, "  SoftAP started: %s (open)", ap_ssid);
    ESP_LOGI(TAG, "  Connect from your phone, then open:");
    ESP_LOGI(TAG, "    http://192.168.4.1/");
    ESP_LOGI(TAG, "==============================================");

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t srv = NULL;
    httpd_start(&srv, &hcfg);
    httpd_uri_t u_root = {.uri = "/",     .method = HTTP_GET,  .handler = h_root, .user_ctx = NULL};
    httpd_uri_t u_scan = {.uri = "/scan", .method = HTTP_GET,  .handler = h_scan, .user_ctx = NULL};
    httpd_uri_t u_list = {.uri = "/list", .method = HTTP_GET,  .handler = h_list, .user_ctx = NULL};
    httpd_uri_t u_save = {.uri = "/save", .method = HTTP_POST, .handler = h_save, .user_ctx = NULL};
    httpd_uri_t u_del  = {.uri = "/del",  .method = HTTP_POST, .handler = h_del,  .user_ctx = NULL};
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_list);
    httpd_register_uri_handler(srv, &u_save);
    httpd_register_uri_handler(srv, &u_del);
}
