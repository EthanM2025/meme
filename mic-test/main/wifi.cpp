// WiFi: NVS-backed STA with SoftAP+HTTP fallback for provisioning.
// No companion app — user opens 192.168.4.1 in any browser.

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

#define NVS_NS       "wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

#define STA_CONNECTED_BIT BIT0
#define STA_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static bool s_connected = false;
static int  s_retry = 0;

// --- index.html served by SoftAP. Inline string for simplicity. ---
static const char PROV_PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Meme Setup</title>"
"<style>body{font-family:-apple-system,sans-serif;max-width:420px;margin:40px auto;padding:0 16px;color:#222}"
"h1{font-size:20px}label{display:block;margin:14px 0 6px;font-size:13px;color:#666}"
"input,select{width:100%;padding:10px;font-size:16px;box-sizing:border-box;border:1px solid #ccc;border-radius:6px}"
"button{margin-top:18px;width:100%;padding:12px;font-size:16px;background:#0a84ff;color:#fff;border:0;border-radius:6px}"
"#msg{margin-top:14px;font-size:13px;color:#555}.ok{color:#1a7f37}.err{color:#cf222e}</style></head>"
"<body><h1>Meme · WiFi 配网</h1>"
"<form id='f'>"
"<label>选择 WiFi（或手动输入）</label>"
"<select id='ssid_sel' onchange=\"document.getElementById('ssid').value=this.value\"></select>"
"<label>SSID</label><input id='ssid' name='ssid' autocomplete='off' required>"
"<label>密码</label><input id='pass' name='pass' type='password' autocomplete='off'>"
"<button type='submit'>保存并连接</button></form>"
"<div id='msg'></div>"
"<script>"
"fetch('/scan').then(r=>r.json()).then(j=>{var s=document.getElementById('ssid_sel');"
"s.innerHTML='<option value=\"\">— 扫描结果 —</option>'+"
"j.map(n=>'<option value=\"'+n+'\">'+n+'</option>').join('');});"
"document.getElementById('f').onsubmit=async e=>{e.preventDefault();var m=document.getElementById('msg');"
"m.textContent='保存中…';m.className='';"
"var b=new URLSearchParams(new FormData(e.target));"
"try{var r=await fetch('/save',{method:'POST',body:b,headers:{'Content-Type':'application/x-www-form-urlencoded'}});"
"if(r.ok){m.textContent='已保存，设备将重启并连接 WiFi。';m.className='ok';}"
"else{m.textContent='保存失败：'+await r.text();m.className='err';}}"
"catch(err){m.textContent='错误：'+err;m.className='err';}};"
"</script></body></html>";

// ============================================================
// NVS helpers
// ============================================================
static bool nvs_load_creds(char* ssid, size_t ssid_size, char* pass, size_t pass_size) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t s = ssid_size, p = pass_size;
    esp_err_t r1 = nvs_get_str(h, NVS_KEY_SSID, ssid, &s);
    esp_err_t r2 = nvs_get_str(h, NVS_KEY_PASS, pass, &p);
    nvs_close(h);
    return r1 == ESP_OK && r2 == ESP_OK && s > 1;
}

static bool nvs_save_creds(const char* ssid, const char* pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
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
            if (s_retry++ < 5) {
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

// ============================================================
// STA: try NVS creds
// ============================================================
bool wifi_try_connect_from_nvs(int timeout_ms) {
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "no NVS creds");
        return false;
    }
    ESP_LOGI(TAG, "trying NVS ssid='%s'", ssid);

    esp_netif_create_default_wifi_sta();
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept open + WPA2
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    xEventGroupClearBits(s_wifi_events, STA_CONNECTED_BIT | STA_FAIL_BIT);
    s_retry = 0;
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        STA_CONNECTED_BIT | STA_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & STA_CONNECTED_BIT) return true;

    ESP_LOGW(TAG, "NVS creds failed/timeout");
    esp_wifi_stop();
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
    // Re-scan to give the user a fresh list.
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

    char ssid[33] = {0}, pass[65] = {0};
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
        if (strcmp(key, "ssid") == 0) strncpy(ssid, val, sizeof(ssid) - 1);
        else if (strcmp(key, "pass") == 0) strncpy(pass, val, sizeof(pass) - 1);
        if (!amp) break;
        p = amp + 1;
    }

    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }
    ESP_LOGI(TAG, "saving ssid='%s' pass_len=%d", ssid, (int)strlen(pass));
    nvs_save_creds(ssid, pass);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OK");

    xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ============================================================
// SoftAP provisioning
// ============================================================
void wifi_start_provisioning(void) {
    // Build SSID using last 4 hex chars of MAC.
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Meme-%02X%02X", mac[4], mac[5]);

    // Idempotent netif creation — a prior wifi_try_connect_from_nvs() may have
    // already created the STA netif. Calling esp_netif_create_default_wifi_sta()
    // a second time asserts on duplicate if_key. Same guard for AP in case we
    // re-enter provisioning later.
    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == NULL) {
        esp_netif_create_default_wifi_ap();
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);   // AP for serving page, STA for scanning real networks

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
    httpd_uri_t u_save = {.uri = "/save", .method = HTTP_POST, .handler = h_save, .user_ctx = NULL};
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_save);

    // Return — caller's task can idle / blink LED / etc. Restart happens via /save handler.
}
