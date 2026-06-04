#pragma once
#include "esp_codec_dev_vol.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wire the chime player to the already-opened ES8311 dev + a getter for
// whether the device is currently capturing audio (we skip chimes during
// recording so the mic doesn't pick up the tone we just played).
// pa_setter is called with `true` before playback and `false` after, so the
// caller can toggle the speaker power amp (AW8737A) — keeping it ON between
// chimes causes audible hiss/clicking from I2S TX DMA looping.
void chime_init(esp_codec_dev_handle_t codec_dev,
                bool (*recording_getter)(void),
                void (*pa_setter)(bool on));

// Non-blocking — enqueues a chime to play in the dedicated task.
// Safe to call from any task (e.g. the WS receive task).
void chime_play_claude_done(void);
void chime_play_codex_done(void);

#ifdef __cplusplus
}
#endif
