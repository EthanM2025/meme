#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t codex_pet_idle_0;
extern const lv_image_dsc_t codex_pet_idle_1;
extern const lv_image_dsc_t codex_pet_idle_2;
extern const lv_image_dsc_t codex_pet_idle_3;
extern const lv_image_dsc_t codex_pet_idle_4;
extern const lv_image_dsc_t codex_pet_idle_5;
extern const lv_image_dsc_t* const codex_pet_idle_frames[6];
extern const uint32_t codex_pet_idle_durations_ms[6];

extern const lv_image_dsc_t codex_pet_typing_0;
extern const lv_image_dsc_t codex_pet_typing_1;
extern const lv_image_dsc_t codex_pet_typing_2;
extern const lv_image_dsc_t codex_pet_typing_3;
extern const lv_image_dsc_t codex_pet_typing_4;
extern const lv_image_dsc_t codex_pet_typing_5;
extern const lv_image_dsc_t* const codex_pet_typing_frames[6];
extern const uint32_t codex_pet_typing_durations_ms[6];

#define CODEX_PET_IDLE_FRAME_COUNT 6
#define CODEX_PET_TYPING_FRAME_COUNT 6
#define CODEX_PET_FRAME_W 96
#define CODEX_PET_FRAME_H 104
#define CODEX_PET_FRAME_STRIDE 192

#ifdef __cplusplus
}
#endif
