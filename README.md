# Meme — voice-input handheld for AI coding agents

*[中文文档](README.zh-CN.md) · [Changelog](CHANGELOG.md)*

A press-to-talk pixel companion that turns the [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) into a desk-mate for Claude Code / Codex / Cursor / any text-input AI agent.

**Hold the side button, speak, release.** Your speech streams over WiFi to a tiny Mac server, gets transcribed by Doubao streaming ASR, and is auto-pasted into whatever window has focus — Claude Code, Codex, VS Code, Slack, your editor. The other button sends Enter (single-press) or clears the input (double-press). Meanwhile the round AMOLED shows live state from your coding session: which model, today's API-equivalent spend, a per-todo dot matrix, a swipe-up dashboard, and a swipe-down pet screen with three pixel mascots to pick from.

When Claude Code or Codex finishes a turn, the device **chimes** — 880 Hz for Claude, 660 Hz for Codex — so you know it's ready without staring at the laptop.

## Architecture

```
┌──────────────────────────────────┐         WiFi WebSocket          ┌──────────────────────────────┐
│  Meme firmware (ESP-IDF)         │ ───── PCM (16 kHz, 16-bit) ───→ │  Mac server (Python)         │
│                                  │                                 │                              │
│  ES8311 mic → I2S → ws_send_task │ ←──── state JSON (2 s tick) ─── │  Doubao streaming ASR ──→    │
│  ES8311 DAC → AW8737A → speaker  │ ←──── chime msg (on completion) │     pastes via osascript     │
│  CO5300 OLED → LVGL              │                                 │                              │
│  CST820 touch (cross-shaped nav) │                                 │  Claude/Codex adapters ──→   │
│  M5PM1 PMIC (battery)            │                                 │     state push + chime emit  │
└──────────────────────────────────┘                                 └──────────────────────────────┘
```

### Cross-shaped navigation

Four screens reachable from the two main pages by swipe:

```
                     ┌──────────────┐
                     │     Pet      │  3 pixel mascots: kitty / diana / nier
                     │  (3 sprites) │  ←  L/R between them
                     └──────────────┘
                            ↑  swipe ↓ from a main page
                            │  swipe ↑ to come back
              ┌─────────────┐   ┌─────────────┐
              │   Claude    │ ↔ │   Codex     │   the two main pages
              └─────────────┘   └─────────────┘
                            │  swipe ↑ from a main page
                            ↓  swipe ↓ to come back
                     ┌──────────────┐
                     │  Dashboard   │  compact data summary
                     │ (both halves)│  Claude top, Codex bottom
                     └──────────────┘
```

- **Claude Code page** — model (e.g. `opus-4.7`), BUSY/IDLE, today's $ cost + token count, todo dot matrix, transcript line, recording indicator.
- **Codex page** — model (e.g. `gpt-5.5`), reasoning effort (HIGH/MEDIUM/LOW), real 5h + 7d quota from the `wham/usage` endpoint, today/week Codex.app focus minutes.
- **Dashboard** (swipe up from a main) — split-screen Claude % remaining on top + Codex 5h/7d quota on bottom; one clock at the bottom.
- **Pet** (swipe down from a main) — full-screen sprite, no UI chrome. Three pets, swipe L/R to switch. Hold the record button → the active pet plays the "wave" animation.

Up/down on a side screen returns you to whichever main page you came from.

Bottom of each main page: battery + charging indicator using LVGL's native symbol set.

---

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | **8 MB OPI** (board target's default is QSPI — wrong, fix in `sdkconfig.defaults`) |
| Flash | 16 MB |
| Display | CO5300 1.06" AMOLED 466×466 round, QSPI |
| Audio in | ES8311 codec + MEMS mic, I²S (16 kHz mono PCM) |
| Audio out | ES8311 DAC → AW8737A 1 W class-D PA → built-in speaker |
| Touch | CST820 single-finger capacitive |
| Power | M5PM1 PMIC (battery monitor + charge enable) |
| IO expansion | M5IOE1 (`0x4F`) — drives audio rail, speaker PA enable, vibration motor enable |
| Other I²C | RX8130 RTC `0x32`, BMI270 IMU `0x68` — neither used yet |
| Vibration motor | wired to IOE pin 9 (PWM) — present but not yet enabled in firmware |
| Buttons | GPIO 1, GPIO 2 |

Speaker PA needs **two** enable lines high simultaneously: `M5IOE1_PIN_10` and `GPIO 14`. Both stay low at boot and only toggle on for the duration of each chime — otherwise idle DMA noise on the I²S TX line gets amplified into audible hiss.

Three Arduino-side toolchain bugs we hit and worked around (see `patches/`): wrong M5IOE1 address in M5GFX 0.2.21, no StopWatch board in M5Unified 0.2.15, wrong default PSRAM mode in board target. ESP-IDF avoids all three.

---

## Firmware modules

All under `mic-test/main/`. ESP-IDF v5.5.4, LVGL 9.5, M5GFX (patched), no Arduino.

### `main.cpp` — boot orchestration

Brings up I²C → PMIC → IOE → display → touch → WiFi → I²S → ES8311 → buttons → WebSocket → mDNS → background tasks. Hosts three FreeRTOS tasks:

- **`mic_task`** — push-to-talk loop. Every 20 ms reads a frame from ES8311; batches 5 frames (100 ms) and enqueues to the WS send task; emits `{"type":"start"}` on button-A press, `{"type":"end"}` on release. Button B is single-press → `{"type":"submit"}` (Enter, delayed 300 ms to allow a double-press override) and double-press → `{"type":"clear"}` (server sends Cmd+A + Backspace). Also handles idle sleep: 120 s of inactivity → screen dim, **skipped while USB is plugged in** so a desk-bound device stays lit.
- **`battery_task`** — every 10 s reads PMIC Vbat + Vin, converts to percentage (linear 3.30 V → 0%, 4.20 V → 100%), pushes to display. Vin > 4.5 V means USB plugged in → charging icon and "no auto-sleep" mode.
- **`ota_check_task`** — once per boot, ~30 s after WiFi comes up, queries the Mac's `/firmware.bin` endpoint, compares its `esp_app_desc.app_elf_sha256` against the running image, installs to the inactive OTA slot and reboots if different.

Mac hostname is read from NVS (set via the SoftAP form) with a compile-time fallback. We resolve via mDNS (`<hostname>.local`) so the device follows the Mac across DHCP/WiFi changes.

### `wifi.cpp` — multi-profile STA + SoftAP provisioning

NVS schema (v2) keeps up to 5 (ssid, password) pairs plus a separate Mac-hostname slot. On boot, `wifi_try_connect_from_nvs(15 s)` walks the saved list, attempting each profile with two retries. Falls back to SoftAP if all profiles fail.

SoftAP serves a JS-free HTML form (`<form method=POST action=/save>`) so any phone's browser can submit it without keyboard popup glitches. Three fields: SSID (with live scan dropdown), password, Mac hostname.

**Subtle bug fixed**: `esp_netif_create_default_wifi_sta()` is not idempotent — calling it twice asserts on duplicate `if_key`. Guarded with `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL`.

### `ws_client.cpp` — WebSocket + producer/consumer audio queue

Manages the connection to the Mac server and parses inbound messages.

**Hard-won architecture**: PCM sending lives on a **dedicated FreeRTOS task** (`ws_send_task`, priority 6) draining a depth-16 queue. `ws_send_pcm()` does `malloc + memcpy + xQueueSend(timeout=0)` and returns immediately. Mic task never blocks on network. Without this, slow WiFi would back-pressure `mic_task` (which has tight I²S deadlines) and the esp_websocket_client lib would tear down the connection mid-recording, every 1–5 seconds.

Buffer size is `16384` (default 4096 was tight for the PCM batches + occasional inbound state JSON).

Inbound text frames split two ways:
- `{"type":"chime","agent":"..."}` → call `chime_play_*_done()` on the chime task's queue.
- Everything else is a state poll: parsed into widget setters (`ui_set_model`, `ui_set_claude_quota`, `ui_set_codex_limits`, `ui_set_todos_statuses`, `ui_set_battery`, `ui_set_transcript`, `ui_set_claude_summary`, …).

### `display.cpp` — LVGL UI + cross-shaped navigation

Five screens: Claude (default), Codex, Dashboard, Pet, Setup. Gesture handler routes swipes:

- LEFT/RIGHT on Claude ↔ Codex (main pages)
- TOP on a main → Dashboard
- BOTTOM on a main → Pet (and remembers which main you came from)
- BOTTOM on Dashboard → back to last main
- TOP on Pet → back to last main
- LEFT/RIGHT on Pet → next/previous pet in the strip

Boot brings up CO5300 panel via M5GFX (which wraps LGFX's `Panel_AMOLED`), allocates two PSRAM-backed LVGL line buffers (60 lines × 468 wide × 2 B ≈ 56 KB each), starts the LVGL tick timer (10 ms) + LVGL handler task, registers a flush callback that pushes pixels via `M5GFX::writePixels`.

Each main page has the same skeleton: brand row (logo + wordmark) + two arc gauges + center mascot + two stat/quota rows + bottom info. The Claude page renders today's $ cost and tokens (API-key users have no subscription quota); the Codex page renders real 5h + 7d quota.

The pet screen uses a generic `PetSpec` table — a new pet is one entry. Sprite frames are rendered at scale 2× (96×104 source → 192×208 display) with antialiasing off so the S3 can keep up at the chosen frame rate.

`ui_set_battery` picks an LVGL symbol (`LV_SYMBOL_BATTERY_*` or `LV_SYMBOL_CHARGE`) by percentage, tints the label (green charging / red below 15% / grey otherwise).

### `chime.cpp` — task-complete tones

Dedicated FreeRTOS task pulling from a depth-1 queue (newest-wins on overflow). For each chime: enables the speaker PA, plays a sine wave at the agent's pitch (880 Hz for Claude, 660 Hz for Codex) for ~450 ms with an exponential decay envelope and ~6 ms attack ramp, then disables the PA so there's no idle hiss between events.

Sample format matches the codec's open config (16 kHz, 16-bit mono). Amplitude is 22000/32767 — well below clipping but loud enough to clear ambient noise on the small built-in speaker.

Skipped automatically while the mic task is recording, so the mic doesn't pick up the tone.

### `buttons.cpp` — debounced push-to-talk events

Reads GPIO 1 (button A) and GPIO 2 (button B) at 20 ms cadence. Exposes `button_a_just_pressed()`, `button_a_just_released()`, `button_b_just_pressed()` so the mic task can run a clean state machine. Active-low with internal pull-up; ~30 ms debounce.

### `cst820.cpp` — touch driver

Driver for the CST820 capacitive controller (I²C at `0x15`). Reads X/Y on touch, exposes `read()` / `isPressed()` / `getX()` / `getY()`. Display wires this into an LVGL pointer indev; the gesture event on each screen feeds into `display.cpp`'s shared `gesture_cb`.

### Pet sprite assets

Three pets, all sourced from the [petdex](https://petdex.crafter.run) sprite library and converted by `scripts/gen_pet.py` to LVGL's RGB565A8 format:

- `kitty_pet_assets.cpp` — petdex #77, gray-white kitten
- `diana_pet_assets.cpp` — petdex `diana-2`, anime girl
- `nier_pet_assets.cpp` — petdex `nier-2b`, anime girl (2B)

Plus the original characters used as small avatars on the main pages:

- `claude_pet_assets.cpp` — openpets "claude" (orange octopus, small)
- `codex_pet_assets.cpp` — openpets "codex" (blue creature, small)

Pet animation state machine (in `display.cpp`): sit in idle for 2–5 cycles (~5–13 s), play one random expression (wait / fail / review) for a single cycle, return to idle. Pressing the record button overrides to "wave" for as long as it's held.

---

## Server modules

All under `mac-server/`. Python 3.9+, asyncio, websockets library.

### `server.py` — WebSocket server + push-to-talk handler

Listens on `ws://0.0.0.0:8765/`. Per connection:

- Spawns a `push_state_loop` task that every 2 s polls Claude + Codex adapters (via `asyncio.to_thread` so blocking I/O doesn't hijack the event loop) and sends a compact JSON state blob to the device.
- Detects **native task-complete signals** and emits a one-off WS frame `{"type":"chime","agent":"claude"|"codex"}` when a new completion crosses the threshold:
  - Claude — `stop_reason == "end_turn"` ISO timestamp in `~/.claude/projects/*/*.jsonl`, strict-forward comparison so jumping between sessions doesn't replay history.
  - Codex — most-recent `event_msg.type == "task_complete"` in the rollout JSONL referenced by `~/.codex/state_5.sqlite` (the thread index database), keyed by `thread_id:timestamp`.
- Handles text frames: `start`, `end`, `submit` (Enter), `clear` (Cmd+A + Backspace).
- Handles binary frames: appends to a WAV file in `captures/` and forwards to streaming ASR.

Also exposes a tiny HTTP endpoint at `/firmware.bin` so the device can pull OTA updates over LAN.

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

Reads the most recent `~/.claude/projects/*/*.jsonl` and aggregates: model, input/cache_create/cache_read/output tokens, estimated API-list-price cost (using current Anthropic pricing constants in the file), turn count, last turn timestamp, busy hint, the latest end-turn timestamp, today's $ spend and output tokens, and the TodoWrite list (tail-scan).

For Pro/Max subscribers there's also `_probe_claude_usage()` which refreshes the OAuth token via macOS Keychain (`Claude Code-credentials` entry) and hits `api.anthropic.com/api/oauth/usage` for real 5h / 7d quota. API-key users (no OAuth payload) just see the per-turn cost tracker.

The displayed cost is **API list price**, not what you actually pay on a Max subscription. We display it anyway because seeing "$X equivalent value consumed" is part of the fun.

### `codex_adapter.py` — Codex desktop activity + quota

- `~/.codex/auth.json` → OAuth access token → `chatgpt.com/backend-api/wham/usage` for real 5h + weekly Codex quota.
- `~/.codex/logs_2.sqlite` → latest turn's `codex.turn.reasoning_effort` and `model` (live reasoning level; more accurate than `config.toml`).
- `~/.codex/state_5.sqlite` + rollout JSONLs → latest `task_complete` event timestamp (used for chime detection).
- macOS `osascript` polled every 5 s for the frontmost app name; when it's "Codex" we accumulate seconds into a day/week bucket persisted to `~/.codex/.stopwatch_usage.json`.

### `paste.py` — AppleScript helpers

Three tiny functions:
- `paste_to_active_window(text)` — `pbcopy` + `osascript "keystroke v using command down"`.
- `press_enter()` — `osascript "keystroke return"` for the submit button.
- `clear_input()` — `osascript "keystroke a using command down; key code 51"` for the double-press-B clear gesture.
- `get_active_app()` returns the frontmost app name (for the `[→ Code]` annotation in logs).

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

For Claude OAuth quota (Pro/Max subscribers only): log into the Claude Code CLI with `claude` → `/login` → choose "Log in with Claude.ai" (not API key). Credentials land in macOS Keychain under `Claude Code-credentials` and the server reads them via `security find-generic-password`.

### 2. Firmware

Needs ESP-IDF v5.5+, `cmake`, `ninja` on PATH. Vendor the `userdemo/` reference firmware once:

```bash
git clone https://github.com/m5stack/M5StopWatch-UserDemo userdemo
cd userdemo && python fetch_repos.py
cd .. && for p in patches/*.patch; do
  ( cd userdemo && git apply ../$p )
done
```

The partition table (`mic-test/partitions.csv`) uses **two 5 MB app slots** + a 6 MB storage region — bigger than the M5 reference layout so the embedded pet sprites and dual OTA can fit. NVS lives at `0x9000`, untouched by reflashes, so Wi-Fi credentials and the Mac hostname survive any future update.

```bash
cd mic-test
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

After the first USB flash, subsequent updates can come from the running Mac server: the `ota_check_task` queries `/firmware.bin`, compares SHA-256 against the running image, and installs in the background.

### 3. First-boot WiFi provisioning

If no WiFi creds are saved (or auth fails on every saved profile):

1. Device boots into SoftAP mode, screen shows `WIFI SETUP / Meme-XXXX / 192.168.4.1`.
2. Connect your phone / laptop to the `Meme-XXXX` open hotspot.
3. Open `http://192.168.4.1/` — pick from scan list or type SSID, enter password, enter Mac hostname (or leave blank for the compile-time default), save.
4. Device restarts and joins the network.

Credentials persist in NVS; up to 5 profiles are tried in order at every boot.

---

## Configuration

Tunable in `mic-test/main/main.cpp`:

- `DEFAULT_MAC_HOST` — fallback Mac mDNS hostname (overridden by SoftAP NVS entry)
- `IDLE_SLEEP_US` — inactivity timeout before screen dims (default 120 s; skipped when USB is plugged in)
- `SAMPLE_RATE` — I²S sample rate (16 kHz; matches Doubao expectations)
- `FRAME_SAMPLES` — I²S frame size (320 = 20 ms)
- `CLAUDE_DAILY_TOKEN_CAP` (in `display.cpp`) — soft daily token cap for API-key users; defaults to 3 M

Server-side knobs (`mac-server/server.py` / `codex_adapter.py`):

- `PORT` — server listen port (default 8765)
- `DAILY_BUDGET_MIN` / `WEEKLY_BUDGET_MIN` — Codex.app focus-time bar scales
- `MODEL_PRICING` in `claude_adapter.py` — adjust if Anthropic updates list prices

---

## Not done yet

Picking these up is the natural follow-on work:

- **WiFi disconnect → SoftAP auto-fallback.** Right now NVS profiles are only walked at boot. If your home WiFi drops mid-session the device just retries forever instead of offering re-provisioning.
- **CJK font support on device.** Removed earlier to save flash (~1.7 MB on the previous 3.5 MB app partition; could revisit now that slots are 5 MB). Chinese ASR output still gets pasted to your Mac, but it doesn't render on the OLED.
- **Vibration motor.** Wired to `M5IOE1_PIN_9` as PWM but not initialized in firmware. Natural use: tactile feedback on chime / button press.
- **More pets / more mascot states.** Three pets so far; the petdex library has 2895. Lower-prio than other items but easy adds (one row in `PETS[]` + a generated asset file). Each new pet costs ~820 KB of flash.
- **RX8130 RTC clock.** Chip is on the I²C bus, not yet initialized. Useful for an offline-mode clock and persistent session uptime.
- **Background server auto-start.** The `Meme Server.command` launcher needs a double-click. A LaunchAgent would survive reboots.
- **Battery curve accuracy.** Linear 3.30 V → 0%, 4.20 V → 100% — fine as a rough indicator but real LiPo discharge is non-linear (sags fast at 3.7 V).
- **More agents.** Adapters exist for Claude Code + Codex. Cursor / Cline / Aider / etc. all expose state to disk in some form; each is a ~150-line adapter.

---

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

- **Pet sprites** — multi-pet support from [petdex](https://petdex.crafter.run) (the kitty, Diana, 2B etc.); original small avatars from [alvinunreal/openpets](https://github.com/alvinunreal/openpets)
- **Reference firmware** — [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) ships LVGL / M5GFX / M5PM1 / M5IOE1 with the right pin map, init sequence, and the speaker-PA enable pattern we cribbed
- **Quota signal layout** — [alexjc-tech/cc-island](https://github.com/alexjc-tech/cc-island) for the Claude / Codex usage-endpoint integrations
- **Audio pipeline pattern** — [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) for the ES8311 + I²S + producer-queue setup
- **Streaming ASR protocol** — wire format cribbed from [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- **LVGL** — the heart of every label and bar on screen
