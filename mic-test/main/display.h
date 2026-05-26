#pragma once
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up CO5300 OLED via M5GFX and initializes LVGL with a flush callback.
void display_init(void);

// Wires CST820 touch into LVGL as a pointer indev + gesture handler.
// Call after display_init() and after the i2c bus has at least one device on it.
void touch_init(i2c_bus_handle_t bus);

// Acquire/release the LVGL mutex. Always lock before touching LVGL widgets,
// because the rendering task does the same.
bool lvgl_lock(void);
void lvgl_unlock(void);

// Thread-safe label setters (all handle locking internally).

// Switch to a dedicated full-screen WiFi-setup page (replaces the Claude page
// while the device is in SoftAP provisioning mode).
void ui_show_wifi_setup(const char* hotspot_ssid, const char* url);
void ui_set_recording(bool on);
void ui_set_sleeping(bool sleeping);
bool ui_consume_touch_activity(void);
void ui_set_model(const char* model);

// Claude page metrics (tokens out, estimated cost USD, busy/idle state).
void ui_set_claude_metrics(int tokens_out, float cost_usd, bool busy);
void ui_set_codex_model(const char* model);
void ui_set_codex_effort(const char* effort);  // "high" / "medium" / "low" or ""
void ui_set_todos_progress(int done, int total, const char* current, const char* recent_done);
void ui_set_codex_limits(int five_hour_left_pct, const char* five_hour_reset,
                         int week_left_pct, const char* week_reset);
void ui_set_transcript(const char* text);

// Battery indicator on both screens. Charging shows a lightning-bolt prefix.
void ui_set_battery(int percent, bool charging);

// Claude page TODO dot matrix. Status string: 'c' completed, 'i' in_progress,
// 'p' pending. Up to 24 dots shown; extras are dropped.
void ui_set_todos_statuses(const char* statuses);

// Codex page Codex.app focus activity. today_min = minutes today, today_pct =
// % of daily budget, week_min/_pct similar for the week.
void ui_set_codex_activity(int today_min, int today_pct, int week_min, int week_pct);

#ifdef __cplusplus
}
#endif
