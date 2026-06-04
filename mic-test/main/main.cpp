// Minimal mic test for M5Stack StopWatch.
// Goal: prove ES8311 + I2S read works. Prints RMS amplitude over serial.

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "chime.h"

extern "C" bool is_recording_getter(void);
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "i2c_bus.h"
#include "M5PM1.h"
#include "M5IOE1.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "wifi.h"
#include "ws_client.h"
#include "buttons.h"
#include "display.h"
#include "mdns.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

// Default Mac mDNS hostname — used only if no `host` has been saved via the
// SoftAP page. Users on a different Mac just set their hostname in the SoftAP
// form once and never have to reflash.
#define DEFAULT_MAC_HOST "Ethan-MacBook-Air.local"
#define MAC_WS_PORT      8765

static const char* TAG = "mic-test";

// Board pins (from M5StopWatch-UserDemo/main/hal/hal_audio.cpp)
static constexpr int I2C_SDA = 47;
static constexpr int I2C_SCL = 48;
static constexpr int I2S_MCLK = 18;
static constexpr int I2S_BCLK = 17;
static constexpr int I2S_WS   = 15;
static constexpr int I2S_DIN  = 16;
static constexpr int I2S_DOUT = 21;

static constexpr int SAMPLE_RATE = 16000;  // 16 kHz is what Doubao ASR wants
static constexpr int FRAME_SAMPLES = 320;  // 20 ms at 16 kHz
static constexpr int64_t IDLE_SLEEP_US = 120LL * 1000 * 1000;

// IOE pin mapping (from hal_ioe.cpp)
static constexpr m5ioe1_pin_t IOE_AU_EN  = M5IOE1_PIN_3;   // audio power enable
static constexpr m5ioe1_pin_t IOE_L3B_EN = M5IOE1_PIN_8;   // upstream rail confirm
static constexpr m5ioe1_pin_t IOE_TP_RST = M5IOE1_PIN_4;   // touch reset (active low)
static constexpr m5ioe1_pin_t IOE_SPK_PA = M5IOE1_PIN_10;  // speaker PA enable (per UserDemo hal_ioe)
static constexpr gpio_num_t   SPK_PA_PIN = GPIO_NUM_14;    // direct-GPIO half of the PA enable

static M5PM1  pmic;
static M5IOE1 ioe;

static i2c_bus_handle_t g_i2c_bus = nullptr;
static i2s_chan_handle_t g_rx_handle = nullptr;
static i2s_chan_handle_t g_tx_handle = nullptr;
static esp_codec_dev_handle_t g_codec_dev = nullptr;

static void init_i2c() {
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = I2C_SDA;
    cfg.scl_io_num = I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = 100000;
    g_i2c_bus = i2c_bus_create(I2C_NUM_0, &cfg);
    ESP_LOGI(TAG, "i2c bus created: %p", g_i2c_bus);
}

static void init_pmic() {
    auto* bus = i2c_bus_get_internal_bus_handle(g_i2c_bus);
    auto r = pmic.begin(bus, 0x6E);
    ESP_LOGI(TAG, "PMIC begin: %d", r);
    // Disable I2C idle sleep, disable watchdog, enable power rails (CHG|DCDC|LDO|LED).
    pmic.setI2cSleepTime(0);
    pmic.wdtSet(0);  // 0 = disable watchdog
    uint8_t pwr = M5PM1_PWR_CFG_CHG_EN | M5PM1_PWR_CFG_DCDC_EN | M5PM1_PWR_CFG_LDO_EN | M5PM1_PWR_CFG_LED_CTRL;
    pmic.setPowerConfig(pwr, pwr);  // (mask, value) — set all four bits to 1
    ESP_LOGI(TAG, "PMIC configured");
}

static void init_ioe() {
    auto* bus = i2c_bus_get_internal_bus_handle(g_i2c_bus);
    auto r = ioe.begin(bus, 0x4F);
    if (r != M5IOE1_OK) {
        ESP_LOGW(TAG, "IOE 0x4F failed (%d), trying 0x6F", r);
        r = ioe.begin(bus, 0x6F);
    }
    ESP_LOGI(TAG, "IOE begin: %d", r);
    ioe.setI2cSleepTime(0);

    ioe.pinMode(IOE_AU_EN,  OUTPUT);
    ioe.pinMode(IOE_L3B_EN, OUTPUT);
    ioe.pinMode(IOE_TP_RST, OUTPUT);
    ioe.pinMode(IOE_SPK_PA, OUTPUT);
    ioe.digitalWrite(IOE_AU_EN,  1);   // audio power on
    ioe.digitalWrite(IOE_L3B_EN, 1);   // upstream rail latch
    // SPK_PA must stay LOW here — UserDemo enables it AFTER the codec is open,
    // otherwise the AW8737A power amp can latch into a bad state.
    ioe.digitalWrite(IOE_SPK_PA, 0);
    gpio_set_direction(SPK_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SPK_PA_PIN, 0);
    // Pulse touch reset low → high (matches UserDemo hal_ioe ioe_tp_reset).
    ioe.digitalWrite(IOE_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe.digitalWrite(IOE_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // L3B_EN sometimes drops — retry until it sticks (matches UserDemo)
    for (int i = 0; i < 10; ++i) {
        vTaskDelay(pdMS_TO_TICKS(80));
        if (ioe.digitalRead(IOE_L3B_EN) == 1) break;
        ioe.digitalWrite(IOE_L3B_EN, 1);
        ESP_LOGW(TAG, "L3B_EN retry %d", i);
    }
    ESP_LOGI(TAG, "IOE configured (AU_EN=high, L3B_EN=high)");
}

static void init_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_tx_handle, &g_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_WS,
            .dout = (gpio_num_t)I2S_DOUT,
            .din  = (gpio_num_t)I2S_DIN,
            .invert_flags = {},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(g_rx_handle));
    ESP_LOGI(TAG, "I2S ready (16 kHz/16-bit/mono)");
}

static void init_codec() {
    auto* bus = i2c_bus_get_internal_bus_handle(g_i2c_bus);

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.rx_handle = g_rx_handle;
    i2s_cfg.tx_handle = g_tx_handle;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if     = ctrl_if;
    es_cfg.gpio_if     = gpio_if;
    es_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es_cfg.pa_pin      = GPIO_NUM_NC;
    es_cfg.pa_reverted = false;
    es_cfg.use_mclk    = true;
    const audio_codec_if_t* codec_if = es8311_codec_new(&es_cfg);

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if  = data_if;
    g_codec_dev = esp_codec_dev_new(&dev_cfg);

    esp_codec_dev_set_in_gain(g_codec_dev, 30.0);
    // Output volume defaults to 0 on this codec — without this, chime writes
    // succeed but no audible signal reaches the speaker.
    esp_codec_dev_set_out_vol(g_codec_dev, 100.0);
    esp_codec_dev_set_out_mute(g_codec_dev, false);

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = 1;
    fs.sample_rate     = SAMPLE_RATE;
    ESP_ERROR_CHECK(esp_codec_dev_open(g_codec_dev, &fs));
    ESP_LOGI(TAG, "ES8311 opened at 16 kHz mono, in_gain=30dB");
}

static bool s_recording = false;
static bool s_sleeping = false;
static int64_t s_last_activity_us = 0;
// Updated by battery_task every 10s. When plugged in, we keep the screen on
// indefinitely; only on battery do we idle-sleep after IDLE_SLEEP_US.
static volatile bool s_on_usb_power = false;
// 0 = no pending; otherwise = ms timestamp of the first press, waiting to see
// if a second press arrives within the double-click window (then it becomes a clear).
static uint32_t s_btn_b_pending_submit_ms = 0;

// Exposed to chime.cpp so it can skip playing while the mic is in use.
extern "C" bool is_recording_getter(void) { return s_recording; }

// Exposed to chime.cpp so it can power-cycle the AW8737A PA around each chime.
// Keeping the PA on between chimes amplifies idle hiss / DMA-loop residue, which
// sounds like continuous ticking.
extern "C" void speaker_pa_set(bool on) {
    ioe.digitalWrite(IOE_SPK_PA, on ? 1 : 0);
    gpio_set_level(SPK_PA_PIN, on ? 1 : 0);
}

static void note_activity() {
    s_last_activity_us = esp_timer_get_time();
}

static bool wake_if_sleeping() {
    if (!s_sleeping) return false;
    s_sleeping = false;
    ui_set_sleeping(false);
    note_activity();
    ESP_LOGI(TAG, "woke from idle sleep");
    return true;
}

static void maybe_enter_idle_sleep() {
    if (s_sleeping || s_recording || s_last_activity_us == 0) return;
    // Plugged in → never auto-sleep. Power isn't the constraint; staying lit
    // is more useful (watch on a desk, dashboard always visible).
    if (s_on_usb_power) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_activity_us >= IDLE_SLEEP_US) {
        s_sleeping = true;
        ui_set_sleeping(true);
        ESP_LOGI(TAG, "entered idle sleep after 120s inactivity");
    }
}

// OTA check task — once per boot, ~30s after WiFi is up, fetch the Mac's
// /firmware.bin endpoint. esp_https_ota compares the running image's version
// to the new image's header version (we stamp it with a build timestamp in
// the top-level CMakeLists.txt, so any rebuild → mismatch → update applies).
// No update needed → returns ESP_ERR_OTA_VALIDATE_FAILED ≈ NOOP.
static void ota_check_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(30000));  // first check 30s after boot, then every 5 min

    char host[64] = {0};
    if (!wifi_get_mac_host(host, sizeof(host)) || !host[0]) {
        strncpy(host, DEFAULT_MAC_HOST, sizeof(host) - 1);
    }
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/firmware.bin", host, MAC_WS_PORT);

    while (true) {
        esp_http_client_config_t http_cfg = {};
        http_cfg.url        = url;
        http_cfg.timeout_ms = 10000;
        http_cfg.keep_alive_enable = true;

        esp_https_ota_config_t ota_cfg = {};
        ota_cfg.http_config = &http_cfg;

        ESP_LOGI(TAG, "OTA check: %s", url);

        // Low-level path so we can compare versions ourselves — the default
        // `esp_https_ota()` refuses when versions match, but our CMake
        // version-stamping only updates on reconfigure, not every build.
        // We compare SHA-256 instead (different on any source change).
        esp_https_ota_handle_t handle = NULL;
        esp_err_t r = esp_https_ota_begin(&ota_cfg, &handle);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "OTA begin: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
            continue;
        }

        esp_app_desc_t new_desc;
        if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
            const esp_app_desc_t* running = esp_app_get_description();
            bool same = (memcmp(new_desc.app_elf_sha256, running->app_elf_sha256,
                                sizeof(new_desc.app_elf_sha256)) == 0);
            if (same) {
                ESP_LOGI(TAG, "OTA: same SHA-256, skipping");
                esp_https_ota_abort(handle);
                vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
                continue;
            }
            ESP_LOGI(TAG, "OTA: new image differs, applying");
        }

        while ((r = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            // download chunks
        }
        if (r == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
            if (esp_https_ota_finish(handle) == ESP_OK) {
                ESP_LOGI(TAG, "OTA: success, rebooting");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                ESP_LOGW(TAG, "OTA finish failed");
            }
        } else {
            ESP_LOGW(TAG, "OTA perform: %s", esp_err_to_name(r));
            esp_https_ota_abort(handle);
        }
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));  // 5 minutes
    }
}

// Confirm we boot OK after an OTA so the loader doesn't roll us back.
static void ota_mark_valid(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA: marking running image valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

// Battery poller — reads PMIC Vbat (battery voltage in mV) and Vin (USB input
// voltage in mV) every 10s, then pushes the percentage + charging state to
// the display. LiPo voltage curve approximated linearly between 3.30V (0%)
// and 4.20V (100%).
static void battery_task(void*) {
    while (true) {
        uint16_t vbat = 0, vin = 0;
        pmic.readVbat(&vbat);
        pmic.readVin(&vin);
        int pct = 0;
        if (vbat > 0) {
            int v = (int)vbat;
            pct = (v - 3300) * 100 / (4200 - 3300);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        // Vin > ~4500 mV means USB power is present → charging.
        bool charging = vin >= 4500;
        s_on_usb_power = charging;
        ui_set_battery(pct, charging);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void mic_task(void*) {
    static int16_t batch[FRAME_SAMPLES * 5];   // 5x20ms = 100ms per WS send
    int batch_idx = 0;
    int16_t buf[FRAME_SAMPLES];
    uint32_t print_count = 0;
    int32_t peak_recent = 0;
    while (true) {
        esp_err_t r = esp_codec_dev_read(g_codec_dev, buf, sizeof(buf));
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "read failed: %d", r);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // Button A push-to-talk: send start/end markers, stream PCM only while held.
        if (button_a_just_pressed()) {
            wake_if_sleeping();
            note_activity();
            ws_send_text("{\"type\":\"start\"}");
            s_recording = true;
            batch_idx = 0;
            ui_set_recording(true);
            ESP_LOGI(TAG, ">> recording start");
        }
        if (button_a_just_released()) {
            note_activity();
            if (!s_recording) {
                continue;
            }
            if (batch_idx > 0) {
                ws_send_pcm(batch, batch_idx);
                batch_idx = 0;
            }
            ws_send_text("{\"type\":\"end\"}");
            s_recording = false;
            ui_set_recording(false);
            ESP_LOGI(TAG, "<< recording stop");
        }
        // Button B: single press → submit (Enter), double press → clear (Cmd+A+Delete).
        // We delay sending "submit" by DOUBLE_WINDOW_MS so a follow-up press can override
        // it. Side effect: single Enter has ~300ms latency, which is barely noticeable.
        constexpr uint32_t DOUBLE_WINDOW_MS = 300;
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (button_b_just_pressed()) {
            wake_if_sleeping();
            note_activity();
            if (s_btn_b_pending_submit_ms != 0
                && (now_ms - s_btn_b_pending_submit_ms) <= DOUBLE_WINDOW_MS) {
                // Second press within the window — cancel pending submit, send clear.
                s_btn_b_pending_submit_ms = 0;
                ws_send_text("{\"type\":\"clear\"}");
                ESP_LOGI(TAG, "B double -> clear");
            } else {
                // First press — arm a delayed submit. Fires below if no second press.
                s_btn_b_pending_submit_ms = now_ms;
            }
        }
        if (button_b_just_released()) {
            note_activity();
        }
        if (s_btn_b_pending_submit_ms != 0
            && (now_ms - s_btn_b_pending_submit_ms) > DOUBLE_WINDOW_MS) {
            s_btn_b_pending_submit_ms = 0;
            ws_send_text("{\"type\":\"submit\"}");
            ESP_LOGI(TAG, "B single -> submit");
        }
        if (button_a_pressed() || button_b_pressed()) {
            note_activity();
        }
        if (ui_consume_touch_activity()) {
            if (s_sleeping) {
                wake_if_sleeping();
                continue;
            }
            note_activity();
        }
        if (!s_recording) {
            // Idle: don't stream PCM. Keep looping so button polls + read drain run.
            maybe_enter_idle_sleep();
            continue;
        }
        memcpy(&batch[batch_idx], buf, sizeof(buf));
        batch_idx += FRAME_SAMPLES;
        if (batch_idx >= (int)(sizeof(batch) / sizeof(int16_t))) {
            ws_send_pcm(batch, batch_idx);
            batch_idx = 0;
        }
        // RMS over the 20 ms frame.
        uint64_t sumsq = 0;
        int32_t peak = 0;
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            int32_t s = buf[i];
            sumsq += (uint64_t)(s * s);
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        int rms = (int)sqrt((double)sumsq / FRAME_SAMPLES);
        if (peak > peak_recent) peak_recent = peak;

        // Print once every ~200 ms — 10 frames per 20 ms = enough cadence.
        if (++print_count % 10 == 0) {
            // Bar based on RMS amplitude (0..32767 → 0..40 chars).
            int bars = rms * 40 / 2000;  // saturate around RMS ~2000 — tuned for in_gain=30dB ambient + speech
            if (bars > 40) bars = 40;
            char bar[41];
            for (int i = 0; i < bars; ++i) bar[i] = '#';
            for (int i = bars; i < 40; ++i) bar[i] = '.';
            bar[40] = 0;
            ESP_LOGI(TAG, "rms=%5d peak=%5ld  |%s|", rms, (long)peak_recent, bar);
            peak_recent = 0;
        }
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "=== mic-test boot ===");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Display + I2C + IOE first so we have a screen + power rails BEFORE WiFi.
    // (Provisioning mode also benefits from being able to show the SSID/URL.)
    init_i2c();
    init_pmic();
    init_ioe();
    display_init();
    touch_init(g_i2c_bus);

    wifi_init();
    bool ok = wifi_try_connect_from_nvs(15000);
    if (!ok) {
        wifi_start_provisioning();
        ui_show_wifi_setup("Meme-9515", "192.168.4.1");
        ESP_LOGI(TAG, "provisioning mode — connect to Meme-9515 / 192.168.4.1");
        while (true) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    init_i2s();
    init_codec();
    // PA stays OFF at boot; chime task power-cycles it for each chime so the
    // amp isn't continuously amplifying idle DMA noise.
    chime_init(g_codec_dev, is_recording_getter, speaker_pa_set);
    buttons_init();


    // Enable mDNS so the lwIP resolver can turn `.local` hostnames into IPs.
    // Without this, `gethostbyname("Ethan-MacBook-Air.local")` fails and the
    // WS client can't connect.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("meme");        // device is now reachable as meme.local
        mdns_instance_name_set("Meme handheld");
    } else {
        ESP_LOGW(TAG, "mdns_init failed — .local hostname won't resolve");
    }

    // Resolve Mac hostname: NVS override (via SoftAP) > compile-time default.
    char host[64] = {0};
    if (!wifi_get_mac_host(host, sizeof(host)) || !host[0]) {
        strncpy(host, DEFAULT_MAC_HOST, sizeof(host) - 1);
    }
    char ws_uri[96];
    snprintf(ws_uri, sizeof(ws_uri), "ws://%s:%d/", host, MAC_WS_PORT);
    ESP_LOGI(TAG, "ws uri: %s", ws_uri);
    ws_start(ws_uri);
    note_activity();

    ota_mark_valid();

    xTaskCreate(mic_task, "mic", 4096, nullptr, 5, nullptr);
    xTaskCreate(battery_task, "battery", 3072, nullptr, 2, nullptr);
    xTaskCreate(ota_check_task, "ota", 8192, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "ready — hold button A to talk");
}
