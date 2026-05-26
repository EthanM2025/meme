#pragma once
#include <lvgl.h>
#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t* const claude_pet_idle_frames[6];
extern const uint32_t claude_pet_idle_durations_ms[6];
extern const lv_image_dsc_t* const claude_pet_running_frames[6];
extern const uint32_t claude_pet_running_durations_ms[6];

#define CLAUDE_PET_IDLE_FRAME_COUNT 6
#define CLAUDE_PET_RUNNING_FRAME_COUNT 6
#define CLAUDE_PET_FRAME_W 96
#define CLAUDE_PET_FRAME_H 104
#define CLAUDE_PET_FRAME_STRIDE 192

#ifdef __cplusplus
}
#endif
