#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIANA_PET_FRAME_W 96
#define DIANA_PET_FRAME_H 104
#define DIANA_PET_FRAME_STRIDE 192

extern const lv_image_dsc_t diana_pet_idle_0;
extern const lv_image_dsc_t diana_pet_idle_1;
extern const lv_image_dsc_t diana_pet_idle_2;
extern const lv_image_dsc_t diana_pet_idle_3;
extern const lv_image_dsc_t diana_pet_idle_4;
extern const lv_image_dsc_t diana_pet_idle_5;
extern const lv_image_dsc_t* const diana_pet_idle_frames[6];
extern const uint32_t diana_pet_idle_durations_ms[6];
#define DIANA_PET_IDLE_FRAME_COUNT 6

extern const lv_image_dsc_t diana_pet_wave_0;
extern const lv_image_dsc_t diana_pet_wave_1;
extern const lv_image_dsc_t diana_pet_wave_2;
extern const lv_image_dsc_t diana_pet_wave_3;
extern const lv_image_dsc_t* const diana_pet_wave_frames[4];
extern const uint32_t diana_pet_wave_durations_ms[4];
#define DIANA_PET_WAVE_FRAME_COUNT 4

extern const lv_image_dsc_t diana_pet_fail_0;
extern const lv_image_dsc_t diana_pet_fail_1;
extern const lv_image_dsc_t diana_pet_fail_2;
extern const lv_image_dsc_t diana_pet_fail_3;
extern const lv_image_dsc_t diana_pet_fail_4;
extern const lv_image_dsc_t diana_pet_fail_5;
extern const lv_image_dsc_t* const diana_pet_fail_frames[6];
extern const uint32_t diana_pet_fail_durations_ms[6];
#define DIANA_PET_FAIL_FRAME_COUNT 6

extern const lv_image_dsc_t diana_pet_wait_0;
extern const lv_image_dsc_t diana_pet_wait_1;
extern const lv_image_dsc_t diana_pet_wait_2;
extern const lv_image_dsc_t diana_pet_wait_3;
extern const lv_image_dsc_t diana_pet_wait_4;
extern const lv_image_dsc_t diana_pet_wait_5;
extern const lv_image_dsc_t* const diana_pet_wait_frames[6];
extern const uint32_t diana_pet_wait_durations_ms[6];
#define DIANA_PET_WAIT_FRAME_COUNT 6

extern const lv_image_dsc_t diana_pet_review_0;
extern const lv_image_dsc_t diana_pet_review_1;
extern const lv_image_dsc_t diana_pet_review_2;
extern const lv_image_dsc_t diana_pet_review_3;
extern const lv_image_dsc_t diana_pet_review_4;
extern const lv_image_dsc_t diana_pet_review_5;
extern const lv_image_dsc_t* const diana_pet_review_frames[6];
extern const uint32_t diana_pet_review_durations_ms[6];
#define DIANA_PET_REVIEW_FRAME_COUNT 6

#ifdef __cplusplus
}
#endif
