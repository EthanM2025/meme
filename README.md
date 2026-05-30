# Meme — voice-input handheld for AI coding agents

A press-to-talk pixel companion that turns the [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) into a desk-mate for Claude Code / Codex / Cursor / any text-input AI agent.

**Hold the side button, speak, release.** Your speech streams over WiFi to a tiny Mac server, gets transcribed by Doubao streaming ASR, and is auto-pasted into whatever window has focus — Claude Code, Codex, VS Code, Slack, your editor. The other button sends Enter. Meanwhile the round AMOLED shows live state from your coding session: which model, output tokens, API-equivalent cost, a per-todo dot matrix, today's Codex.app focus time, and a pixel mascot reacting to whether you're recording.

## Architecture

```
┌──────────────────────────────────┐         WiFi WebSocket          ┌──────────────────────────────┐
│  Meme firmware (ESP-IDF)         │ ───── PCM (16 kHz, 16-bit) ───→ │  Mac server (Python)         │
│                                  │                                 │                              │
│  ES8311 mic → I2S → ws_send_task │ ←──── state JSON (2 s tick) ─── │  Doubao streaming ASR ──→    │
│  CO5300 OLED → LVGL              │                                 │     pastes via osascript     │
│  CST820 touch (swipe pages)      │                                 │                              │
│  M5PM1 PMIC (battery)            │                                 │  Claude/Codex adapters →     │
└──────────────────────────────────┘                                 │     state push to device     │
                                                                     └──────────────────────────────┘
```

Two screens, swipe to switch:

- **Claude Code page** — model (e.g. `opus-4.7`), BUSY/IDLE, output token count, API-equivalent cost USD, a row of dots one per todo (orange = done, cyan = in-progress, grey = pending)
- **CodeX page** — model (e.g. `gpt-5.5`), reasoning effort (HIGH/MEDIUM/LOW), 5h/week quota placeholders, Codex.app focus minutes today/week, an animated pixel octopus

Bottom of each page: battery + charging indicator using LVGL's native symbol set.

---

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | **8 MB OPI** (board target's default is QSPI — wrong, fix in `sdkconfig.defaults`) |
| Flash | 16 MB |
| Display | CO5300 1.06" AMOLED 466×466 round, QSPI |
| Audio in | ES8311 codec + MEMS mic, I²S (16 kHz mono PCM) |
| Touch | CST820 single-finger capacitive |
| Power | M5PM1 PMIC (battery monitor + charge enable) |
| IO expansion | M5IOE1 (`0x4F`, not `0x6F` like older M5GFX assumed) |
| Other I²C | RX8130 RTC `0x32`, BMI270 IMU `0x68` — neither used yet |
| Buttons | GPIO 1, GPIO 2 |

Three Arduino-side toolchain bugs we hit and worked around (see `patches/`): wrong M5IOE1 address in M5GFX 0.2.21, no StopWatch board in M5Unified 0.2.15, wrong default PSRAM mode in board target. ESP-IDF avoids all three.

---

## Firmware modules

All under `mic-test/main/`. ESP-IDF v5.5.4, LVGL 9.5, M5GFX (patched), no Arduino.

### `main.cpp` — boot orchestration

Brings up I²C → PMIC → IOE → display → touch → WiFi → I²S → ES8311 → buttons → WebSocket → mDNS → background tasks. Hosts two FreeRTOS tasks:

- **`mic_task`** — push-to-talk loop. Every 20 ms reads a frame from ES8311; batches 5 frames (100 ms) and enqueues to the WS send task; emits `{"type":"start"}` on button-A press, `{"type":"end"}` on release, `{"type":"submit"}` on button-B press. Also handles idle sleep (120 s of inactivity → screen dim).
- **`battery_task`** — every 10 s reads PMIC Vbat + Vin, converts to percentage (linear 3.30 V → 0%, 4.20 V → 100%), pushes to display. Vin > 4.5 V means USB plugged in → charging icon.

The `MAC_WS_URI` macro is **the one line you edit per Mac**. We resolve via mDNS (`<hostname>.local`) so the device follows the Mac across DHCP/WiFi changes.

### `wifi.cpp` — STA + SoftAP provisioning

Three-stage flow:

1. `wifi_init` brings up the stack, registers event handlers, opens NVS.
2. `wifi_try_connect_from_nvs(timeout_ms)` reads saved `ssid` / `pass` and tries STA mode. Returns false on timeout.
3. On failure, `wifi_start_provisioning` switches to APSTA mode, serves a one-page HTML form at `http://192.168.4.1/` with a live SSID scan, accepts the user's choice, writes to NVS, schedules `esp_restart()` in 1.5 s.

**Subtle bug fixed**: `esp_netif_create_default_wifi_sta()` is not idempotent — calling it twice asserts on duplicate `if_key`. Guarded with `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL`.

### `ws_client.cpp` — WebSocket + producer/consumer audio queue

Manages the connection to the Mac server and parses inbound state JSON.

**Hard-won architecture**: PCM sending lives on a **dedicated FreeRTOS task** (`ws_send_task`, priority 6) draining a depth-16 queue. `ws_send_pcm()` does `malloc + memcpy + xQueueSend(timeout=0)` and returns immediately. Mic task never blocks on network. Without this, slow WiFi would back-pressure `mic_task` (which has tight I²S deadlines) and the esp_websocket_client lib would tear down the connection mid-recording, every 1-5 seconds.

Buffer size is `16384` (default 4096 was tight for the PCM batches + occasional inbound state JSON).

Inbound state JSON is parsed into widget setters (`ui_set_model`, `ui_set_claude_metrics`, `ui_set_codex_activity`, `ui_set_todos_statuses`, `ui_set_battery`, `ui_set_transcript`, …).

### `display.cpp` — LVGL UI

Three screens: Claude (default), Codex, Setup (only shown during SoftAP).

Boot brings up CO5300 panel via M5GFX (which wraps LGFX's `Panel_AMOLED`), allocates two PSRAM-backed LVGL line buffers (60 lines × 468 wide × 2 B ≈ 56 KB each), starts the LVGL tick timer (10 ms) + LVGL handler task, registers a flush callback that pushes pixels via `M5GFX::writePixels`.

Each page mirrors the same skeleton: title + underline + recording dot + two arc gauges side by side + center mascot + section header + metric rows + bottom info. Specific data sources per page are documented above.

`ui_set_battery` picks an LVGL symbol (`LV_SYMBOL_BATTERY_*` or `LV_SYMBOL_CHARGE`) by percentage, tints the label (green charging / red below 15% / grey otherwise).

### `buttons.cpp` — debounced push-to-talk events

Reads GPIO 1 (button A) and GPIO 2 (button B) at 20 ms cadence. Exposes `button_a_just_pressed()`, `button_a_just_released()`, `button_b_just_pressed()` so the mic task can run a clean state machine. Active-low with internal pull-up; ~30 ms debounce.

### `cst820.cpp` — touch driver

Driver for the CST820 capacitive controller (I²C at `0x15`). Reads X/Y on touch, exposes `read()` / `isPressed()` / `getX()` / `getY()`. Display wires this into an LVGL pointer indev; gesture events on each screen are mapped to left-swipe → next page, right-swipe → previous page.

### `claude_pet_assets.cpp` / `codex_pet_assets.cpp` — sprite frames

The pixel octopus comes from [openpets](https://github.com/alvinunreal/openpets)' default pet sprite sheet (1536×1872 webp, 8×9 grid of 192×208 frames). We extract two animation strips:

- **idle** — slow blink loop (row 0, 8 frames @ ~700 ms each)
- **running** — fast wave loop (row 7, 8 frames @ ~100 ms each)

Each frame is converted from webp to LVGL's RGB565 + alpha-byte format and emitted as a C constant. `set_codex_pet_recording(bool)` swaps strips so the pet "calms down → wiggles" when you press button A.

Same sprite shared between Claude and Codex pages; user wanted same character, different animation.

---

## Server modules

All under `mac-server/`. Python 3.9+, asyncio, websockets library.

### `server.py` — WebSocket server + push-to-talk handler

Listens on `ws://0.0.0.0:8765/`. Per connection:

- Spawns a `push_state_loop` task that every 2 s polls Claude + Codex adapters (via `asyncio.to_thread` so blocking I/O doesn't hijack the event loop) and sends a compact JSON state blob to the device.
- Handles text frames: `start`, `end`, `submit`.
- Handles binary frames: appends to a WAV file in `captures/` and forwards to streaming ASR.

Multi-layered fallback for the ASR path:

1. **Streaming ASR** — opens a Doubao WebSocket on `start`, streams PCM as it arrives, returns when device sends `end`. Best case: text appears within ~200 ms of release.
2. **Implicit start** — if PCM arrives without a preceding `start` (device firmware quirk we hit early on), server opens a WAV anyway.
3. **Mid-recording disconnect** — if the device WS drops before `end`, the `finally` block tries to flush + finalize streaming session; if it returns empty, falls back to file ASR on the captured WAV.

### `doubao_streaming.py` — Doubao seedasr 2.0 streaming client

WebSocket protocol per ByteDance's spec: 4-byte header (version / type / serial / compression) + 4-byte big-endian payload length + gzipped JSON or PCM. Message types: `0x1` initial config, `0x2` audio frame (flag `0x2` = last frame), `0xF` server error.

**Critical architecture rule**: `start()` (WS connect + config handshake) is fire-and-forget via `start_background()`. PCM arriving during the handshake queues in `_pending_pcm`; once `_ready` is set, the queue drains in order. Without this, a slow handshake (≤30 s worst case) blocks the WS message loop and stalls everything.

Utterances accumulate by `(start_ms, end_ms, text)` key so VAD-split sentences don't overwrite each other.

### `doubao_asr.py` — file-based fallback

Standard Doubao `seedasr.auc` submit-and-poll path. Used when streaming returns empty or never starts. Polling starts aggressive (100 ms) and backs off to a 500 ms cap.

### `claude_adapter.py` — Claude Code session reader

Reads the most recent `~/.claude/projects/*/*.jsonl` and aggregates: model, input/cache_create/cache_read/output tokens, estimated API-list-price cost (using current Anthropic pricing constants in the file), turn count, last turn timestamp, busy hint (true if latest assistant turn used a tool), and the TodoWrite list (tail-scan).

The cost is **API list price**, not what you actually pay on a Max subscription. We display it anyway because seeing "$X equivalent value consumed" is part of the fun.

### `codex_adapter.py` — Codex desktop activity tracker

Two data sources:

1. `~/.codex/config.toml` for `model` and `model_reasoning_effort`.
2. macOS `osascript` polled every 5 s for the frontmost app name; when it's "Codex" we accumulate seconds into a day/week bucket persisted to `~/.codex/.stopwatch_usage.json`.

Originally we drove the Codex CLI via PTY and parsed `/status` output for 5h/week quota. Codex v3+ rewrote the CLI to redirect users to the desktop app, so that path is dead. We left the quota bars as `--%` placeholders.

### `paste.py` — AppleScript helpers

Two tiny functions: `paste_to_active_window(text)` uses `osascript "tell System Events to keystroke ..."`, `press_enter()` for the submit button. `get_active_app()` returns the frontmost app name (for the `[→ Code]` annotation in logs).

---

## Setup

### 1. Mac server

```bash
cd mac-server
pip3 install websockets aiohttp python-dotenv

cp .env.example .env
# fill in your Doubao API key + app id

./run_server.sh
```

Server log goes to stdout (and `/tmp/server.log` when launched via the desktop `.command` shortcut). Optional: copy `Meme Server.command` somewhere convenient and double-click to start.

Doubao keys: register at [volcengine.com](https://www.volcengine.com/), enable the "Streaming ASR Big Model 2.0" SKU, copy the app ID + API key.

### 2. Firmware

Needs ESP-IDF v5.5+, `cmake`, `ninja` on PATH. Vendor the `userdemo/` reference firmware once:

```bash
git clone https://github.com/m5stack/M5StopWatch-UserDemo userdemo
cd userdemo && python fetch_repos.py
cd .. && for p in patches/*.patch; do
  ( cd userdemo && git apply ../$p )
done
```

Edit `mic-test/main/main.cpp` line ~28:

```c
#define MAC_WS_URI "ws://<YourMacName>.local:8765/"
```

Get the hostname with `scutil --get LocalHostName`, or **System Settings → General → Sharing → Local hostname**. Because we resolve via mDNS, the device follows your Mac across WiFi/IP changes — only reflash if you rename the Mac.

```bash
cd mic-test
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

### 3. First-boot WiFi provisioning

If no WiFi creds are saved (or auth fails after 8 s):

1. Device boots into SoftAP mode, screen shows `WIFI SETUP / Meme-XXXX / 192.168.4.1`.
2. Connect your phone / laptop to the `Meme-XXXX` open hotspot.
3. Open `http://192.168.4.1/` — pick from scan list or type SSID, enter password, save.
4. Device restarts and joins the network.

Credentials persist in NVS.

---

## Configuration

Tunable in `mic-test/main/main.cpp`:

- `MAC_WS_URI` — Mac mDNS hostname
- `IDLE_SLEEP_US` — inactivity timeout before screen dims (default 120 s)
- `SAMPLE_RATE` — I²S sample rate (16 kHz; matches Doubao expectations)
- `FRAME_SAMPLES` — I²S frame size (320 = 20 ms)

Server-side knobs (`mac-server/server.py` / `codex_adapter.py`):

- `PORT` — server listen port (default 8765)
- `DAILY_BUDGET_MIN` / `WEEKLY_BUDGET_MIN` — Codex.app focus-time bar scales
- `MODEL_PRICING` in `claude_adapter.py` — adjust if Anthropic updates list prices

Display bar scales (`display.cpp`):

- `CLAUDE_TOKENS_BAR_MAX` — 100M tokens for full bar
- `CLAUDE_COST_BAR_MAX` — $5000 for full bar

---

## Not done yet

Picking these up is the natural follow-on work:

- **Codex 5h / week real quota.** The v3+ Codex CLI redirected to desktop so PTY scraping is dead. Mining `~/.codex/logs_2.sqlite` (~240 MB) for rate-limit events, or proxying the desktop app's HTTPS calls, are the two paths. Currently the bars read `--%`.
- **Mac hostname in NVS.** `MAC_WS_URI` is still a `#define`. Adding a third field to the SoftAP form (and reading it back into the URL at boot) would eliminate the last hardcoded thing.
- **WiFi disconnect → SoftAP auto-fallback.** Right now NVS creds are only checked at boot. If your home WiFi drops mid-session the device just retries forever instead of offering re-provisioning.
- **CJK font support on device.** Removed earlier to save flash (1.7 MB of the 4 MB app partition). Chinese ASR output still gets pasted to your Mac, but it doesn't render on the OLED. Would need a subset (~3000 most common chars) or a larger flash partition.
- **More mascot states.** Currently idle / running only. Battery-low, error, sleeping, mid-edit, … all easy adds; the sprite sheet has 72 frames total.
- **RX8130 RTC clock.** Chip is on the I²C bus, not yet initialized. Useful for a Codex page bottom clock or session uptime.
- **OTA updates.** Every firmware change needs a USB cable. ESP-IDF supports OTA over WiFi, partition table needs adjusting to fit two app partitions.
- **Background server auto-start.** The `Meme Server.command` launcher needs a double-click. A LaunchAgent would survive reboots.
- **Battery curve accuracy.** Linear 3.30 V → 0%, 4.20 V → 100% — fine as a rough indicator but real LiPo discharge is non-linear (sags fast at 3.7 V).
- **More agents.** Adapters exist for Claude Code + Codex. Cursor / Cline / Aider / etc. all expose state to disk in some form; each is a ~150-line adapter.
- **Hardware buttons → server actions.** Right now button B is hardcoded to Enter. Multi-press / long-press could fire other actions (model swap, /clear, switch agent).

---

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

- **Pixel mascot** — default pet sprite sheet from [alvinunreal/openpets](https://github.com/alvinunreal/openpets) (MIT)
- **Reference firmware** — [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) ships LVGL / M5GFX / M5PM1 / M5IOE1 with the right pin map and init sequence
- **Audio pipeline pattern** — [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) for the ES8311 + I²S + producer-queue setup
- **Streaming ASR protocol** — wire format cribbed from [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- **LVGL** — the heart of every label and bar on screen
