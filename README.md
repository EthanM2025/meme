# Meme — voice-input handheld for AI coding agents

Press-to-talk pixel companion built on the [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3 + ES8311 mic + CO5300 round AMOLED). Press button A, speak, release — your speech is transcribed in real time and pasted into the active window (Claude Code, Codex, VS Code, …). Press button B to fire Enter and submit.

The screen also shows live state from your AI coding session — current model, output tokens, estimated API spend, and a per-todo dot matrix.

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

Two screens (swipe to switch):
- **Claude Code** — model, BUSY/IDLE, output tokens, API-equivalent cost USD, todo dot matrix
- **CodeX** — model, reasoning effort, 5h/week quota placeholders, Codex.app focus time today/week, pixel pet

The pixel mascot is the default "claude" pet from [alvinunreal/openpets](https://github.com/alvinunreal/openpets) (orange octopus, idle + running animations).

## Repo layout

```
mic-test/          ESP-IDF firmware project
  main/            firmware sources (display, mic, ws, wifi, pets, fonts)
  sdkconfig.defaults
  partitions.csv

mac-server/        Python server
  server.py            WebSocket server + push-to-talk handler
  doubao_streaming.py  ByteDance/Volcengine streaming ASR client
  doubao_asr.py        file-based ASR fallback
  claude_adapter.py    parse ~/.claude/projects/*/*.jsonl for state
  codex_adapter.py     parse ~/.codex/config.toml + Codex.app focus time
  paste.py             AppleScript cmd-V / Enter helpers
  run_server.sh

patches/           patches applied on top of M5StopWatch-UserDemo components
                   (M5GFX I²C address, M5PM1 device id, etc.)
```

`userdemo/` is **not committed** — it's M5Stack's reference firmware, used as
a source of LVGL / M5GFX / M5PM1 / M5IOE1 components. Clone it separately:

```bash
git clone https://github.com/m5stack/M5StopWatch-UserDemo userdemo
cd userdemo && python fetch_repos.py        # pulls LVGL / M5GFX into components/
cd .. && for p in patches/*.patch; do
  ( cd userdemo && git apply ../$p )
done
```

## Setup

### 1. Mac server

Needs Python 3.9+ and a Doubao (Volcengine) speech key.

```bash
cd mac-server
pip3 install websockets aiohttp python-dotenv

cp .env.example .env
# then edit .env and fill in your Doubao API key + app id

./run_server.sh
```

The server listens on `ws://0.0.0.0:8765/` and prints incoming connections, ASR partials, and the pasted final text.

### 2. Firmware

Needs ESP-IDF v5.5+ on macOS.

Edit one line in `mic-test/main/main.cpp` — set `MAC_WS_URI` to your Mac's
mDNS hostname:

```c
#define MAC_WS_URI "ws://<YourMacName>.local:8765/"
```

Get the hostname with `scutil --get LocalHostName` on your Mac, or check
**System Settings → General → Sharing → Local hostname**. Because we resolve
via mDNS, the device follows your Mac across WiFi changes and DHCP IP
rotations — only reflash if you rename your Mac.

```bash
cd mic-test
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

On first boot the device looks for stored WiFi credentials. If none are saved (or auth fails), it falls back to SoftAP provisioning:

1. Connect your phone/laptop to the `Meme-XXXX` open hotspot
2. Open `http://192.168.4.1/`
3. Pick / type your WiFi SSID + password, save
4. Device reboots and connects

Credentials are stored in NVS.

## Hardware notes

- **OPI PSRAM is required** — `sdkconfig.defaults` sets `CONFIG_SPIRAM_MODE_OCT=y`. Arduino board target's default is QSPI and breaks display init.
- **M5IOE1 is at I²C address `0x4F`** (not `0x6F` as M5GFX 0.2.21 thinks). The vendored `userdemo/components/M5GFX` has the fix.
- ES8311 codec is on the same I²C bus + I2S DIN/DOUT, mic enabled via the M5IOE1 power rail pin.

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgements

- Mascot sprites: [alvinunreal/openpets](https://github.com/alvinunreal/openpets)
- Reference firmware: [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo)
- Audio pipeline pattern: [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- Streaming ASR protocol: cribbed from [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
