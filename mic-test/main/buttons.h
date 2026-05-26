#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Button A (GPIO 2 - the larger one) / Button B (GPIO 1 - the smaller one).
// Both pulled up; reading 0 = pressed.

void buttons_init(void);
bool button_a_pressed(void);
bool button_b_pressed(void);
// Edge events (return true once on the rising edge).
bool button_a_just_pressed(void);
bool button_a_just_released(void);
bool button_b_just_pressed(void);
bool button_b_just_released(void);

#ifdef __cplusplus
}
#endif
