// Display + LVGL boot for M5StopWatch.
// Architecture (lifted from M5StopWatch-UserDemo/main/hal/hal_display.cpp):
//   - Panel_CO5300 (QSPI, 468x466 visible area on a 480x480 buffer, 6px x-offset)
//   - M5GFX wraps the bus + panel into one drawable handle
//   - LVGL gets a partial render buffer in PSRAM; flush_cb pushes pixels via M5GFX
//   - CST820 touch is wired through `touch_read_cb` into an LVGL pointer indev,
//     with gesture events on each screen mapped to swipe-page transitions.

#include "display.h"

#include <cstring>
#include <memory>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <lvgl.h>
#include "cst820.h"
#include "codex_pet_assets.h"
#include "claude_pet_assets.h"
#include "i2c_bus.h"

// CJK font removed — Chinese rendering wasn't worth the ~1.7MB it took in
// the app partition. Transcript label now uses Montserrat; Chinese ASR output
// renders as placeholders, but the server-side stream gives us the text.

static const char* TAG = "display";

// --- pins (from UserDemo hal_display.cpp) ---
static constexpr gpio_num_t cfg_pin_sclk = GPIO_NUM_40;
static constexpr gpio_num_t cfg_pin_io0  = GPIO_NUM_41;
static constexpr gpio_num_t cfg_pin_io1  = GPIO_NUM_42;
static constexpr gpio_num_t cfg_pin_io2  = GPIO_NUM_46;
static constexpr gpio_num_t cfg_pin_io3  = GPIO_NUM_45;
static constexpr gpio_num_t cfg_pin_cs   = GPIO_NUM_39;
static constexpr gpio_num_t cfg_pin_te   = GPIO_NUM_38;
static constexpr gpio_num_t cfg_pin_rst  = GPIO_NUM_NC;

// --- CO5300 panel definition ---
class Panel_CO5300 : public lgfx::Panel_AMOLED {
public:
    Panel_CO5300() {
        _cfg.memory_width  = _cfg.panel_width  = 480;
        _cfg.memory_height = _cfg.panel_height = 480;
        _write_depth = lgfx::color_depth_t::rgb565_2Byte;
        _read_depth  = lgfx::color_depth_t::rgb565_2Byte;
    }
    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0x11, 0 + CMD_INIT_DELAY, 150,    // Sleep out + delay
            0xC4, 1, 0x80,
            0x35, 1, 0x80,
            0x44, 2, 0x01, 0xD2,              // Tear effect line 0x1D2 = 466
            0x53, 1, 0x20,
            0x20, 0, 0x36,
            1, 0, 0x51,
            1, 0xA0, 0x29,
            0, 0xff, 0xff
        };
        return (listno == 0) ? list0 : nullptr;
    }
};

class StopWatchGfx : public M5GFX {
    lgfx::Bus_SPI _bus;
    Panel_CO5300  _panel;
public:
    bool init_impl(bool use_reset, bool use_clear) override {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 80000000;
            cfg.freq_read  = 10000000;
            cfg.pin_sclk = cfg_pin_sclk;
            cfg.pin_io0  = cfg_pin_io0;
            cfg.pin_io1  = cfg_pin_io1;
            cfg.pin_io2  = cfg_pin_io2;
            cfg.pin_io3  = cfg_pin_io3;
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.spi_3wire   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_rst      = cfg_pin_rst;
            cfg.pin_cs       = cfg_pin_cs;
            cfg.panel_width  = 468;
            cfg.panel_height = 466;
            cfg.offset_x     = 6;
            cfg.offset_y     = 0;
            cfg.readable     = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
        lgfx::pinMode(cfg_pin_te, lgfx::pin_mode_t::input_pullup);

        if (!LGFX_Device::init_impl(use_reset, use_clear)) return false;

        // Framebuffer in PSRAM so partial flushes are cheap.
        if (_panel.initPanelFb()) {
            auto* fb = _panel.getPanelFb();
            if (fb) {
                fb->setBus(&_bus);
                fb->setAutoDisplay(true);
                setPanel(fb);
            }
        }
        _panel.setBrightness(180);
        return true;
    }
    void setBrightness(uint8_t b) { _panel.setBrightness(b); }
};

static std::unique_ptr<StopWatchGfx> s_gfx;

// --- LVGL plumbing ---
static SemaphoreHandle_t s_gui_sema = nullptr;
static lv_display_t* s_lv_disp = nullptr;
static std::unique_ptr<Cst820> s_touch;
static volatile bool s_touch_activity = false;

// Two screens: Claude (default) and Codex. Swipe left → Codex, right → Claude.
static lv_obj_t* s_scr_claude = nullptr;
static lv_obj_t* s_scr_codex  = nullptr;
// Setup screen — only shown while SoftAP provisioning is active.
static lv_obj_t* s_scr_setup  = nullptr;
static lv_obj_t* s_setup_ssid_label = nullptr;
static lv_obj_t* s_setup_url_label  = nullptr;

// Claude-page widgets — mirrors Codex page layout (two-arc header + metric
// rows + bottom todo block).
static lv_obj_t* s_rec_dot = nullptr;
static lv_obj_t* s_model_value = nullptr;        // left arc: model name (e.g. "opus-4.7")
static lv_obj_t* s_mode_value = nullptr;         // right arc: "BUSY" / "IDLE"
static lv_obj_t* s_out_value = nullptr;          // OUT row text "1.8K"
static lv_obj_t* s_out_pct_label = nullptr;
static lv_obj_t* s_out_bar = nullptr;
static lv_obj_t* s_cost_value = nullptr;         // COST row text "$1.23"
static lv_obj_t* s_cost_pct_label = nullptr;
static lv_obj_t* s_cost_bar = nullptr;
static lv_obj_t* s_todos_label = nullptr;        // bottom: "X / Y" centered
static lv_obj_t* s_transcript_label = nullptr;
// Battery indicator — one label per screen, both updated from one setter.
static lv_obj_t* s_battery_claude = nullptr;
static lv_obj_t* s_battery_codex  = nullptr;

// Claude page todo dot matrix — pre-allocated, hidden when not in use.
static constexpr int TODO_DOTS_MAX = 24;
static lv_obj_t* s_todo_dots[TODO_DOTS_MAX] = {nullptr};

// Codex page activity rows (Codex.app focus time today/week).
static lv_obj_t* s_codex_today_value = nullptr;
static lv_obj_t* s_codex_today_bar   = nullptr;
static lv_obj_t* s_codex_today_pct   = nullptr;
static lv_obj_t* s_codex_actweek_value = nullptr;
static lv_obj_t* s_codex_actweek_bar   = nullptr;
static lv_obj_t* s_codex_actweek_pct   = nullptr;

// Codex-page widgets. Naming convention: prefix matches the row label it lives
// next to ("5h" row, "week" row, "resets" bottom line). Previous names had
// `_today_*` and `_token_*` aliasing onto WEEK / 5H rows respectively — fixed.
static lv_obj_t* s_codex_rec_dot = nullptr;
static lv_obj_t* s_codex_model_value = nullptr;
static lv_obj_t* s_codex_effort_value = nullptr;   // "HIGH"/"MEDIUM"/"LOW" in right arc
static lv_obj_t* s_codex_5h_value = nullptr;       // "X% used" text, 5H row
static lv_obj_t* s_codex_5h_pct_label = nullptr;  // right-side "X%", 5H row
static lv_obj_t* s_codex_5h_bar = nullptr;
static lv_obj_t* s_codex_week_value = nullptr;     // "X% used" text, WEEK row
static lv_obj_t* s_codex_week_pct_label = nullptr; // right-side "X%", WEEK row
static lv_obj_t* s_codex_week_bar = nullptr;
static lv_obj_t* s_codex_pet_img = nullptr;
static lv_timer_t* s_codex_pet_timer = nullptr;
static uint8_t s_codex_pet_frame = 0;
static const lv_image_dsc_t* const* s_codex_pet_frames = codex_pet_idle_frames;
static const uint32_t* s_codex_pet_durations = codex_pet_idle_durations_ms;
static uint8_t s_codex_pet_frame_count = CODEX_PET_IDLE_FRAME_COUNT;

// Claude pet (OpenPets "claude" default — orange octopus) — mirror of the
// codex pet machinery. row 0 = idle (slow blinks), row 7 = running (working).
static lv_obj_t* s_claude_pet_img = nullptr;
static lv_timer_t* s_claude_pet_timer = nullptr;
static uint8_t s_claude_pet_frame = 0;
static const lv_image_dsc_t* const* s_claude_pet_frames = claude_pet_idle_frames;
static const uint32_t* s_claude_pet_durations = claude_pet_idle_durations_ms;
static uint8_t s_claude_pet_frame_count = CLAUDE_PET_IDLE_FRAME_COUNT;

#define LV_BUF_LINES 60   // 468 * 60 * 2 bytes = ~56 KiB per buffer × 2 = ~112 KiB in PSRAM

static void lvgl_tick_cb(void*) { lv_tick_inc(10); }

static void lvgl_task(void*) {
    while (true) {
        if (xSemaphoreTake(s_gui_sema, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_gui_sema);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    auto* g = (StopWatchGfx*)lv_display_get_driver_data(disp);
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    g->startWrite();
    g->setAddrWindow(area->x1, area->y1, w, h);
    g->writePixels((lgfx::rgb565_t*)px, w * h);
    g->endWrite();
    lv_display_flush_ready(disp);
}

bool lvgl_lock(void) {
    return xSemaphoreTake(s_gui_sema, portMAX_DELAY) == pdTRUE;
}
void lvgl_unlock(void) {
    xSemaphoreGive(s_gui_sema);
}

// Helper: a labelled text row on a given screen at given vertical offset from top.
static lv_obj_t* mk_text(lv_obj_t* parent, const char* text, int x, int y,
                         const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t* mk_center_text(lv_obj_t* parent, const char* text, int y,
                                const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void fit_label(lv_obj_t* label, int width) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static lv_obj_t* mk_hbar(lv_obj_t* parent, int x, int y, int w, int h,
                         uint32_t fill_color) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x303042), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(fill_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    return b;
}

static lv_obj_t* mk_arc(lv_obj_t* parent, int x, int y, int size,
                        uint32_t color, int value, int rotation) {
    lv_obj_t* a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_set_pos(a, x, y);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, value);
    lv_arc_set_rotation(a, rotation);
    lv_arc_set_bg_angles(a, 0, 115);
    lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(a, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x343246), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static void codex_pet_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_codex_pet_img) return;
    s_codex_pet_frame = (s_codex_pet_frame + 1) % s_codex_pet_frame_count;
    lv_image_set_src(s_codex_pet_img, s_codex_pet_frames[s_codex_pet_frame]);
    lv_timer_set_period(s_codex_pet_timer, s_codex_pet_durations[s_codex_pet_frame]);
}

static void claude_pet_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (!s_claude_pet_img) return;
    s_claude_pet_frame = (s_claude_pet_frame + 1) % s_claude_pet_frame_count;
    lv_image_set_src(s_claude_pet_img, s_claude_pet_frames[s_claude_pet_frame]);
    lv_timer_set_period(s_claude_pet_timer, s_claude_pet_durations[s_claude_pet_frame]);
}

static void set_claude_pet_recording(bool recording) {
    if (!s_claude_pet_img) return;
    if (recording) {
        s_claude_pet_frames     = claude_pet_running_frames;
        s_claude_pet_durations  = claude_pet_running_durations_ms;
        s_claude_pet_frame_count = CLAUDE_PET_RUNNING_FRAME_COUNT;
    } else {
        s_claude_pet_frames     = claude_pet_idle_frames;
        s_claude_pet_durations  = claude_pet_idle_durations_ms;
        s_claude_pet_frame_count = CLAUDE_PET_IDLE_FRAME_COUNT;
    }
    s_claude_pet_frame = 0;
    lv_image_set_src(s_claude_pet_img, s_claude_pet_frames[0]);
    if (s_claude_pet_timer) lv_timer_set_period(s_claude_pet_timer, s_claude_pet_durations[0]);
}

static void draw_claude_pet(lv_obj_t* parent, int cx, int cy) {
    s_claude_pet_frame = 0;
    set_claude_pet_recording(false);
    s_claude_pet_img = lv_image_create(parent);
    lv_image_set_src(s_claude_pet_img, claude_pet_idle_frames[0]);
    lv_obj_set_size(s_claude_pet_img, CLAUDE_PET_FRAME_W, CLAUDE_PET_FRAME_H);
    lv_obj_set_pos(s_claude_pet_img, cx - CLAUDE_PET_FRAME_W / 2, cy - CLAUDE_PET_FRAME_H / 2);
    lv_image_set_antialias(s_claude_pet_img, false);
    lv_obj_clear_flag(s_claude_pet_img, LV_OBJ_FLAG_CLICKABLE);

    if (!s_claude_pet_timer) {
        s_claude_pet_timer = lv_timer_create(claude_pet_timer_cb,
                                             claude_pet_idle_durations_ms[0],
                                             nullptr);
    }
}

static void set_codex_pet_recording(bool recording) {
    if (!s_codex_pet_img) return;
    if (recording) {
        s_codex_pet_frames = codex_pet_typing_frames;
        s_codex_pet_durations = codex_pet_typing_durations_ms;
        s_codex_pet_frame_count = CODEX_PET_TYPING_FRAME_COUNT;
    } else {
        s_codex_pet_frames = codex_pet_idle_frames;
        s_codex_pet_durations = codex_pet_idle_durations_ms;
        s_codex_pet_frame_count = CODEX_PET_IDLE_FRAME_COUNT;
    }
    s_codex_pet_frame = 0;
    lv_image_set_src(s_codex_pet_img, s_codex_pet_frames[0]);
    if (s_codex_pet_timer) {
        lv_timer_set_period(s_codex_pet_timer, s_codex_pet_durations[0]);
    }
}

static void draw_codex_pet(lv_obj_t* parent, int cx, int cy) {
    s_codex_pet_frame = 0;
    set_codex_pet_recording(false);
    s_codex_pet_img = lv_image_create(parent);
    lv_image_set_src(s_codex_pet_img, codex_pet_idle_frames[0]);
    lv_obj_set_size(s_codex_pet_img, CODEX_PET_FRAME_W, CODEX_PET_FRAME_H);
    lv_obj_set_pos(s_codex_pet_img, cx - CODEX_PET_FRAME_W / 2, cy - 58);
    lv_image_set_antialias(s_codex_pet_img, false);
    lv_obj_clear_flag(s_codex_pet_img, LV_OBJ_FLAG_CLICKABLE);

    if (!s_codex_pet_timer) {
        s_codex_pet_timer = lv_timer_create(codex_pet_timer_cb,
                                            codex_pet_idle_durations_ms[0],
                                            nullptr);
    }
}

// Pink ghost mascot — approximation of the mockup using LVGL primitives.
// Body: rounded square in pink (#FF7AB6). Eyes: two black dots.
// Headphone arc: red rectangle on top + small caps at sides.
static void build_claude_screen(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_claude = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Title + underline. Claude family color is orange (matches Anthropic
    // branding); Codex page uses cyan for visual contrast on swipe.
    mk_center_text(scr, "Claude Code", 30, &lv_font_montserrat_24, 0xFF6B35);
    lv_obj_t* line = lv_obj_create(scr);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(line, 126, 4);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xFF6B35), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 62);

    // Recording indicator — shared visual language with Codex page.
    s_rec_dot = lv_obj_create(scr);
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_rec_dot, 12, 12);
    lv_obj_set_style_radius(s_rec_dot, 6, 0);
    lv_obj_set_style_bg_color(s_rec_dot, lv_color_hex(0xFF2F2F), 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_align(s_rec_dot, LV_ALIGN_TOP_MID, 0, 79);
    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);

    // Left arc: MODEL (orange family color)
    mk_arc(scr, 70, 118, 92, 0xFF6B35, 62, 195);
    mk_text(scr, "MODEL", 91, 150, &lv_font_montserrat_14, 0xF2EAF9);
    s_model_value = mk_text(scr, "-", 79, 169, &lv_font_montserrat_18, 0xFFFFFF);
    fit_label(s_model_value, 86);
    mk_text(scr, "claude", 95, 194, &lv_font_montserrat_14, 0xD8D0F0);

    // Center mascot — orange octopus (openpets default "claude" pet)
    draw_claude_pet(scr, 234, 160);

    // Right arc: MODE (cyan family color)
    mk_arc(scr, 306, 118, 92, 0x88CCEE, 50, 230);
    mk_text(scr, "MODE", 332, 150, &lv_font_montserrat_14, 0xF2EAF9);
    s_mode_value = mk_text(scr, "IDLE", 333, 171, &lv_font_montserrat_18, 0x88CCEE);
    mk_text(scr, "session", 320, 197, &lv_font_montserrat_14, 0xD8D0F0);

    // Live Metrics header + line
    mk_text(scr, "Live Metrics", 70, 244, &lv_font_montserrat_14, 0xFF6B35);
    lv_obj_t* metric_line = lv_obj_create(scr);
    lv_obj_remove_flag(metric_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(metric_line, 112, 2);
    lv_obj_set_style_bg_color(metric_line, lv_color_hex(0x38354A), 0);
    lv_obj_set_style_border_width(metric_line, 0, 0);
    lv_obj_set_pos(metric_line, 245, 252);

    // OUT row: tokens out / bar (scale: 100k = 100%)
    mk_text(scr, "OUT", 77, 272, &lv_font_montserrat_14, 0xF2EAF9);
    s_out_value = mk_text(scr, "0", 132, 271, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_out_value, 86);
    s_out_bar = mk_hbar(scr, 236, 272, 124, 14, 0xFF6B35);
    s_out_pct_label = mk_text(scr, "0%", 370, 268, &lv_font_montserrat_14, 0xFF6B42);

    // COST row: $X.XX / bar (scale: $10 = 100%)
    mk_text(scr, "COST", 77, 304, &lv_font_montserrat_14, 0xF2EAF9);
    s_cost_value = mk_text(scr, "$0.00", 132, 303, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_cost_value, 86);
    s_cost_bar = mk_hbar(scr, 236, 304, 124, 14, 0x88CCEE);
    s_cost_pct_label = mk_text(scr, "0%", 370, 300, &lv_font_montserrat_14, 0x88CCEE);

    // TodoList header + line
    mk_text(scr, "TodoList", 100, 334, &lv_font_montserrat_18, 0xF3EEF8);
    lv_obj_t* todo_line = lv_obj_create(scr);
    lv_obj_remove_flag(todo_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(todo_line, 155, 2);
    lv_obj_set_style_bg_color(todo_line, lv_color_hex(0x48475C), 0);
    lv_obj_set_style_border_width(todo_line, 0, 0);
    lv_obj_set_pos(todo_line, 216, 344);

    // Centered "X / Y" + per-todo dot matrix below.
    s_todos_label = mk_center_text(scr, "- / -", 360, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_align(s_todos_label, LV_ALIGN_TOP_MID, 0, 360);

    // Pre-allocate the dot widgets — show/hide/recolor based on status string.
    // Each dot is 10px diameter (stride managed in ui_set_todos_statuses).
    constexpr int DOT_D = 10;
    constexpr int DOTS_ROW_Y = 392;
    for (int i = 0; i < TODO_DOTS_MAX; ++i) {
        lv_obj_t* dot = lv_obj_create(scr);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, DOT_D, DOT_D);
        lv_obj_set_style_radius(dot, DOT_D / 2, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_todo_dots[i] = dot;
        // Positioned on demand in ui_set_todos_statuses (centering depends on count).
        lv_obj_set_pos(dot, 0, DOTS_ROW_Y);
    }

    // Transcript at the very bottom — fills as ASR runs (Montserrat, ASCII).
    s_transcript_label = mk_center_text(scr, "", 418, &lv_font_montserrat_14, 0xFFD700);
    lv_obj_set_width(s_transcript_label, 380);
    lv_label_set_long_mode(s_transcript_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_transcript_label, LV_ALIGN_TOP_MID, 0, 418);

    // Battery indicator at the very bottom.
    s_battery_claude = lv_label_create(scr);
    lv_label_set_text(s_battery_claude, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_set_style_text_color(s_battery_claude, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_battery_claude, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_claude, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_codex_screen(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_codex = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    mk_center_text(scr, "CodeX", 30, &lv_font_montserrat_24, 0x25C7FF);
    lv_obj_t* line = lv_obj_create(scr);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(line, 126, 4);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x25C7FF), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 62);

    s_codex_rec_dot = lv_obj_create(scr);
    lv_obj_remove_flag(s_codex_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_codex_rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_codex_rec_dot, 12, 12);
    lv_obj_set_style_radius(s_codex_rec_dot, 6, 0);
    lv_obj_set_style_bg_color(s_codex_rec_dot, lv_color_hex(0xFF2F2F), 0);
    lv_obj_set_style_border_width(s_codex_rec_dot, 0, 0);
    lv_obj_align(s_codex_rec_dot, LV_ALIGN_TOP_MID, 0, 79);
    lv_obj_add_flag(s_codex_rec_dot, LV_OBJ_FLAG_HIDDEN);

    mk_arc(scr, 70, 118, 92, 0xFF4A2D, 62, 195);
    mk_text(scr, "MODEL", 91, 150, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_model_value = mk_text(scr, "GPT-5.5", 79, 169, &lv_font_montserrat_18, 0xFFFFFF);
    mk_text(scr, "auto", 101, 194, &lv_font_montserrat_14, 0xD8D0F0);

    draw_codex_pet(scr, 234, 160);

    mk_arc(scr, 306, 118, 92, 0x25C7FF, 76, 230);
    mk_text(scr, "EFFORT", 329, 150, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_effort_value = mk_text(scr, "--", 333, 171, &lv_font_montserrat_18, 0x25C7FF);
    mk_text(scr, "reasoning", 318, 197, &lv_font_montserrat_14, 0xD8D0F0);

    mk_text(scr, "Live Metrics", 70, 244, &lv_font_montserrat_14, 0xFF4A2D);
    lv_obj_t* metric_line = lv_obj_create(scr);
    lv_obj_remove_flag(metric_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(metric_line, 112, 2);
    lv_obj_set_style_bg_color(metric_line, lv_color_hex(0x38354A), 0);
    lv_obj_set_style_border_width(metric_line, 0, 0);
    lv_obj_set_pos(metric_line, 245, 252);

    mk_text(scr, "5H", 77, 272, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_5h_value = mk_text(scr, "--% used", 132, 271, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_codex_5h_value, 86);
    s_codex_5h_bar = mk_hbar(scr, 236, 272, 124, 14, 0xFF4A2D);
    s_codex_5h_pct_label = mk_text(scr, "--%", 370, 268, &lv_font_montserrat_14, 0xFF6B42);

    mk_text(scr, "WEEK", 77, 304, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_week_value = mk_text(scr, "--% used", 132, 303, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_codex_week_value, 86);
    s_codex_week_bar = mk_hbar(scr, 236, 304, 124, 14, 0x21BFFF);
    s_codex_week_pct_label = mk_text(scr, "--%", 370, 300, &lv_font_montserrat_14, 0x21BFFF);

    // Activity section (Codex.app focus time, derived from frontmost-app
    // polling on the Mac). Two rows: TODAY and WEEK.
    mk_text(scr, "Activity", 100, 334, &lv_font_montserrat_18, 0xF3EEF8);
    lv_obj_t* act_line = lv_obj_create(scr);
    lv_obj_remove_flag(act_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(act_line, 155, 2);
    lv_obj_set_style_bg_color(act_line, lv_color_hex(0x48475C), 0);
    lv_obj_set_style_border_width(act_line, 0, 0);
    lv_obj_set_pos(act_line, 216, 344);

    mk_text(scr, "DAY", 77, 360, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_today_value = mk_text(scr, "-- min", 132, 359, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_codex_today_value, 86);
    s_codex_today_bar = mk_hbar(scr, 236, 360, 124, 12, 0xFF4A2D);
    s_codex_today_pct = mk_text(scr, "--%", 370, 356, &lv_font_montserrat_14, 0xFF6B42);

    mk_text(scr, "WK", 77, 388, &lv_font_montserrat_14, 0xF2EAF9);
    s_codex_actweek_value = mk_text(scr, "-- min", 132, 387, &lv_font_montserrat_14, 0xFFFFFF);
    fit_label(s_codex_actweek_value, 86);
    s_codex_actweek_bar = mk_hbar(scr, 236, 388, 124, 12, 0x21BFFF);
    s_codex_actweek_pct = mk_text(scr, "--%", 370, 384, &lv_font_montserrat_14, 0x21BFFF);

    // Battery indicator at the very bottom.
    s_battery_codex = lv_label_create(scr);
    lv_label_set_text(s_battery_codex, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_set_style_text_color(s_battery_codex, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_battery_codex, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_codex, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void build_setup_screen(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_setup = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Top-of-screen attention header.
    lv_obj_t* head = lv_label_create(scr);
    lv_label_set_text(head, "WIFI SETUP");
    lv_obj_set_style_text_color(head, lv_color_hex(0xFF4A2D), 0);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_24, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "Connect phone to:");
    lv_obj_set_style_text_color(sub, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 150);

    s_setup_ssid_label = lv_label_create(scr);
    lv_label_set_text(s_setup_ssid_label, "Meme-XXXX");
    lv_obj_set_style_text_color(s_setup_ssid_label, lv_color_hex(0x25C7FF), 0);
    lv_obj_set_style_text_font(s_setup_ssid_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_setup_ssid_label, LV_ALIGN_TOP_MID, 0, 180);

    lv_obj_t* then = lv_label_create(scr);
    lv_label_set_text(then, "Then open:");
    lv_obj_set_style_text_color(then, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(then, &lv_font_montserrat_14, 0);
    lv_obj_align(then, LV_ALIGN_TOP_MID, 0, 240);

    s_setup_url_label = lv_label_create(scr);
    lv_label_set_text(s_setup_url_label, "192.168.4.1");
    lv_obj_set_style_text_color(s_setup_url_label, lv_color_hex(0x25C7FF), 0);
    lv_obj_set_style_text_font(s_setup_url_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_setup_url_label, LV_ALIGN_TOP_MID, 0, 270);

    lv_obj_t* tip = lv_label_create(scr);
    lv_label_set_text(tip, "After saving, device reboots.");
    lv_obj_set_style_text_color(tip, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(tip, &lv_font_montserrat_14, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 340);
}

static void build_boot_screen(void) {
    build_claude_screen();
    build_codex_screen();
    build_setup_screen();
    lv_screen_load(s_scr_claude);
}

void ui_show_wifi_setup(const char* hotspot_ssid, const char* url) {
    if (!s_scr_setup) return;
    if (!lvgl_lock()) return;
    if (s_setup_ssid_label && hotspot_ssid && hotspot_ssid[0]) {
        lv_label_set_text(s_setup_ssid_label, hotspot_ssid);
    }
    if (s_setup_url_label && url && url[0]) {
        lv_label_set_text(s_setup_url_label, url);
    }
    lv_screen_load(s_scr_setup);
    lvgl_unlock();
}

void ui_set_recording(bool on) {
    if (lvgl_lock()) {
        if (s_rec_dot) {
            if (on) lv_obj_clear_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_codex_rec_dot) {
            if (on) lv_obj_clear_flag(s_codex_rec_dot, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(s_codex_rec_dot, LV_OBJ_FLAG_HIDDEN);
        }
        set_codex_pet_recording(on);
        set_claude_pet_recording(on);
        lvgl_unlock();
    }
}

void ui_set_sleeping(bool sleeping) {
    if (lvgl_lock()) {
        if (s_codex_pet_timer) {
            if (sleeping) lv_timer_pause(s_codex_pet_timer);
            else          lv_timer_resume(s_codex_pet_timer);
        }
        if (s_claude_pet_timer) {
            if (sleeping) lv_timer_pause(s_claude_pet_timer);
            else          lv_timer_resume(s_claude_pet_timer);
        }
        lvgl_unlock();
    }
    if (s_gfx) {
        s_gfx->setBrightness(sleeping ? 0 : 180);
    }
}

bool ui_consume_touch_activity(void) {
    bool touched = s_touch_activity;
    s_touch_activity = false;
    return touched;
}

void ui_set_model(const char* model) {
    if (!s_model_value || !model || !model[0]) return;
    if (!lvgl_lock()) return;
    // Compact form for the arc display: strip the "claude-" prefix and
    // squash the patch version, e.g. "claude-opus-4-7" → "opus-4.7".
    char short_name[20];
    const char* src = model;
    if (strncmp(src, "claude-", 7) == 0) src += 7;
    size_t i = 0;
    for (; i < sizeof(short_name) - 1 && src[i]; ++i) short_name[i] = src[i];
    short_name[i] = 0;
    // Replace last '-' with '.' to look like a version number.
    char* last_dash = strrchr(short_name, '-');
    if (last_dash) *last_dash = '.';
    lv_label_set_text(s_model_value, short_name);
    lvgl_unlock();
}

void ui_set_codex_model(const char* model) {
    if (!s_codex_model_value || !model || !model[0]) return;
    if (lvgl_lock()) {
        char m[18];
        strncpy(m, model, sizeof(m) - 1);
        m[sizeof(m) - 1] = 0;
        lv_label_set_text(s_codex_model_value, m);
        lvgl_unlock();
    }
}

void ui_set_codex_effort(const char* effort) {
    if (!s_codex_effort_value || !effort) return;
    if (!lvgl_lock()) return;
    if (!effort[0]) {
        lv_label_set_text(s_codex_effort_value, "--");
    } else {
        // Uppercase for display ("high" → "HIGH").
        char up[12];
        size_t i = 0;
        for (; i < sizeof(up) - 1 && effort[i]; ++i) {
            char c = effort[i];
            up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        up[i] = 0;
        lv_label_set_text(s_codex_effort_value, up);
    }
    lvgl_unlock();
}

// Bar scale factors. 100M tokens / $5000 = 100%.
static constexpr int64_t CLAUDE_TOKENS_BAR_MAX = 100000000LL;
static constexpr float   CLAUDE_COST_BAR_MAX   = 5000.0f;

void ui_set_claude_metrics(int tokens_out, float cost_usd, bool busy) {
    if (!lvgl_lock()) return;
    char b[32];

    // OUT row
    if (s_out_value) {
        if (tokens_out >= 1000) snprintf(b, sizeof(b), "%.1fK", tokens_out / 1000.0);
        else                    snprintf(b, sizeof(b), "%d", tokens_out);
        lv_label_set_text(s_out_value, b);
    }
    int out_pct = (int)((int64_t)tokens_out * 100 / CLAUDE_TOKENS_BAR_MAX);
    if (out_pct > 100) out_pct = 100;
    if (s_out_bar)       lv_bar_set_value(s_out_bar, out_pct, LV_ANIM_OFF);
    if (s_out_pct_label) {
        snprintf(b, sizeof(b), "%d%%", out_pct);
        lv_label_set_text(s_out_pct_label, b);
    }

    // COST row
    if (s_cost_value) {
        snprintf(b, sizeof(b), "$%.2f", cost_usd);
        lv_label_set_text(s_cost_value, b);
    }
    int cost_pct = (int)(cost_usd * 100.0f / CLAUDE_COST_BAR_MAX);
    if (cost_pct > 100) cost_pct = 100;
    if (s_cost_pct_label) {
        snprintf(b, sizeof(b), "%d%%", cost_pct);
        lv_label_set_text(s_cost_pct_label, b);
    }
    if (s_cost_bar) lv_bar_set_value(s_cost_bar, cost_pct, LV_ANIM_OFF);

    // Right-arc mode text
    if (s_mode_value) {
        lv_label_set_text(s_mode_value, busy ? "BUSY" : "IDLE");
    }

    lvgl_unlock();
}

void ui_set_todos_progress(int done, int total, const char* current, const char* recent_done) {
    (void)current; (void)recent_done;  // new layout only shows X/Y count.
    if (!s_todos_label) return;
    if (!lvgl_lock()) return;
    char b[24];
    snprintf(b, sizeof(b), "%d / %d", done, total);
    lv_label_set_text(s_todos_label, b);
    lvgl_unlock();
}

void ui_set_codex_limits(int five_hour_left_pct, const char* five_hour_reset,
                         int week_left_pct, const char* week_reset) {
    // Server still sends `left_pct` (kept stable so ws_client.cpp doesn't change),
    // but UI now shows "% used" — progress bars feel more natural that way.
    if (!lvgl_lock()) return;
    char b[96];

    if (five_hour_left_pct < 0) five_hour_left_pct = 0;
    if (five_hour_left_pct > 100) five_hour_left_pct = 100;
    if (week_left_pct < 0) week_left_pct = 0;
    if (week_left_pct > 100) week_left_pct = 100;

    int used_5h = 100 - five_hour_left_pct;
    int used_wk = 100 - week_left_pct;

    if (s_codex_5h_value) {
        snprintf(b, sizeof(b), "%d%% used", used_5h);
        lv_label_set_text(s_codex_5h_value, b);
    }
    if (s_codex_5h_bar) {
        lv_bar_set_value(s_codex_5h_bar, used_5h, LV_ANIM_OFF);
    }
    if (s_codex_5h_pct_label) {
        snprintf(b, sizeof(b), "%d%%", used_5h);
        lv_label_set_text(s_codex_5h_pct_label, b);
    }

    if (s_codex_week_value) {
        snprintf(b, sizeof(b), "%d%% used", used_wk);
        lv_label_set_text(s_codex_week_value, b);
    }
    if (s_codex_week_bar) {
        lv_bar_set_value(s_codex_week_bar, used_wk, LV_ANIM_OFF);
    }
    if (s_codex_week_pct_label) {
        snprintf(b, sizeof(b), "%d%%", used_wk);
        lv_label_set_text(s_codex_week_pct_label, b);
    }

    // Reset times intentionally not displayed — quota source not wired up
    // yet, so the values would be empty placeholders.
    (void)five_hour_reset;
    (void)week_reset;
    lvgl_unlock();
}

void ui_set_transcript(const char* text) {
    if (!s_transcript_label || !text) return;
    if (lvgl_lock()) {
        lv_label_set_text(s_transcript_label, text);
        lvgl_unlock();
    }
}

void ui_set_todos_statuses(const char* statuses) {
    if (!statuses) statuses = "";
    int n = 0;
    while (statuses[n] && n < TODO_DOTS_MAX) ++n;
    if (!lvgl_lock()) return;
    constexpr int DOT_D = 10;
    constexpr int DOT_STRIDE = 14;
    constexpr int CENTER_X = 234;  // Claude page horizontal center
    int total_w = n > 0 ? (n * DOT_D + (n - 1) * (DOT_STRIDE - DOT_D)) : 0;
    int x0 = CENTER_X - total_w / 2;

    for (int i = 0; i < TODO_DOTS_MAX; ++i) {
        if (!s_todo_dots[i]) continue;
        if (i >= n) {
            lv_obj_add_flag(s_todo_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_todo_dots[i], LV_OBJ_FLAG_HIDDEN);
        // Reposition X (Y was set at creation).
        int x = x0 + i * DOT_STRIDE;
        lv_obj_set_x(s_todo_dots[i], x);
        // Color by status: c=completed orange, i=in_progress cyan, else grey.
        uint32_t color = 0x444444;
        char s = statuses[i];
        if (s == 'c')      color = 0xFF6B35;
        else if (s == 'i') color = 0x88CCEE;
        lv_obj_set_style_bg_color(s_todo_dots[i], lv_color_hex(color), 0);
    }
    lvgl_unlock();
}

void ui_set_codex_activity(int today_min, int today_pct, int week_min, int week_pct) {
    if (today_pct < 0) today_pct = 0;
    if (today_pct > 100) today_pct = 100;
    if (week_pct  < 0) week_pct  = 0;
    if (week_pct  > 100) week_pct  = 100;
    if (!lvgl_lock()) return;
    char b[24];
    if (s_codex_today_value) {
        snprintf(b, sizeof(b), "%d min", today_min);
        lv_label_set_text(s_codex_today_value, b);
    }
    if (s_codex_today_bar) lv_bar_set_value(s_codex_today_bar, today_pct, LV_ANIM_OFF);
    if (s_codex_today_pct) {
        snprintf(b, sizeof(b), "%d%%", today_pct);
        lv_label_set_text(s_codex_today_pct, b);
    }
    if (s_codex_actweek_value) {
        snprintf(b, sizeof(b), "%d min", week_min);
        lv_label_set_text(s_codex_actweek_value, b);
    }
    if (s_codex_actweek_bar) lv_bar_set_value(s_codex_actweek_bar, week_pct, LV_ANIM_OFF);
    if (s_codex_actweek_pct) {
        snprintf(b, sizeof(b), "%d%%", week_pct);
        lv_label_set_text(s_codex_actweek_pct, b);
    }
    lvgl_unlock();
}

void ui_set_battery(int percent, bool charging) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Pick a battery glyph matching the percentage — LVGL's Montserrat fonts
    // bundle these in the symbol range.
    const char* sym;
    if (percent >= 88)      sym = LV_SYMBOL_BATTERY_FULL;
    else if (percent >= 63) sym = LV_SYMBOL_BATTERY_3;
    else if (percent >= 38) sym = LV_SYMBOL_BATTERY_2;
    else if (percent >= 15) sym = LV_SYMBOL_BATTERY_1;
    else                    sym = LV_SYMBOL_BATTERY_EMPTY;

    char buf[32];
    if (charging) snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %d%%", percent);
    else          snprintf(buf, sizeof(buf), "%s %d%%", sym, percent);

    // Tint green when charging, red below 15%, grey otherwise.
    uint32_t color = 0xAAAAAA;
    if (charging)     color = 0x3CB371;
    else if (percent < 15) color = 0xE74C3C;

    if (!lvgl_lock()) return;
    if (s_battery_claude) {
        lv_label_set_text(s_battery_claude, buf);
        lv_obj_set_style_text_color(s_battery_claude, lv_color_hex(color), 0);
    }
    if (s_battery_codex) {
        lv_label_set_text(s_battery_codex, buf);
        lv_obj_set_style_text_color(s_battery_codex, lv_color_hex(color), 0);
    }
    lvgl_unlock();
}

void display_init(void) {
    ESP_LOGI(TAG, "display init");
    s_gfx = std::make_unique<StopWatchGfx>();
    if (!s_gfx->init()) {
        ESP_LOGE(TAG, "M5GFX init failed");
        return;
    }
    ESP_LOGI(TAG, "panel %dx%d up", s_gfx->width(), s_gfx->height());

    lv_init();
    s_lv_disp = lv_display_create(s_gfx->width(), s_gfx->height());
    lv_display_set_driver_data(s_lv_disp, s_gfx.get());
    lv_display_set_flush_cb(s_lv_disp, flush_cb);

    size_t buf_bytes = s_gfx->width() * LV_BUF_LINES * sizeof(uint16_t);
    void* buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void* buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "lvgl buf alloc failed (%zu bytes each)", buf_bytes);
        return;
    }
    lv_display_set_buffers(s_lv_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_gui_sema = xSemaphoreCreateMutex();
    const esp_timer_create_args_t tcfg = {.callback = &lvgl_tick_cb, .name = "lv_tick"};
    esp_timer_handle_t th;
    esp_timer_create(&tcfg, &th);
    esp_timer_start_periodic(th, 10 * 1000);

    xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr);

    if (lvgl_lock()) {
        build_boot_screen();
        lvgl_unlock();
    }
    ESP_LOGI(TAG, "lvgl ready");
}

// --- touch + gestures ---
static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (!s_touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (s_touch->read() && s_touch->isPressed()) {
        s_touch_activity = true;
        data->point.x = s_touch->getX();
        data->point.y = s_touch->getY();
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void gesture_cb(lv_event_t* e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    lv_obj_t* cur = lv_screen_active();
    if (dir == LV_DIR_LEFT && cur == s_scr_claude && s_scr_codex) {
        lv_screen_load_anim(s_scr_codex, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
    } else if (dir == LV_DIR_RIGHT && cur == s_scr_codex && s_scr_claude) {
        lv_screen_load_anim(s_scr_claude, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    }
}

void touch_init(i2c_bus_handle_t bus) {
    auto* master = i2c_bus_get_internal_bus_handle(bus);
    s_touch = std::make_unique<Cst820>();
    if (!s_touch->begin(master, 0x15)) {
        ESP_LOGE(TAG, "CST820 init failed");
        s_touch.reset();
        return;
    }
    ESP_LOGI(TAG, "CST820 touch ready");

    // LVGL pointer input device.
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_lv_disp);

    // Register gesture handler on both screens.
    if (lvgl_lock()) {
        if (s_scr_claude) lv_obj_add_event_cb(s_scr_claude, gesture_cb, LV_EVENT_GESTURE, nullptr);
        if (s_scr_codex)  lv_obj_add_event_cb(s_scr_codex,  gesture_cb, LV_EVENT_GESTURE, nullptr);
        lvgl_unlock();
    }
}
