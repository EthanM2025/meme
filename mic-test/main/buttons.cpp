#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// From M5Unified PR #257 — StopWatch hardware mapping:
//   GPIO_NUM_2 → button A (large / top)
//   GPIO_NUM_1 → button B (small / bottom)
// Both wired with internal-or-external pull-up; reading low = pressed.

static const gpio_num_t PIN_A = GPIO_NUM_2;
static const gpio_num_t PIN_B = GPIO_NUM_1;

// Simple debounced state. Polled at ~100 Hz from a task.
static volatile bool s_a = false;
static volatile bool s_b = false;
static volatile bool s_a_press_edge = false;
static volatile bool s_a_release_edge = false;
static volatile bool s_b_press_edge = false;
static volatile bool s_b_release_edge = false;

static void poll_task(void*) {
    bool a_prev = false, b_prev = false;
    // Two-sample debounce.
    bool a_last = false, b_last = false;
    while (true) {
        bool a_now = (gpio_get_level(PIN_A) == 0);
        bool b_now = (gpio_get_level(PIN_B) == 0);
        bool a_stable = a_now && a_last;
        bool b_stable = b_now && b_last;
        // Released needs two consecutive HIGH reads too.
        if (!a_now && !a_last) a_stable = false;
        if (!b_now && !b_last) b_stable = false;
        // Update state only when both samples agree.
        if (a_now == a_last) {
            if (a_now != a_prev) {
                if (a_now) s_a_press_edge = true;
                else       s_a_release_edge = true;
                a_prev = a_now;
            }
            s_a = a_now;
        }
        if (b_now == b_last) {
            if (b_now != b_prev) {
                if (b_now) s_b_press_edge = true;
                else       s_b_release_edge = true;
                b_prev = b_now;
            }
            s_b = b_now;
        }
        a_last = a_now;
        b_last = b_now;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void buttons_init(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B);
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    xTaskCreate(poll_task, "btn", 2048, nullptr, 5, nullptr);
}

bool button_a_pressed(void) { return s_a; }
bool button_b_pressed(void) { return s_b; }

bool button_a_just_pressed(void)  { if (s_a_press_edge)   { s_a_press_edge = false;   return true; } return false; }
bool button_a_just_released(void) { if (s_a_release_edge) { s_a_release_edge = false; return true; } return false; }
bool button_b_just_pressed(void)  { if (s_b_press_edge)   { s_b_press_edge = false;   return true; } return false; }
bool button_b_just_released(void) { if (s_b_release_edge) { s_b_release_edge = false; return true; } return false; }
