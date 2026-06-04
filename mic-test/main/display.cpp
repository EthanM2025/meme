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
#include "kitty_pet_assets.h"
#include "diana_pet_assets.h"
#include "nier_pet_assets.h"
#include "esp_random.h"

// 48x48 RGB565 brand logos from alexjc-tech/cc-island.
extern "C" const lv_image_dsc_t logo_claude;
extern "C" const lv_image_dsc_t logo_codex;

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

// Screens form a "cross": Claude ↔ Codex horizontally.
//   swipe UP   from a main  → Dashboard (data summary)
//   swipe DOWN from a main  → Pet (animation-only, no UI chrome)
//   on Dashboard / any Pet, swipe in the inverse direction → last main
//   left/right on a pet screen → cycle to next/prev pet (linear, edge-bounded)
static constexpr int NUM_PETS = 3;
static lv_obj_t* s_scr_claude    = nullptr;
static lv_obj_t* s_scr_codex     = nullptr;
static lv_obj_t* s_scr_dashboard = nullptr;
static lv_obj_t* s_scr_pets[NUM_PETS] = {nullptr};
static int s_pet_active          = 0;          // index into s_scr_pets[]
static lv_obj_t* s_last_main     = nullptr;   // claude or codex; updated when we leave a main
// Setup screen — only shown while SoftAP provisioning is active.
static lv_obj_t* s_scr_setup  = nullptr;

// Hard-coded daily token cap for API-key users (no real quota endpoint).
// Used to derive the "X% remaining" on the dashboard's Claude half.
static constexpr int CLAUDE_DAILY_TOKEN_CAP = 3000000;
static lv_obj_t* s_setup_ssid_label = nullptr;
static lv_obj_t* s_setup_url_label  = nullptr;

// Claude-page widgets.
//   Title:     [logo] "Claude"
//   Arcs:      MODEL (model name) + STATUS (BUSY/IDLE)
//   Bars:      5H + 7D quota in mockup style (label/reset above thin bar,
//              filled by REMAINING percent)
//   TodoList:  X / Y + dot matrix
//   Summary:   "今日 $X X.XK Token HH:MM"
static lv_obj_t* s_rec_dot = nullptr;
static lv_obj_t* s_model_value      = nullptr;   // left arc center: model short name
static lv_obj_t* s_mode_value       = nullptr;   // right arc center: BUSY / IDLE
static lv_obj_t* s_claude_model_arc = nullptr;   // left arc fill (decorative now)
static lv_obj_t* s_claude_mode_arc  = nullptr;   // right arc fill (decorative now)
// Claude page uses TODAY $ / TOKENS rows instead of 5H/7D quota bars.
// Subscription-only OAuth users have quota; API-key users don't, and that's the common case.
static lv_obj_t* s_claude_today_label  = nullptr;
static lv_obj_t* s_claude_today_leader = nullptr;   // thin gray line, width adjusted to abut value
static lv_obj_t* s_claude_today_value  = nullptr;   // "$3.50" big number, right
static lv_obj_t* s_claude_tokens_label = nullptr;
static lv_obj_t* s_claude_tokens_leader = nullptr;
static lv_obj_t* s_claude_tokens_value = nullptr;   // "12.5K" big number, right
static lv_obj_t* s_todos_label      = nullptr;   // "X / Y" centered
static lv_obj_t* s_claude_summary   = nullptr;   // bottom summary line
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
static lv_obj_t* s_codex_model_arc  = nullptr;     // left arc fill — now 5h remaining %
static lv_obj_t* s_codex_effort_arc = nullptr;     // right arc fill — now 7d remaining %
static lv_obj_t* s_codex_5h_label = nullptr;       // "5H" big label
static lv_obj_t* s_codex_5h_pct   = nullptr;       // big "X%" number, right
static lv_obj_t* s_codex_5h_reset = nullptr;       // small "reset HH:MM" above bar
static lv_obj_t* s_codex_5h_bar   = nullptr;
static lv_obj_t* s_codex_7d_label = nullptr;
static lv_obj_t* s_codex_7d_pct   = nullptr;
static lv_obj_t* s_codex_7d_reset = nullptr;
static lv_obj_t* s_codex_7d_bar   = nullptr;
static lv_obj_t* s_codex_pet_img = nullptr;
static lv_timer_t* s_codex_pet_timer = nullptr;
static uint8_t s_codex_pet_frame = 0;
static const lv_image_dsc_t* const* s_codex_pet_frames = codex_pet_idle_frames;
static const uint32_t* s_codex_pet_durations = codex_pet_idle_durations_ms;
static uint8_t s_codex_pet_frame_count = CODEX_PET_IDLE_FRAME_COUNT;

// Per-pet animation lookup table — kitty / diana / nier share identical
// frame counts and durations (see /tmp/gen_pet.py), so all the state machine
// needs from a given pet is which frame array to pull from.
struct PetSpec {
    const lv_image_dsc_t* const* idle_frames;
    const uint32_t*              idle_durs;
    const lv_image_dsc_t* const* wave_frames;
    const uint32_t*              wave_durs;
    const lv_image_dsc_t* const* fail_frames;
    const uint32_t*              fail_durs;
    const lv_image_dsc_t* const* wait_frames;
    const uint32_t*              wait_durs;
    const lv_image_dsc_t* const* review_frames;
    const uint32_t*              review_durs;
    uint8_t idle_count, wave_count, fail_count, wait_count, review_count;
    uint16_t frame_w, frame_h;   // for screen-centering math
};
static const PetSpec PETS[NUM_PETS] = {
    {
        kitty_pet_idle_frames,   kitty_pet_idle_durations_ms,
        kitty_pet_wave_frames,   kitty_pet_wave_durations_ms,
        kitty_pet_fail_frames,   kitty_pet_fail_durations_ms,
        kitty_pet_wait_frames,   kitty_pet_wait_durations_ms,
        kitty_pet_review_frames, kitty_pet_review_durations_ms,
        KITTY_PET_IDLE_FRAME_COUNT, KITTY_PET_WAVE_FRAME_COUNT,
        KITTY_PET_FAIL_FRAME_COUNT, KITTY_PET_WAIT_FRAME_COUNT,
        KITTY_PET_REVIEW_FRAME_COUNT,
        KITTY_PET_FRAME_W, KITTY_PET_FRAME_H,
    },
    {
        diana_pet_idle_frames,   diana_pet_idle_durations_ms,
        diana_pet_wave_frames,   diana_pet_wave_durations_ms,
        diana_pet_fail_frames,   diana_pet_fail_durations_ms,
        diana_pet_wait_frames,   diana_pet_wait_durations_ms,
        diana_pet_review_frames, diana_pet_review_durations_ms,
        DIANA_PET_IDLE_FRAME_COUNT, DIANA_PET_WAVE_FRAME_COUNT,
        DIANA_PET_FAIL_FRAME_COUNT, DIANA_PET_WAIT_FRAME_COUNT,
        DIANA_PET_REVIEW_FRAME_COUNT,
        DIANA_PET_FRAME_W, DIANA_PET_FRAME_H,
    },
    {
        nier_pet_idle_frames,    nier_pet_idle_durations_ms,
        nier_pet_wave_frames,    nier_pet_wave_durations_ms,
        nier_pet_fail_frames,    nier_pet_fail_durations_ms,
        nier_pet_wait_frames,    nier_pet_wait_durations_ms,
        nier_pet_review_frames,  nier_pet_review_durations_ms,
        NIER_PET_IDLE_FRAME_COUNT, NIER_PET_WAVE_FRAME_COUNT,
        NIER_PET_FAIL_FRAME_COUNT, NIER_PET_WAIT_FRAME_COUNT,
        NIER_PET_REVIEW_FRAME_COUNT,
        NIER_PET_FRAME_W, NIER_PET_FRAME_H,
    },
};

// One image widget per pet screen — only the active screen's widget is updated.
static lv_obj_t* s_pet_imgs[NUM_PETS] = {nullptr};

// Dashboard widgets — Claude top half + Codex bottom half + bottom clock.
static lv_obj_t* s_dash_claude_big       = nullptr;   // "67%" big
static lv_obj_t* s_dash_claude_token_bar = nullptr;
static lv_obj_t* s_dash_claude_token_rst = nullptr;
static lv_obj_t* s_dash_claude_cost      = nullptr;   // "$3.50"
static lv_obj_t* s_dash_codex_big        = nullptr;
static lv_obj_t* s_dash_codex_5h_bar     = nullptr;
static lv_obj_t* s_dash_codex_5h_rst     = nullptr;
static lv_obj_t* s_dash_codex_7d_bar     = nullptr;
static lv_obj_t* s_dash_codex_7d_rst     = nullptr;
static lv_obj_t* s_dash_clock            = nullptr;

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

// Centered text within an arc's bounding box (x..x+92). Use this instead of
// hand-positioned mk_text so the label/value/subtitle line up with the arc
// regardless of how wide the text turns out to be.
static lv_obj_t* mk_arc_text(lv_obj_t* parent, int arc_x, int y,
                             const lv_font_t* font, uint32_t color,
                             const char* initial) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, initial);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_width(l, 92);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, arc_x, y);
    return l;
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

// Quota row: 4 widgets aligned to user's importance hierarchy.
//   left:   big label "5H" / "7D"   (18px, white)        — title
//   middle: bar (10px tall)         — visualization
//   right:  big "67%"                (24px, brand color)  — primary number
//   below bar, right-aligned: small "23:00" reset (14px gray) — supporting
struct QuotaRow {
    lv_obj_t* label;
    lv_obj_t* bar;
    lv_obj_t* pct;
    lv_obj_t* reset;
};
static QuotaRow mk_quota_row(lv_obj_t* parent, int y, uint32_t color,
                             const char* init_label) {
    QuotaRow r{};
    // Block is centered on the 466px display: label at x=54, pct right-edge at x=412.
    // Bar stretches all the way up to pct's left edge so "100%" sits right next to it.
    // Short values like "7%" still leave a gap by nature of right-aligning a short string,
    // but the bar itself now fills the row so the eye reads it as one connected block.
    constexpr int LABEL_X = 54;
    constexpr int BAR_X   = 102;
    constexpr int BAR_W   = 244;                  // bar ends at 346
    constexpr int PCT_X   = BAR_X + BAR_W;        // = 346 — pct box starts right at bar end
    constexpr int PCT_W   = 66;                    // right edge at 412; symmetric to LABEL_X=54

    // Big label on the left, vertically centered with the bar+pct row.
    r.label = lv_label_create(parent);
    lv_label_set_text(r.label, init_label);
    lv_obj_set_style_text_color(r.label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(r.label, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(r.label, LABEL_X, y - 2);

    // Bar in the middle.
    r.bar = lv_bar_create(parent);
    lv_obj_set_size(r.bar, BAR_W, 10);
    lv_obj_set_pos(r.bar, BAR_X, y + 8);
    lv_bar_set_range(r.bar, 0, 100);
    lv_obj_set_style_radius(r.bar, 5, 0);
    lv_obj_set_style_radius(r.bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(0x303042), 0);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, LV_PART_INDICATOR);

    // Big % on the right — fixed-width box right-aligned so "7%" and "100%" both end at the same edge.
    r.pct = lv_label_create(parent);
    lv_label_set_text(r.pct, "--%");
    lv_obj_set_style_text_color(r.pct, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(r.pct, &lv_font_montserrat_24, 0);
    lv_obj_set_width(r.pct, PCT_W);
    lv_obj_set_style_text_align(r.pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r.pct, PCT_X, y - 6);

    // Reset time floats just ABOVE the bar, right-aligned to the bar's right edge.
    // Same visual block as the bar — not its own row.
    r.reset = lv_label_create(parent);
    lv_label_set_text(r.reset, "");
    lv_obj_set_style_text_color(r.reset, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(r.reset, &lv_font_montserrat_14, 0);
    lv_obj_set_width(r.reset, BAR_W);
    lv_obj_set_style_text_align(r.reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r.reset, BAR_X, y - 8);
    return r;
}

// Stat row: same alignment as the quota row (LABEL_X=54 left, value right-edge at 412)
// but no bar/reset — for unbounded counters like today's cost or token total.
// A thin gray "leader" line bridges label and value so the eye reads the row as
// one connected element instead of two floating words on opposite edges.
struct StatRow {
    lv_obj_t* label;
    lv_obj_t* leader;
    lv_obj_t* value;
};
static StatRow mk_stat_row(lv_obj_t* parent, int y, uint32_t color,
                           const char* init_label) {
    constexpr int LABEL_X  = 54;
    constexpr int VALUE_W  = 100;                  // wide enough for "$999.99"
    constexpr int VALUE_X  = 412 - VALUE_W;        // = 312, right edge symmetric with quota row
    constexpr int LEADER_X = 130;                  // after the "TODAY"/"TOKENS" label
    constexpr int LEADER_W = 175;                  // ends at 305 — 7px gap before value box

    StatRow r{};
    r.label = lv_label_create(parent);
    lv_label_set_text(r.label, init_label);
    lv_obj_set_style_text_color(r.label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(r.label, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(r.label, LABEL_X, y - 2);

    r.leader = lv_obj_create(parent);
    lv_obj_remove_flag(r.leader, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r.leader, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r.leader, LEADER_W, 2);
    lv_obj_set_style_radius(r.leader, 1, 0);
    lv_obj_set_style_bg_color(r.leader, lv_color_hex(0x303042), 0);
    lv_obj_set_style_bg_opa(r.leader, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r.leader, 0, 0);
    lv_obj_set_style_pad_all(r.leader, 0, 0);
    lv_obj_set_pos(r.leader, LEADER_X, y + 12);    // sits at vertical mid of the value text

    r.value = lv_label_create(parent);
    lv_label_set_text(r.value, "--");
    lv_obj_set_style_text_color(r.value, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(r.value, &lv_font_montserrat_24, 0);
    lv_obj_set_width(r.value, VALUE_W);
    lv_obj_set_style_text_align(r.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r.value, VALUE_X, y - 6);
    return r;
}

static void build_claude_screen(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_claude = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Title row: 32px logo + 24px "Claude" text.
    // Logo widget bounding box is the original 48px even though displayed is scaled to ~32;
    // visible image sits centered inside (8px padding T/B/L/R).
    // Vertical centering: image center = widget_y + 24. Montserrat_24 cap center ≈ title_y + 14.
    // So widget_y = title_y - 10 puts both centers on the same line.
    lv_obj_t* logo = lv_image_create(scr);
    lv_image_set_src(logo, &logo_claude);
    lv_image_set_scale(logo, 170);   // 48 * 170/256 ≈ 32
    lv_obj_set_pos(logo, 162, 22);   // visible logo: x 170–202, y 30–62
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Claude");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6B35), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 212, 32);

    s_rec_dot = lv_obj_create(scr);
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_rec_dot, 12, 12);
    lv_obj_set_style_radius(s_rec_dot, 6, 0);
    lv_obj_set_style_bg_color(s_rec_dot, lv_color_hex(0xFF2F2F), 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_align(s_rec_dot, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);

    // Left arc: MODEL. Decorative fixed fill — API-key users have no quota to sync to.
    s_claude_model_arc = mk_arc(scr, 70, 105, 92, 0xFF6B35, 65, 195);
    mk_arc_text(scr, 70, 138, &lv_font_montserrat_14, 0xF2EAF9, "MODEL");
    s_model_value = mk_arc_text(scr, 70, 157, &lv_font_montserrat_18, 0xFFFFFF, "-");
    mk_arc_text(scr, 70, 182, &lv_font_montserrat_14, 0xD8D0F0, "claude");

    draw_claude_pet(scr, 234, 147);

    // Right arc: STATUS. Dynamic — set to 100 when busy, 30 when idle, in ui_set_claude_metrics.
    s_claude_mode_arc = mk_arc(scr, 306, 105, 92, 0x88CCEE, 30, 230);
    mk_arc_text(scr, 306, 138, &lv_font_montserrat_14, 0xF2EAF9, "STATUS");
    s_mode_value = mk_arc_text(scr, 306, 157, &lv_font_montserrat_18, 0x88CCEE, "IDLE");
    mk_arc_text(scr, 306, 182, &lv_font_montserrat_14, 0xD8D0F0, "session");

    // No quota for API-key users → show today's cost and tokens instead.
    // Same row alignment as Codex quota for visual consistency.
    StatRow today  = mk_stat_row(scr, 220, 0xFF6B35, "TODAY");
    s_claude_today_label  = today.label;
    s_claude_today_leader = today.leader;
    s_claude_today_value  = today.value;
    StatRow tokens = mk_stat_row(scr, 268, 0xFF6B35, "TOKENS");
    s_claude_tokens_label  = tokens.label;
    s_claude_tokens_leader = tokens.leader;
    s_claude_tokens_value  = tokens.value;

    // Inline TodoList header — text becomes "TodoList X / Y" (or hidden if 0 todos).
    s_todos_label = mk_center_text(scr, "TodoList", 300, &lv_font_montserrat_18, 0xF3EEF8);
    lv_obj_align(s_todos_label, LV_ALIGN_TOP_MID, 0, 300);

    constexpr int DOT_D = 10;
    constexpr int DOTS_ROW_Y = 328;
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
        lv_obj_set_pos(dot, 0, DOTS_ROW_Y);
    }

    // Bottom summary line: today cost + tokens + Mac local time.
    s_claude_summary = mk_center_text(scr, "Today $-- -- Tokens --:--",
                                       378, &lv_font_montserrat_14, 0xCCCCCC);
    lv_obj_align(s_claude_summary, LV_ALIGN_TOP_MID, 0, 378);

    s_transcript_label = mk_center_text(scr, "", 404, &lv_font_montserrat_14, 0xFFD700);
    lv_obj_set_width(s_transcript_label, 380);
    lv_label_set_long_mode(s_transcript_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_transcript_label, LV_ALIGN_TOP_MID, 0, 404);

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

    // Title row — same vertical centering logic as Claude (see comment there).
    lv_obj_t* logo = lv_image_create(scr);
    lv_image_set_src(logo, &logo_codex);
    lv_image_set_scale(logo, 170);
    lv_obj_set_pos(logo, 168, 22);   // visible logo: x 176–208, y 30–62
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Codex");
    lv_obj_set_style_text_color(title, lv_color_hex(0x25C7FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 218, 32);

    s_codex_rec_dot = lv_obj_create(scr);
    lv_obj_remove_flag(s_codex_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_codex_rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_codex_rec_dot, 12, 12);
    lv_obj_set_style_radius(s_codex_rec_dot, 6, 0);
    lv_obj_set_style_bg_color(s_codex_rec_dot, lv_color_hex(0xFF2F2F), 0);
    lv_obj_set_style_border_width(s_codex_rec_dot, 0, 0);
    lv_obj_align(s_codex_rec_dot, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_add_flag(s_codex_rec_dot, LV_OBJ_FLAG_HIDDEN);

    // Codex MODEL arc fills with 5h remaining %.
    s_codex_model_arc = mk_arc(scr, 70, 105, 92, 0xFF4A2D, 0, 195);
    mk_arc_text(scr, 70, 138, &lv_font_montserrat_14, 0xF2EAF9, "MODEL");
    s_codex_model_value = mk_arc_text(scr, 70, 157, &lv_font_montserrat_18, 0xFFFFFF, "GPT-5.5");
    mk_arc_text(scr, 70, 182, &lv_font_montserrat_14, 0xD8D0F0, "auto");

    draw_codex_pet(scr, 234, 147);

    // EFFORT arc: fill = 7d remaining % (synced); text shows reasoning level.
    s_codex_effort_arc = mk_arc(scr, 306, 105, 92, 0x25C7FF, 0, 230);
    mk_arc_text(scr, 306, 138, &lv_font_montserrat_14, 0xF2EAF9, "EFFORT");
    s_codex_effort_value = mk_arc_text(scr, 306, 157, &lv_font_montserrat_18, 0x25C7FF, "--");
    mk_arc_text(scr, 306, 182, &lv_font_montserrat_14, 0xD8D0F0, "reasoning");

    // Quota rows — both bars cyan; y=220 / y=268 (48px apart).
    QuotaRow q5 = mk_quota_row(scr, 220, 0x25C7FF, "5H");
    s_codex_5h_label = q5.label;
    s_codex_5h_pct   = q5.pct;
    s_codex_5h_reset = q5.reset;
    s_codex_5h_bar   = q5.bar;
    QuotaRow qw = mk_quota_row(scr, 268, 0x25C7FF, "7D");
    s_codex_7d_label = qw.label;
    s_codex_7d_pct   = qw.pct;
    s_codex_7d_reset = qw.reset;
    s_codex_7d_bar   = qw.bar;

    // Bottom block spread out — Codex has no TodoList/dots/summary, so we have ~140px
    // of room from bar bottom (~290) to battery (~440). Use it.
    mk_center_text(scr, "Activity", 320, &lv_font_montserrat_18, 0xF3EEF8);
    s_codex_today_value   = mk_center_text(scr, "DAY  -- m", 358,
                                            &lv_font_montserrat_14, 0xCCCCCC);
    s_codex_actweek_value = mk_center_text(scr, "WK  -- m", 388,
                                            &lv_font_montserrat_14, 0xCCCCCC);
    s_codex_today_bar     = nullptr;
    s_codex_today_pct     = nullptr;
    s_codex_actweek_bar   = nullptr;
    s_codex_actweek_pct   = nullptr;

    // Battery indicator at the very bottom.
    s_battery_codex = lv_label_create(scr);
    lv_label_set_text(s_battery_codex, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_set_style_text_color(s_battery_codex, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_battery_codex, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_codex, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// Compact bar row used inside the dashboard halves: label top-left, reset top-right,
// thin 4px bar below. Width is caller-controlled so it fits inside the circle.
struct DashBarRow {
    lv_obj_t* label;
    lv_obj_t* reset;
    lv_obj_t* bar;
};
static DashBarRow mk_dash_bar_row(lv_obj_t* parent, int x, int y, int width,
                                  uint32_t fill, const char* label_text) {
    DashBarRow r{};
    r.label = lv_label_create(parent);
    lv_label_set_text(r.label, label_text);
    lv_obj_set_style_text_color(r.label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(r.label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(r.label, x, y);

    r.reset = lv_label_create(parent);
    lv_label_set_text(r.reset, "reset --");
    lv_obj_set_style_text_color(r.reset, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(r.reset, &lv_font_montserrat_14, 0);
    lv_obj_set_width(r.reset, width);
    lv_obj_set_style_text_align(r.reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r.reset, x, y);

    r.bar = lv_bar_create(parent);
    lv_obj_set_size(r.bar, width, 6);
    lv_obj_set_pos(r.bar, x, y + 22);
    lv_bar_set_range(r.bar, 0, 100);
    lv_obj_set_style_radius(r.bar, 3, 0);
    lv_obj_set_style_radius(r.bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(0x2A2A3A), 0);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(fill), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return r;
}

// Left-column block: brand (small logo + name) on top, big "67%" middle, "REMAINING" bottom.
// All three are vertically stacked and horizontally centered around x=130 so they
// read as one unit instead of three floating pieces.
//   title_y_abs = absolute y of the brand title baseline anchor.
//   pct_y_abs   = absolute y of the big-% label top.
//   sub_y_abs   = absolute y of the REMAINING label top.
//   text_w      = approx pixel width of "Claude"/"Codex" wordmark, for centering the brand row.
//   returns the big-% label so callers can update it later.
static constexpr int LEFT_CENTER_X = 130;

static lv_obj_t* mk_dash_left_block(lv_obj_t* parent,
                                    const lv_image_dsc_t* logo,
                                    const char* name, uint32_t color,
                                    int text_w,
                                    int title_y_abs, int pct_y_abs, int sub_y_abs) {
    // Brand row centered at x=LEFT_CENTER_X. Logo widget bbox is 48 but image is
    // scaled to ~22 around the widget center (default pivot), so visible image
    // spans widget_x+13 … widget_x+35. We work backward from that to lay out the row.
    constexpr int LOGO_VIS = 22;
    constexpr int GAP      = 6;
    int block_w  = LOGO_VIS + GAP + text_w;
    int block_x  = LEFT_CENTER_X - block_w / 2;       // visible left edge of brand row
    int logo_widget_x = block_x - 13;
    int title_x       = block_x + LOGO_VIS + GAP;
    int logo_widget_y = title_y_abs - 14;             // image-center y matches text cap-center y

    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, logo);
    lv_image_set_scale(img, 117);                     // 48 * 117/256 ≈ 22
    lv_obj_set_pos(img, logo_widget_x, logo_widget_y);

    lv_obj_t* t = lv_label_create(parent);
    lv_label_set_text(t, name);
    lv_obj_set_style_text_color(t, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(t, title_x, title_y_abs);

    lv_obj_t* big = lv_label_create(parent);
    lv_label_set_text(big, "--%");
    lv_obj_set_style_text_color(big, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_48, 0);
    lv_obj_set_width(big, 160);
    lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(big, LEFT_CENTER_X - 80, pct_y_abs);

    lv_obj_t* sub = lv_label_create(parent);
    lv_label_set_text(sub, "REMAINING");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_set_width(sub, 160);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sub, LEFT_CENTER_X - 80, sub_y_abs);
    return big;
}

// ================== Pet screens (3 of them, swipe-able) ==================
// Each pet has its own screen + image widget. A single timer drives whichever
// pet's screen is currently active. The state machine (idle ~ random expression
// ~ idle) is pet-agnostic — frame data is looked up via PETS[s_pet_active].

enum PetState { P_IDLE = 0, P_WAVE, P_FAIL, P_WAIT, P_REVIEW };

static lv_timer_t* s_pet_timer        = nullptr;
static PetState s_pet_state           = P_IDLE;
static uint8_t s_pet_frame            = 0;
// Most of the time we sit in P_IDLE; every N idle cycles a single expression
// (wait/fail/review) is mixed in. Carries across pet swaps so behaviour stays
// consistent if you flip between pets quickly.
static uint8_t s_pet_idle_cycles_left = 0;
static bool s_pet_recording           = false;

static const lv_image_dsc_t* const* pet_anim_frames(const PetSpec& p, PetState s) {
    switch (s) {
        case P_WAVE:   return p.wave_frames;
        case P_FAIL:   return p.fail_frames;
        case P_WAIT:   return p.wait_frames;
        case P_REVIEW: return p.review_frames;
        case P_IDLE:
        default:       return p.idle_frames;
    }
}
static const uint32_t* pet_anim_durs(const PetSpec& p, PetState s) {
    switch (s) {
        case P_WAVE:   return p.wave_durs;
        case P_FAIL:   return p.fail_durs;
        case P_WAIT:   return p.wait_durs;
        case P_REVIEW: return p.review_durs;
        case P_IDLE:
        default:       return p.idle_durs;
    }
}
static int pet_anim_count(const PetSpec& p, PetState s) {
    switch (s) {
        case P_WAVE:   return p.wave_count;
        case P_FAIL:   return p.fail_count;
        case P_WAIT:   return p.wait_count;
        case P_REVIEW: return p.review_count;
        case P_IDLE:
        default:       return p.idle_count;
    }
}

static PetState pet_random_expression(void) {
    static const PetState pool[] = { P_WAIT, P_FAIL, P_REVIEW };
    return pool[esp_random() % 3];
}
// 2–5 idle cycles between expressions, ~7–17s with current durations.
static uint8_t pet_pick_idle_run(void) { return 2 + (esp_random() % 4); }

static void pet_timer_cb(lv_timer_t* /*t*/) {
    const int i = s_pet_active;
    if (i < 0 || i >= NUM_PETS || !s_pet_imgs[i]) return;
    const PetSpec& spec = PETS[i];
    s_pet_frame++;
    if (s_pet_frame >= pet_anim_count(spec, s_pet_state)) {
        s_pet_frame = 0;
        if (s_pet_recording) {
            s_pet_state = P_WAVE;
        } else if (s_pet_state == P_IDLE) {
            if (s_pet_idle_cycles_left > 1) {
                s_pet_idle_cycles_left--;
            } else {
                s_pet_idle_cycles_left = 0;
                s_pet_state = pet_random_expression();
            }
        } else {
            // Expression cycle done — back to idle for a while.
            s_pet_state = P_IDLE;
            s_pet_idle_cycles_left = pet_pick_idle_run();
        }
    }
    lv_image_set_src(s_pet_imgs[i], pet_anim_frames(spec, s_pet_state)[s_pet_frame]);
    lv_timer_set_period(s_pet_timer, pet_anim_durs(spec, s_pet_state)[s_pet_frame]);
}

static void pet_set_recording_locked(bool recording) {
    s_pet_recording = recording;
    const int i = s_pet_active;
    if (recording && i >= 0 && i < NUM_PETS) {
        s_pet_state = P_WAVE;
        s_pet_frame = 0;
        if (s_pet_imgs[i]) lv_image_set_src(s_pet_imgs[i], PETS[i].wave_frames[0]);
        if (s_pet_timer)   lv_timer_set_period(s_pet_timer, PETS[i].wave_durs[0]);
    } else if (!recording) {
        // Wave finishes naturally; timer_cb's "expression done" branch returns to idle.
        s_pet_idle_cycles_left = pet_pick_idle_run();
    }
}

// When the user swipes between pets, snap the new pet's image to a fresh idle
// frame so it doesn't start mid-expression. The timer keeps running and will
// pick up the new pet on its next tick.
static void pet_switch_to_locked(int new_idx) {
    if (new_idx < 0 || new_idx >= NUM_PETS) return;
    s_pet_active = new_idx;
    s_pet_state = P_IDLE;
    s_pet_frame = 0;
    s_pet_idle_cycles_left = pet_pick_idle_run();
    if (s_pet_imgs[new_idx]) lv_image_set_src(s_pet_imgs[new_idx],
                                              PETS[new_idx].idle_frames[0]);
    if (s_pet_timer) lv_timer_set_period(s_pet_timer, PETS[new_idx].idle_durs[0]);
}

static void build_one_pet_screen(int idx) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_pets[idx] = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const PetSpec& spec = PETS[idx];
    lv_obj_t* img = lv_image_create(scr);
    s_pet_imgs[idx] = img;
    lv_image_set_src(img, spec.idle_frames[0]);
    // Source frames are 96×104; display at scale 2× for crisp, jitter-free
    // playback on the S3. AA off (nearest-neighbor) — bilinear+alpha was too
    // heavy and caused drops.
    lv_image_set_scale(img, 512);
    lv_image_set_antialias(img, false);
    lv_obj_set_pos(img, (468 - spec.frame_w) / 2, (466 - spec.frame_h) / 2);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

static void build_pet_screens(void) {
    for (int i = 0; i < NUM_PETS; ++i) build_one_pet_screen(i);

    s_pet_active = 0;
    s_pet_state = P_IDLE;
    s_pet_idle_cycles_left = pet_pick_idle_run();

    if (!s_pet_timer) {
        s_pet_timer = lv_timer_create(pet_timer_cb,
                                      PETS[0].idle_durs[0], nullptr);
    }
}

static void build_dashboard_screen(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr_dashboard = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Right column kept narrow so "reset 6/11 10:39" never grazes the bezel.
    // At y≈110 the circle visible x runs from ~35 to ~430; right edge 390 leaves 40px margin.
    constexpr int COMPACT_X = 250;
    constexpr int COMPACT_W = 140;

    // ========== TOP HALF: CLAUDE ==========
    // Top content slid down a touch, bottom slid up — gap between halves shrinks
    // from ~67px to ~40px so the two read more like one dashboard, less like two cards.
    s_dash_claude_big = mk_dash_left_block(scr, &logo_claude, "Claude", 0xFF6B35,
                                            68, 80, 114, 176);

    DashBarRow tok = mk_dash_bar_row(scr, COMPACT_X, 114, COMPACT_W, 0xFF6B35, "Token");
    s_dash_claude_token_bar = tok.bar;
    s_dash_claude_token_rst = tok.reset;
    lv_label_set_text(tok.reset, "reset 00:00");

    lv_obj_t* row2_lbl = lv_label_create(scr);
    lv_label_set_text(row2_lbl, "Today");
    lv_obj_set_style_text_color(row2_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(row2_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(row2_lbl, COMPACT_X, 164);

    s_dash_claude_cost = lv_label_create(scr);
    lv_label_set_text(s_dash_claude_cost, "$--");
    lv_obj_set_style_text_color(s_dash_claude_cost, lv_color_hex(0xFF6B35), 0);
    lv_obj_set_style_text_font(s_dash_claude_cost, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_dash_claude_cost, COMPACT_W);
    lv_obj_set_style_text_align(s_dash_claude_cost, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_dash_claude_cost, COMPACT_X, 160);

    // ========== BOTTOM HALF: CODEX ==========
    s_dash_codex_big = mk_dash_left_block(scr, &logo_codex, "Codex", 0x25C7FF,
                                           60, 236, 270, 332);

    DashBarRow d5 = mk_dash_bar_row(scr, COMPACT_X, 270, COMPACT_W, 0x25C7FF, "5h");
    s_dash_codex_5h_bar = d5.bar;
    s_dash_codex_5h_rst = d5.reset;

    DashBarRow d7 = mk_dash_bar_row(scr, COMPACT_X, 320, COMPACT_W, 0x25C7FF, "7d");
    s_dash_codex_7d_bar = d7.bar;
    s_dash_codex_7d_rst = d7.reset;

    // Bottom clock.
    s_dash_clock = lv_label_create(scr);
    lv_label_set_text(s_dash_clock, "--:--");
    lv_obj_set_style_text_color(s_dash_clock, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_dash_clock, &lv_font_montserrat_18, 0);
    lv_obj_align(s_dash_clock, LV_ALIGN_BOTTOM_MID, 0, -16);
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
    build_dashboard_screen();
    build_pet_screens();
    build_setup_screen();
    s_last_main = s_scr_claude;
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
        pet_set_recording_locked(on);
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
    // Compact: strip "claude-" prefix, replace last '-' with '.'.
    char m[18];
    const char* src = model;
    if (strncmp(src, "claude-", 7) == 0) src += 7;
    size_t i = 0;
    for (; i < sizeof(m) - 1 && src[i]; ++i) m[i] = src[i];
    m[i] = 0;
    char* last_dash = strrchr(m, '-');
    if (last_dash) *last_dash = '.';
    if (lvgl_lock()) {
        lv_label_set_text(s_model_value, m);
        lvgl_unlock();
    }
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

void ui_set_claude_metrics(int tokens_out, float cost_usd, bool busy) {
    // tokens / cost live in the TODAY / TOKENS rows (driven by ui_set_claude_summary).
    // Here we update the STATUS arc text + give the arc a subtle BUSY/IDLE fill.
    (void)tokens_out;
    (void)cost_usd;
    if (!lvgl_lock()) return;
    if (s_mode_value)      lv_label_set_text(s_mode_value, busy ? "BUSY" : "IDLE");
    if (s_claude_mode_arc) lv_arc_set_value(s_claude_mode_arc, busy ? 100 : 30);
    lvgl_unlock();
}

// Format the quota row in HTML-preview style:
//   "5H" big label | bar fills with USED% | big "67%" right | "reset HH:MM" above bar
static void update_quota_row_locked(lv_obj_t* label_w, lv_obj_t* pct_w,
                                    lv_obj_t* reset_w, lv_obj_t* bar_w,
                                    const char* label_prefix, int used_pct,
                                    const char* reset_abs) {
    char b[16];
    if (label_w) lv_label_set_text(label_w, label_prefix);
    if (pct_w) {
        if (used_pct < 0) snprintf(b, sizeof(b), "--%%");
        else              snprintf(b, sizeof(b), "%d%%", used_pct);
        lv_label_set_text(pct_w, b);
    }
    if (reset_w) {
        if (reset_abs && reset_abs[0]) snprintf(b, sizeof(b), "reset %s", reset_abs);
        else                            snprintf(b, sizeof(b), "reset --");
        lv_label_set_text(reset_w, b);
    }
    if (bar_w) {
        lv_bar_set_value(bar_w, used_pct < 0 ? 0 : used_pct, LV_ANIM_OFF);
    }
}

// Stub — Claude page no longer renders 5h/7d bars (API-key users have no quota).
// Server still pushes the fields, but we ignore them. If user later switches to OAuth,
// the bars can come back; for now today's cost+tokens carry the same info.
void ui_set_claude_quota(int five_h_used_pct, const char* five_h_reset_abs,
                         int seven_d_used_pct, const char* seven_d_reset_abs) {
    (void)five_h_used_pct;  (void)five_h_reset_abs;
    (void)seven_d_used_pct; (void)seven_d_reset_abs;
}

// Stat row constants — mirror what mk_stat_row uses, needed here for dynamic leader sizing.
static constexpr int STAT_LEADER_X = 130;
static constexpr int STAT_VALUE_X  = 312;
static constexpr int STAT_VALUE_W  = 100;

// Set a stat row's value text AND shrink/grow the leader line so it ends just before
// the value text actually starts (otherwise short values like "$3.50" leave a big
// gap on the right while the line abuts the label on the left — looks unbalanced).
static void set_stat_value_locked(lv_obj_t* leader, lv_obj_t* value, const char* text) {
    if (!value || !text) return;
    lv_label_set_text(value, text);
    if (!leader) return;
    lv_point_t sz;
    lv_text_get_size(&sz, text, &lv_font_montserrat_24, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int visible_left = STAT_VALUE_X + STAT_VALUE_W - sz.x;
    int new_w = visible_left - STAT_LEADER_X - 10;   // 10px gap before value
    if (new_w < 20) new_w = 20;
    lv_obj_set_width(leader, new_w);
}

// Cost + tokens go in the TODAY / TOKENS rows on the Claude main page.
// They also feed the Claude half of the dashboard (big %, token bar, cost text).
void ui_set_claude_summary(float today_cost_usd, int today_tokens, const char* hhmm) {
    char cost[16], tok[16];
    snprintf(cost, sizeof(cost), "$%.2f", today_cost_usd);
    if (today_tokens >= 1000000)      snprintf(tok, sizeof(tok), "%.1fM", today_tokens / 1e6);
    else if (today_tokens >= 1000)    snprintf(tok, sizeof(tok), "%.1fK", today_tokens / 1e3);
    else                              snprintf(tok, sizeof(tok), "%d", today_tokens);

    // Dashboard derived values.
    int used_pct = today_tokens > 0
                   ? (int)((int64_t)today_tokens * 100 / CLAUDE_DAILY_TOKEN_CAP)
                   : 0;
    if (used_pct > 100) used_pct = 100;
    int remain_pct = 100 - used_pct;
    char big_buf[8];
    snprintf(big_buf, sizeof(big_buf), "%d%%", remain_pct);

    if (!lvgl_lock()) return;
    // Main page widgets.
    set_stat_value_locked(s_claude_today_leader,  s_claude_today_value,  cost);
    set_stat_value_locked(s_claude_tokens_leader, s_claude_tokens_value, tok);
    if (s_claude_summary) lv_label_set_text(s_claude_summary,
                                            (hhmm && hhmm[0]) ? hhmm : "--:--");
    // Dashboard widgets.
    if (s_dash_claude_big)       lv_label_set_text(s_dash_claude_big, big_buf);
    if (s_dash_claude_token_bar) lv_bar_set_value(s_dash_claude_token_bar, used_pct, LV_ANIM_OFF);
    if (s_dash_claude_cost)      lv_label_set_text(s_dash_claude_cost, cost);
    if (s_dash_clock)            lv_label_set_text(s_dash_clock,
                                                   (hhmm && hhmm[0]) ? hhmm : "--:--");
    lvgl_unlock();
}

void ui_set_todos_progress(int done, int total, const char* current, const char* recent_done) {
    (void)current; (void)recent_done;
    if (!s_todos_label) return;
    if (!lvgl_lock()) return;
    char b[32];
    snprintf(b, sizeof(b), "TodoList  %d / %d", done, total);
    lv_label_set_text(s_todos_label, b);
    lv_obj_clear_flag(s_todos_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_unlock();
}

void ui_set_codex_limits(int five_hour_left_pct, const char* five_hour_reset_abs,
                         int week_left_pct, const char* week_reset_abs) {
    if (!lvgl_lock()) return;
    int used_5h = (five_hour_left_pct >= 0 && five_hour_left_pct <= 100)
                  ? (100 - five_hour_left_pct) : -1;
    int used_wk = (week_left_pct >= 0 && week_left_pct <= 100)
                  ? (100 - week_left_pct) : -1;
    update_quota_row_locked(s_codex_5h_label, s_codex_5h_pct,
                            s_codex_5h_reset, s_codex_5h_bar,
                            "5H", used_5h, five_hour_reset_abs);
    update_quota_row_locked(s_codex_7d_label, s_codex_7d_pct,
                            s_codex_7d_reset, s_codex_7d_bar,
                            "7D", used_wk, week_reset_abs);
    // Arcs mirror the bars (fill = used%). Effort arc is overridden by
    // ui_set_codex_effort_arc when a live reasoning level arrives.
    int u5 = (used_5h >= 0) ? used_5h : 0;
    int u7 = (used_wk >= 0) ? used_wk : 0;
    if (s_codex_model_arc)  lv_arc_set_value(s_codex_model_arc,  u5);
    if (s_codex_effort_arc) lv_arc_set_value(s_codex_effort_arc, u7);

    // Dashboard Codex half.
    if (s_dash_codex_big) {
        char b[16];
        int p = five_hour_left_pct;
        if (p < 0)        snprintf(b, sizeof(b), "--%%");
        else if (p > 100) snprintf(b, sizeof(b), "100%%");
        else              snprintf(b, sizeof(b), "%d%%", p);
        lv_label_set_text(s_dash_codex_big, b);
    }
    if (s_dash_codex_5h_bar) lv_bar_set_value(s_dash_codex_5h_bar, u5, LV_ANIM_OFF);
    if (s_dash_codex_7d_bar) lv_bar_set_value(s_dash_codex_7d_bar, u7, LV_ANIM_OFF);
    if (s_dash_codex_5h_rst) {
        char b[20];
        snprintf(b, sizeof(b), "reset %s",
                 (five_hour_reset_abs && five_hour_reset_abs[0]) ? five_hour_reset_abs : "--");
        lv_label_set_text(s_dash_codex_5h_rst, b);
    }
    if (s_dash_codex_7d_rst) {
        char b[24];
        snprintf(b, sizeof(b), "reset %s",
                 (week_reset_abs && week_reset_abs[0]) ? week_reset_abs : "--");
        lv_label_set_text(s_dash_codex_7d_rst, b);
    }
    lvgl_unlock();
}

void ui_set_transcript(const char* text) {
    if (!s_transcript_label || !text) return;
    if (lvgl_lock()) {
        lv_label_set_text(s_transcript_label, text);
        lvgl_unlock();
    }
}

void ui_set_codex_effort_arc(int effort_pct) {
    if (effort_pct < 0) effort_pct = 0;
    if (effort_pct > 100) effort_pct = 100;
    if (!lvgl_lock()) return;
    if (s_codex_effort_arc) lv_arc_set_value(s_codex_effort_arc, effort_pct);
    lvgl_unlock();
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
    (void)today_pct; (void)week_pct;  // no caps make % meaningless here
    if (!lvgl_lock()) return;
    char b[24];
    if (s_codex_today_value) {
        snprintf(b, sizeof(b), "DAY  %d m", today_min);
        lv_label_set_text(s_codex_today_value, b);
    }
    if (s_codex_actweek_value) {
        snprintf(b, sizeof(b), "WK  %d m", week_min);
        lv_label_set_text(s_codex_actweek_value, b);
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

// Returns the index in s_scr_pets[] of the given screen, or -1 if not a pet screen.
static int pet_screen_index(lv_obj_t* scr) {
    for (int i = 0; i < NUM_PETS; ++i) if (scr == s_scr_pets[i]) return i;
    return -1;
}

static void gesture_cb(lv_event_t* e) {
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    lv_obj_t* cur = lv_screen_active();
    const int pet_idx = pet_screen_index(cur);
    const bool on_main = (cur == s_scr_claude || cur == s_scr_codex);

    if (dir == LV_DIR_LEFT && cur == s_scr_claude && s_scr_codex) {
        lv_screen_load_anim(s_scr_codex, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
    } else if (dir == LV_DIR_RIGHT && cur == s_scr_codex && s_scr_claude) {
        lv_screen_load_anim(s_scr_claude, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    } else if (dir == LV_DIR_TOP && on_main && s_scr_dashboard) {
        s_last_main = cur;
        lv_screen_load_anim(s_scr_dashboard, LV_SCR_LOAD_ANIM_MOVE_TOP, 250, 0, false);
    } else if (dir == LV_DIR_BOTTOM && cur == s_scr_dashboard && s_last_main) {
        lv_screen_load_anim(s_last_main, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 250, 0, false);
    } else if (dir == LV_DIR_BOTTOM && on_main && s_scr_pets[s_pet_active]) {
        // Down-swipe from a main → currently-active pet (preserves last visited pet).
        s_last_main = cur;
        lv_screen_load_anim(s_scr_pets[s_pet_active],
                            LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 250, 0, false);
    } else if (dir == LV_DIR_TOP && pet_idx >= 0 && s_last_main) {
        lv_screen_load_anim(s_last_main, LV_SCR_LOAD_ANIM_MOVE_TOP, 250, 0, false);
    } else if (dir == LV_DIR_LEFT && pet_idx >= 0 && pet_idx < NUM_PETS - 1) {
        pet_switch_to_locked(pet_idx + 1);
        lv_screen_load_anim(s_scr_pets[pet_idx + 1],
                            LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
    } else if (dir == LV_DIR_RIGHT && pet_idx > 0) {
        pet_switch_to_locked(pet_idx - 1);
        lv_screen_load_anim(s_scr_pets[pet_idx - 1],
                            LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
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

    // Register gesture handler on every nav screen.
    if (lvgl_lock()) {
        if (s_scr_claude)    lv_obj_add_event_cb(s_scr_claude,    gesture_cb, LV_EVENT_GESTURE, nullptr);
        if (s_scr_codex)     lv_obj_add_event_cb(s_scr_codex,     gesture_cb, LV_EVENT_GESTURE, nullptr);
        if (s_scr_dashboard) lv_obj_add_event_cb(s_scr_dashboard, gesture_cb, LV_EVENT_GESTURE, nullptr);
        for (int i = 0; i < NUM_PETS; ++i) {
            if (s_scr_pets[i]) lv_obj_add_event_cb(s_scr_pets[i], gesture_cb, LV_EVENT_GESTURE, nullptr);
        }
        lvgl_unlock();
    }
}
