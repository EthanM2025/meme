// Procedural two-note chime player. Pushes int16 sine samples (with a
// fast exponential-decay envelope so each note sounds plucky, not buzzy)
// into the already-open ES8311 codec via esp_codec_dev_write.
//
// Each chime runs in a dedicated FreeRTOS task that pops from a 4-deep
// queue; the public play_*() functions just enqueue and return so they
// can be called from the WS receive callback without blocking.

#include "chime.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

static const char* TAG = "chime";

enum ChimeKind { CHIME_CLAUDE = 0, CHIME_CODEX };

struct Tone { float freq_hz; int duration_ms; };

// Two short notes per chime — same pitches, opposite directions, so the two
// agents are recognizably related but distinct ("ding-dong" vs "dong-ding").
//   E5 = 659 Hz, B5 = 988 Hz (a perfect 5th apart, pleasant interval).
// One tone per agent — Claude = higher (A5, 880 Hz), Codex = lower (E5, 660 Hz).
// Same duration so they have the same "weight"; pitch is the only distinguisher.
static const Tone CLAUDE_TONES[] = { { 880.0f, 450 } };
static const Tone CODEX_TONES[]  = { { 660.0f, 450 } };

static esp_codec_dev_handle_t s_codec       = nullptr;
static bool (*s_is_recording)(void)         = nullptr;
static void (*s_pa_setter)(bool)            = nullptr;
static QueueHandle_t s_chime_queue          = nullptr;

static constexpr int SAMPLE_RATE = 16000;     // matches main.cpp's codec open
static constexpr int CHUNK_SAMPS = 256;

// Play one tone with an exponential decay envelope (~3τ over its duration).
static void play_one_tone(const Tone& t) {
    const int total = SAMPLE_RATE * t.duration_ms / 1000;
    const float w   = 2.0f * (float)M_PI * t.freq_hz / SAMPLE_RATE;
    // Big amplitude — the 1W AW8737A is small and ambient room noise easily masks
    // a quieter signal. Sine + envelope keeps it from clipping.
    constexpr float AMP = 22000.0f;
    int16_t buf[CHUNK_SAMPS];
    float phase = 0.0f;
    int written = 0;
    while (written < total) {
        int chunk = total - written;
        if (chunk > CHUNK_SAMPS) chunk = CHUNK_SAMPS;
        for (int k = 0; k < chunk; ++k) {
            int pos = written + k;
            // Decay slower (~1.5τ over duration) so the note rings audibly to the end.
            float env = expf(-1.5f * (float)pos / (float)total);
            // Tiny attack ramp (~6 ms) to avoid a click on note onset.
            const int attack = SAMPLE_RATE * 6 / 1000;
            if (pos < attack) env *= (float)pos / (float)attack;
            buf[k] = (int16_t)(AMP * env * sinf(phase));
            phase += w;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        if (esp_codec_dev_write(s_codec, buf, chunk * (int)sizeof(int16_t)) != 0) {
            // codec write failed — bail rather than spin
            return;
        }
        written += chunk;
    }
}

// Write zeros for `ms` milliseconds. Used as a preamble/postamble around the
// chime so the DAC doesn't latch a non-zero value (which clicks when the PA
// drops out) and so the I2S TX has buffered silence before the first sample.
static void play_silence(int ms) {
    static const int16_t zeros[CHUNK_SAMPS] = {};
    int total = SAMPLE_RATE * ms / 1000;
    int written = 0;
    while (written < total) {
        int chunk = total - written;
        if (chunk > CHUNK_SAMPS) chunk = CHUNK_SAMPS;
        esp_codec_dev_write(s_codec, (void*)zeros, chunk * (int)sizeof(int16_t));
        written += chunk;
    }
}

static void chime_task(void* /*arg*/) {
    ChimeKind kind;
    while (true) {
        if (xQueueReceive(s_chime_queue, &kind, portMAX_DELAY) != pdTRUE) continue;
        if (!s_codec) { ESP_LOGW(TAG, "no codec, skip"); continue; }
        if (s_is_recording && s_is_recording()) {
            ESP_LOGI(TAG, "skip chime: recording in progress");
            continue;
        }
        ESP_LOGI(TAG, "play chime: %s", kind == CHIME_CLAUDE ? "claude" : "codex");
        const Tone* tones; int n;
        if (kind == CHIME_CLAUDE) { tones = CLAUDE_TONES; n = sizeof(CLAUDE_TONES)/sizeof(Tone); }
        else                       { tones = CODEX_TONES;  n = sizeof(CODEX_TONES)/sizeof(Tone); }
        // Power on AW8737A just for this chime, settle, then play.
        if (s_pa_setter) s_pa_setter(true);
        vTaskDelay(pdMS_TO_TICKS(15));
        play_silence(15);
        for (int i = 0; i < n; ++i) {
            play_one_tone(tones[i]);
            if (i + 1 < n) play_silence(20);
        }
        play_silence(40);                              // settle DAC to 0 before PA drops
        if (s_pa_setter) s_pa_setter(false);           // PA off — kills the idle hiss/clicks
        ESP_LOGI(TAG, "chime done");
    }
}

void chime_init(esp_codec_dev_handle_t codec_dev,
                bool (*recording_getter)(void),
                void (*pa_setter)(bool on)) {
    s_codec = codec_dev;
    s_is_recording = recording_getter;
    s_pa_setter = pa_setter;
    if (!s_chime_queue) {
        // Queue depth 1 — overlapping triggers should NOT stack into a "long
        // beeping" sound. Latest wins; older drops on enqueue.
        s_chime_queue = xQueueCreate(1, sizeof(ChimeKind));
        xTaskCreate(chime_task, "chime", 4096, nullptr, 4, nullptr);
    }
}

static void enqueue(ChimeKind k) {
    if (!s_chime_queue) return;
    // Drop if queue is full — avoids backing up on rapid completion events.
    xQueueSend(s_chime_queue, &k, 0);
}

void chime_play_claude_done(void) { enqueue(CHIME_CLAUDE); }
void chime_play_codex_done(void)  { enqueue(CHIME_CODEX);  }
