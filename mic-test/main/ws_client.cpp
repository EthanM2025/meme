#include "ws_client.h"
#include "display.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "ws";
static esp_websocket_client_handle_t s_client = nullptr;
static bool s_connected = false;

// Decouple PCM producer (mic_task) from the actual WS send: producer drops
// each batch into a queue; a dedicated send task drains it. Without this, a
// slow TCP write blocks mic_task for ≥100 ms and the esp_websocket_client's
// internal state machine eventually tears down the connection mid-recording.
struct PcmMsg {
    uint8_t* data;
    int      len;
};
static QueueHandle_t s_pcm_queue = nullptr;
static uint32_t      s_dropped_batches = 0;

static void ws_send_task(void* /*arg*/) {
    PcmMsg msg;
    while (true) {
        if (xQueueReceive(s_pcm_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (s_connected && s_client && msg.data && msg.len > 0) {
            esp_websocket_client_send_bin(s_client, (const char*)msg.data,
                                          msg.len, pdMS_TO_TICKS(500));
        }
        free(msg.data);
    }
}

static void handle_state_json(const char* payload, int len) {
    cJSON* root = cJSON_ParseWithLength(payload, len);
    if (!root) return;

    cJSON* model = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(model)) ui_set_model(model->valuestring);

    cJSON* codex_model = cJSON_GetObjectItem(root, "codex_model");
    if (cJSON_IsString(codex_model)) ui_set_codex_model(codex_model->valuestring);

    cJSON* codex_effort = cJSON_GetObjectItem(root, "codex_effort");
    if (cJSON_IsString(codex_effort)) ui_set_codex_effort(codex_effort->valuestring);

    cJSON* effort_pct = cJSON_GetObjectItem(root, "codex_effort_pct");
    if (cJSON_IsNumber(effort_pct)) ui_set_codex_effort_arc((int)effort_pct->valuedouble);

    cJSON* c5h    = cJSON_GetObjectItem(root, "claude_5h_used_pct");
    cJSON* c5h_ra = cJSON_GetObjectItem(root, "claude_5h_reset_abs");
    cJSON* c7d    = cJSON_GetObjectItem(root, "claude_7d_used_pct");
    cJSON* c7d_ra = cJSON_GetObjectItem(root, "claude_7d_reset_abs");
    {
        int  v5 = cJSON_IsNumber(c5h) ? (int)c5h->valuedouble : -1;
        int  v7 = cJSON_IsNumber(c7d) ? (int)c7d->valuedouble : -1;
        const char* r5 = cJSON_IsString(c5h_ra) ? c5h_ra->valuestring : "";
        const char* r7 = cJSON_IsString(c7d_ra) ? c7d_ra->valuestring : "";
        ui_set_claude_quota(v5, r5, v7, r7);
    }

    cJSON* today_cost = cJSON_GetObjectItem(root, "claude_today_cost_usd");
    cJSON* today_tok  = cJSON_GetObjectItem(root, "claude_today_tokens");
    cJSON* hhmm       = cJSON_GetObjectItem(root, "mac_hhmm");
    if (cJSON_IsNumber(today_cost) || cJSON_IsNumber(today_tok) || cJSON_IsString(hhmm)) {
        float cost = cJSON_IsNumber(today_cost) ? (float)today_cost->valuedouble : 0.0f;
        int   tok  = cJSON_IsNumber(today_tok)  ? (int)today_tok->valuedouble    : 0;
        const char* hm = cJSON_IsString(hhmm) ? hhmm->valuestring : "";
        ui_set_claude_summary(cost, tok, hm);
    }

    // Claude metrics: tokens_out + cost_usd + busy → combined setter.
    cJSON* out  = cJSON_GetObjectItem(root, "tokens_out");
    cJSON* cost = cJSON_GetObjectItem(root, "cost_usd");
    cJSON* busy = cJSON_GetObjectItem(root, "busy");
    if (cJSON_IsNumber(out) || cJSON_IsNumber(cost) || cJSON_IsBool(busy)) {
        int   tokens   = cJSON_IsNumber(out)  ? (int)out->valuedouble : 0;
        float cost_v   = cJSON_IsNumber(cost) ? (float)cost->valuedouble : 0.0f;
        bool  busy_v   = cJSON_IsTrue(busy);
        ui_set_claude_metrics(tokens, cost_v, busy_v);
    }

    cJSON* todos_done  = cJSON_GetObjectItem(root, "todos_done");
    cJSON* todos_total = cJSON_GetObjectItem(root, "todos_total");
    cJSON* todos_cur   = cJSON_GetObjectItem(root, "todos_current");
    cJSON* todos_done_text = cJSON_GetObjectItem(root, "todos_done_text");
    cJSON* todos_statuses  = cJSON_GetObjectItem(root, "todos_statuses");
    if (cJSON_IsNumber(todos_done) && cJSON_IsNumber(todos_total)) {
        const char* cur  = cJSON_IsString(todos_cur)        ? todos_cur->valuestring        : "";
        const char* done = cJSON_IsString(todos_done_text)  ? todos_done_text->valuestring  : "";
        ui_set_todos_progress((int)todos_done->valuedouble, (int)todos_total->valuedouble, cur, done);
        if (cJSON_IsString(todos_statuses)) {
            ui_set_todos_statuses(todos_statuses->valuestring);
        }
    }

    cJSON* act_today_min = cJSON_GetObjectItem(root, "codex_today_min");
    cJSON* act_today_pct = cJSON_GetObjectItem(root, "codex_today_pct");
    cJSON* act_week_min  = cJSON_GetObjectItem(root, "codex_week_min");
    cJSON* act_week_pct  = cJSON_GetObjectItem(root, "codex_week_pct");
    if (cJSON_IsNumber(act_today_min) || cJSON_IsNumber(act_week_min)) {
        ui_set_codex_activity(
            cJSON_IsNumber(act_today_min) ? (int)act_today_min->valuedouble : 0,
            cJSON_IsNumber(act_today_pct) ? (int)act_today_pct->valuedouble : 0,
            cJSON_IsNumber(act_week_min)  ? (int)act_week_min->valuedouble  : 0,
            cJSON_IsNumber(act_week_pct)  ? (int)act_week_pct->valuedouble  : 0);
    }

    cJSON* codex_5h_left   = cJSON_GetObjectItem(root, "codex_5h_left_pct");
    cJSON* codex_5h_abs    = cJSON_GetObjectItem(root, "codex_5h_reset_abs");
    cJSON* codex_week_left = cJSON_GetObjectItem(root, "codex_week_left_pct");
    cJSON* codex_week_abs  = cJSON_GetObjectItem(root, "codex_week_reset_abs");
    {
        int v5 = cJSON_IsNumber(codex_5h_left)   ? (int)codex_5h_left->valuedouble   : -1;
        int vw = cJSON_IsNumber(codex_week_left) ? (int)codex_week_left->valuedouble : -1;
        const char* r5 = cJSON_IsString(codex_5h_abs)   ? codex_5h_abs->valuestring   : "";
        const char* rw = cJSON_IsString(codex_week_abs) ? codex_week_abs->valuestring : "";
        ui_set_codex_limits(v5, r5, vw, rw);
    }

    cJSON* transcript = cJSON_GetObjectItem(root, "transcript");
    if (cJSON_IsString(transcript)) ui_set_transcript(transcript->valuestring);

    cJSON_Delete(root);
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            s_connected = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "error");
            break;
        case WEBSOCKET_EVENT_DATA: {
            auto* d = (esp_websocket_event_data_t*)data;
            // op_code: 1=text, 2=binary, 0x8=close, 0x9=ping, 0xA=pong
            if (d->op_code == 0x01 && d->payload_len > 0 && d->payload_offset == 0) {
                handle_state_json((const char*)d->data_ptr, d->data_len);
            }
            break;
        }
        default:
            break;
    }
}

void ws_start(const char* uri) {
    esp_websocket_client_config_t cfg = {};
    cfg.uri                  = uri;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms   = 10000;
    cfg.buffer_size          = 16384;  // was 4096; PCM batches + state JSON were tight
    cfg.ping_interval_sec    = 10;
    cfg.pingpong_timeout_sec = 20;

    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, nullptr);
    esp_websocket_client_start(s_client);

    // Queue depth 16 × ~1600B/batch ≈ 25 KB of PCM in flight at worst (~0.8 s
    // of audio). Plenty for normal jitter; if WiFi truly stalls we drop new
    // batches rather than block the producer.
    s_pcm_queue = xQueueCreate(16, sizeof(PcmMsg));
    xTaskCreate(ws_send_task, "ws_send", 4096, nullptr, 6, nullptr);

    ESP_LOGI(TAG, "connecting to %s", uri);
}

void ws_send_pcm(const int16_t* samples, int n) {
    if (!s_connected || !s_client || !s_pcm_queue) return;
    int bytes = n * 2;
    uint8_t* copy = (uint8_t*)malloc(bytes);
    if (!copy) return;
    memcpy(copy, samples, bytes);
    PcmMsg msg = { copy, bytes };
    // Non-blocking enqueue. If the queue is full, the WS task is behind —
    // drop the batch rather than stall mic_task (which would back up I2S DMA).
    if (xQueueSend(s_pcm_queue, &msg, 0) != pdTRUE) {
        free(copy);
        if ((++s_dropped_batches & 0x1F) == 0) {
            ESP_LOGW(TAG, "pcm queue full — dropped %lu batches total",
                     (unsigned long)s_dropped_batches);
        }
    }
}

void ws_send_text(const char* text) {
    if (!s_connected || !s_client || !text) return;
    esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(100));
}

bool ws_is_connected(void) { return s_connected; }
