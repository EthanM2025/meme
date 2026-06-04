# Meme — 给 AI 编程助手的语音输入小设备

*[English](README.md) · [更新日志](CHANGELOG.md)*

把 [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) 改造成桌面 AI 编程伴侣的"按住说话"小手柄。专门给 Claude Code / Codex / Cursor 这类基于文本输入的 AI agent 用。

**按住侧边按钮说话，松手出字。** 语音通过 WiFi 流式传到 Mac 上的小 server，由豆包流式 ASR 识别，结果自动粘贴到当前聚焦窗口——Claude Code、Codex、VS Code、Slack 都行。另一个按钮单击发回车，双击清空输入框。圆形 AMOLED 屏上同时显示编程会话的实时状态：当前模型、今日 API 等价花费、todo 圆点矩阵、上滑可看到的数据总览页、下滑可切换的三只像素吉祥物。

Claude Code 或 Codex 完成一次任务时，设备会**响一声**——Claude 是 880 Hz 高音，Codex 是 660 Hz 低音——你不用一直盯着笔记本也能知道任务完了。

## 架构

```
┌──────────────────────────────────┐         WiFi WebSocket          ┌──────────────────────────────┐
│  Meme 固件（ESP-IDF）             │ ───── PCM (16 kHz, 16-bit) ───→ │  Mac server（Python）        │
│                                  │                                 │                              │
│  ES8311 mic → I2S → ws_send_task │ ←──── 状态 JSON (2s 一次) ───── │  豆包流式 ASR ──→            │
│  ES8311 DAC → AW8737A → 喇叭     │ ←──── chime 消息（任务完成时） │     osascript 粘贴           │
│  CO5300 OLED → LVGL              │                                 │                              │
│  CST820 触摸（十字导航）         │                                 │  Claude/Codex 适配器 →       │
│  M5PM1 PMIC（电池）              │                                 │     状态推送 + chime 发出    │
└──────────────────────────────────┘                                 └──────────────────────────────┘
```

### 十字导航

主界面（Claude / Codex 左右滑）基础上，上下两个方向各挂一个辅助屏：

```
                     ┌──────────────┐
                     │   Pet 屏     │  三只像素吉祥物：kitty / diana / nier
                     │  （3 只猫）  │  ← 左右滑切换
                     └──────────────┘
                            ↑  主界面下滑进入
                            │  Pet 屏上滑返回
              ┌─────────────┐   ┌─────────────┐
              │   Claude    │ ↔ │   Codex     │   主界面（左右滑切换）
              └─────────────┘   └─────────────┘
                            │  主界面上滑进入
                            ↓  Dashboard 下滑返回
                     ┌──────────────┐
                     │  Dashboard   │  紧凑数据总览
                     │ （上下两半）│  上半 Claude、下半 Codex
                     └──────────────┘
```

- **Claude Code 页** — 模型（如 `opus-4.7`）、BUSY/IDLE、今日 $ 花费 + token 数、todo 圆点矩阵、转写文本、录音指示。
- **Codex 页** — 模型（如 `gpt-5.5`）、推理力度（HIGH/MEDIUM/LOW）、来自 `wham/usage` 端点的真实 5h + 7d 配额、今日/本周 Codex.app 聚焦时长。
- **Dashboard**（主页上滑） — 上下分屏：上半 Claude 剩余 %、下半 Codex 5h/7d 配额；底部一个时钟。
- **Pet**（主页下滑） — 整屏宠物动画，没有其他文字数字。三只可切换，左右滑选；按住录音键时当前宠物会切到 wave（挥手）动画。

辅助屏上反向滑（dashboard 下滑 / pet 上滑）回到刚才那个主页。

主界面底部都有电池图标 + 充电状态，用的 LVGL 原生 symbol。

---

## 硬件

| 模块 | 详情 |
|---|---|
| 主控 | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | **8 MB OPI**（board target 默认是 QSPI——错的，需在 `sdkconfig.defaults` 修） |
| Flash | 16 MB |
| 显示屏 | CO5300 1.06" AMOLED 466×466 圆形，QSPI |
| 音频输入 | ES8311 codec + MEMS mic，I²S（16 kHz mono PCM） |
| 音频输出 | ES8311 DAC → AW8737A 1 W class-D 功放 → 板载喇叭 |
| 触摸 | CST820 单指电容 |
| 电源 | M5PM1 PMIC（电池监测 + 充电使能） |
| IO 扩展 | M5IOE1 (`0x4F`) — 驱动音频电源轨、喇叭 PA 使能、震动马达使能 |
| 其他 I²C | RX8130 RTC `0x32`、BMI270 IMU `0x68` — 都还没用 |
| 震动马达 | 接在 IOE pin 9 (PWM) — 硬件有，固件还没启用 |
| 按键 | GPIO 1、GPIO 2 |

喇叭功放需要 **两个 enable 引脚同时拉高**：`M5IOE1_PIN_10` 和 `GPIO 14`。两个都在开机时保持低，只在每次响 chime 期间通电——否则 I²S TX 空闲时 DMA 会一直放残留 buffer，被功放放大成持续底噪。

Arduino 端我们踩过三个工具链 bug 都打了 patch（见 `patches/`）：M5GFX 0.2.21 里 M5IOE1 地址错了、M5Unified 0.2.15 没有 StopWatch 板子、board target 默认 PSRAM 模式错的。ESP-IDF 路线三个都避开。

---

## 固件模块

全部在 `mic-test/main/` 下。ESP-IDF v5.5.4、LVGL 9.5、M5GFX（打了 patch），完全不用 Arduino。

### `main.cpp` — 启动编排

依次拉起 I²C → PMIC → IOE → 显示 → 触摸 → WiFi → I²S → ES8311 → 按键 → WebSocket → mDNS → 后台任务。同时跑三个 FreeRTOS 任务：

- **`mic_task`** — 按住说话主循环。每 20 ms 从 ES8311 读一帧；攒满 5 帧（100 ms）扔给 WS 发送任务；按 A 键发 `{"type":"start"}`，松手发 `{"type":"end"}`。按 B 键单击发 `{"type":"submit"}`（回车，延迟 300 ms 留给可能的双击覆盖）；300 ms 内双击发 `{"type":"clear"}`（server 端发 Cmd+A + 退格清空输入框）。还负责 idle sleep：120 秒无操作后屏幕熄灭，**插着 USB 时跳过**，放桌子上充电时屏幕一直亮。
- **`battery_task`** — 每 10 秒读 PMIC Vbat + Vin，换算成百分比（线性 3.30 V → 0%，4.20 V → 100%），推到屏幕。Vin > 4.5 V = 接了 USB → 显示充电图标并启用"不自动睡"模式。
- **`ota_check_task`** — 每次开机后 ~30 秒，等 WiFi 起来后查 Mac 上的 `/firmware.bin`，对比 `esp_app_desc.app_elf_sha256` 与正在运行的镜像。不一样就把新版写到 inactive OTA 槽里并重启。

Mac 主机名从 NVS 读（通过 SoftAP 表单设置），有编译时 fallback。我们走 mDNS 解析（`<hostname>.local`），所以 Mac 在 DHCP / WiFi 之间换 IP 设备也跟得上。

### `wifi.cpp` — 多 profile STA + SoftAP 配网

NVS schema v2 存最多 5 组 (ssid, password) 加上独立的 Mac hostname 字段。开机时 `wifi_try_connect_from_nvs(15 s)` 按顺序尝试每个 profile，每个最多重试 2 次。全失败才进 SoftAP。

SoftAP 提供一个无 JS 的 HTML 表单（`<form method=POST action=/save>`），任何手机浏览器都能正常提交，不会出现键盘弹不出来的问题。三个字段：SSID（带实时扫描下拉）、密码、Mac 主机名。

**踩过的坑**：`esp_netif_create_default_wifi_sta()` 不是幂等的——重复调会因 duplicate `if_key` 触发 assert。加了 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL` 守卫。

### `ws_client.cpp` — WebSocket + 生产者/消费者音频队列

管理跟 Mac server 的连接，解析进来的消息。

**关键架构**：PCM 发送跑在**独立的 FreeRTOS 任务**（`ws_send_task`，优先级 6）里，从深度 16 的队列消费。`ws_send_pcm()` 做完 `malloc + memcpy + xQueueSend(timeout=0)` 立即返回。Mic 任务永远不会因为网络阻塞。没这层的话，WiFi 卡顿会反压 `mic_task`（I²S 时间预算很紧），esp_websocket_client 库每隔 1–5 秒就会把连接掐掉。

Buffer size 设成 `16384`（默认 4096 对 PCM 批次 + 偶尔进来的状态 JSON 有点紧）。

进来的文本帧两路分流：
- `{"type":"chime","agent":"..."}` → 把对应的 `chime_play_*_done()` 塞到 chime 任务队列。
- 其他都是状态推送 → 解析后调对应的 widget setter（`ui_set_model`、`ui_set_claude_quota`、`ui_set_codex_limits`、`ui_set_todos_statuses`、`ui_set_battery`、`ui_set_transcript`、`ui_set_claude_summary`…）。

### `display.cpp` — LVGL UI + 十字导航

五个屏幕：Claude（默认）、Codex、Dashboard、Pet、Setup。手势 handler 路由：

- 主界面左右滑 → Claude ↔ Codex
- 主界面**上滑** → Dashboard（并记下从哪个主页来）
- 主界面**下滑** → Pet（同上）
- Dashboard **下滑** → 回到刚才的主页
- Pet **上滑** → 回到刚才的主页
- Pet 左右滑 → 下一只 / 上一只宠物

启动时通过 M5GFX（封装了 LGFX 的 `Panel_AMOLED`）拉起 CO5300 屏，在 PSRAM 申请两块 LVGL line buffer（60 行 × 468 宽 × 2 B ≈ 56 KB 每块），起 LVGL tick 定时器（10 ms）+ LVGL handler 任务，注册 flush callback 通过 `M5GFX::writePixels` 推像素。

每个主界面骨架相同：品牌行（logo + 文字）+ 两个圆弧表 + 中央吉祥物 + 两条统计/配额行 + 底部信息。Claude 页显示今日 $ 花费和 tokens（API key 用户没有订阅配额）；Codex 页显示真实 5h + 7d 配额。

Pet 屏用了通用的 `PetSpec` 表 —— 加一只新宠物只需一行配置。Sprite 帧按 2× 缩放渲染（96×104 源 → 192×208 显示），关闭 AA，让 S3 能按帧率跟上。

`ui_set_battery` 根据百分比挑 LVGL symbol（`LV_SYMBOL_BATTERY_*` 或 `LV_SYMBOL_CHARGE`），染色（充电=绿、低于 15%=红、其他=灰）。

### `chime.cpp` — 任务完成提示音

独立 FreeRTOS 任务从深度 1 的队列消费（队满时丢旧的、留新的）。每次响铃：开启喇叭 PA、按对应频率（Claude 880 Hz、Codex 660 Hz）播 ~450 ms 的正弦波（带指数衰减包络 + ~6 ms attack ramp）、关闭 PA。这样空闲时间设备绝对安静，没有底噪。

样本格式跟 codec 打开时一致（16 kHz、16-bit、单声道）。幅度 22000/32767——离截止还有余量但够冲过环境噪声。

录音过程中自动跳过，免得麦克风把自己刚发出的提示音录进去。

### `buttons.cpp` — 按键防抖

20 ms 周期读 GPIO 1（按键 A）和 GPIO 2（按键 B）。暴露 `button_a_just_pressed()`、`button_a_just_released()`、`button_b_just_pressed()`，mic 任务可以跑干净的状态机。按下为低，内部上拉；~30 ms 防抖。

### `cst820.cpp` — 触摸驱动

CST820 电容触摸控制器（I²C `0x15`）驱动。触摸时读 X/Y，暴露 `read()` / `isPressed()` / `getX()` / `getY()`。Display 把它接入 LVGL pointer indev；手势事件通过 `display.cpp` 的统一 `gesture_cb` 处理。

### 宠物 sprite 资源

三只猫，全部来自 [petdex](https://petdex.crafter.run) 开源 sprite 库，通过 `scripts/gen_pet.py` 转成 LVGL 的 RGB565A8 格式：

- `kitty_pet_assets.cpp` — petdex #77，灰白小猫
- `diana_pet_assets.cpp` — petdex `diana-2`，二次元少女
- `nier_pet_assets.cpp` — petdex `nier-2b`，二次元少女（尼尔 2B）

加上主界面上的小头像：

- `claude_pet_assets.cpp` — openpets "claude"（橙色小章鱼，小尺寸）
- `codex_pet_assets.cpp` — openpets "codex"（蓝色小生物，小尺寸）

宠物动画状态机（在 `display.cpp` 里）：先 idle 待机 2–5 个周期（约 5–13 秒），中间随机插一次表情动作（wait / fail / review），完了回 idle。按住录音键时强制切到 wave，松手后下一周期自动回 idle。

---

## Server 模块

全部在 `mac-server/` 下。Python 3.9+，asyncio，websockets 库。

### `server.py` — WebSocket server + 按住说话 handler

监听 `ws://0.0.0.0:8765/`。每个连接：

- 起一个 `push_state_loop` 任务，每 2 秒轮询 Claude + Codex 适配器（通过 `asyncio.to_thread` 避免阻塞 I/O 卡 event loop），把紧凑 JSON 状态推给设备。
- 检测 **原生任务完成信号**，触发时发独立 WS 帧 `{"type":"chime","agent":"claude"|"codex"}`：
  - Claude — 扫 `~/.claude/projects/*/*.jsonl` 最近的 `stop_reason == "end_turn"` 的 assistant 消息时间戳，**只在 ISO 时间戳严格往前走时**才响（跨多个 session 跳来跳去不会回放旧的完成）。
  - Codex — 从 `~/.codex/state_5.sqlite`（thread 索引数据库）找最近 thread → 对应 rollout JSONL → 倒着找 `event_msg.type == "task_complete"` 事件，键值用 `thread_id:timestamp`。
- 处理文本帧：`start`、`end`、`submit`（回车）、`clear`（Cmd+A + 退格）。
- 处理二进制帧：append 到 `captures/` 下的 WAV 文件，同时转给流式 ASR。

还顺便提供了一个小 HTTP 端点 `/firmware.bin`，设备走局域网拉 OTA。

ASR 路径多层兜底：

1. **流式 ASR** — `start` 时连豆包 WS，边收 PCM 边流，设备发 `end` 时定稿。理想情况下松手 ~200 ms 出字。
2. **隐式 start** — 没等到 `start` 就开始进 PCM（早期固件 bug），server 还是开 WAV。
3. **录音过程中断连** — 设备 WS 在 `end` 之前掉线，`finally` 块试着 flush + finalize 流式 session；返回空就 fallback 到文件 ASR。

### `doubao_streaming.py` — 豆包 seedasr 2.0 流式客户端

WS 协议按 ByteDance 规范：4 字节 header（version / type / serial / compression）+ 4 字节 big-endian payload 长度 + gzipped JSON 或 PCM。消息类型：`0x1` 初始配置、`0x2` 音频帧（flag `0x2` = 最后一帧）、`0xF` server 错误。

**关键架构**：`start()`（WS 连接 + 配置握手）通过 `start_background()` 异步发后即忘。握手期间到的 PCM 排到 `_pending_pcm`；`_ready` 标志置位后队列按序流出。否则慢握手（最差 30 秒）会卡 WS 消息循环，整个流水线僵死。

每段语音用 `(start_ms, end_ms, text)` 三元组 key 累加，避免 VAD 切句的部分结果互相覆盖。

### `doubao_asr.py` — 文件兜底

标准的豆包 `seedasr.auc` 提交-轮询路径。流式返回空或没起来时用。轮询初始激进（100 ms）退避到 500 ms 上限。

### `claude_adapter.py` — Claude Code session 读取器

读最近的 `~/.claude/projects/*/*.jsonl` 聚合：模型、input/cache_create/cache_read/output token、API 等价花费（用文件里的 Anthropic 价格常量）、turn 数、最后 turn 时间、busy 提示、最近 end_turn 时间戳、今日花费 + 输出 token、TodoWrite 列表（倒序扫）。

Pro/Max 订阅用户还有 `_probe_claude_usage()`：通过 macOS Keychain 的 `Claude Code-credentials` 取出 OAuth token、刷新后调 `api.anthropic.com/api/oauth/usage` 拿真实 5h / 7d 配额。API key 用户（没 OAuth payload）就只看每 turn 的成本累计。

显示的成本是 **API 牌价**，不是 Max 订阅实际付的钱。但看到"今日烧了 $X 的等价 token"还是挺有趣的，所以保留这个显示。

### `codex_adapter.py` — Codex 桌面活动 + 配额

- `~/.codex/auth.json` → OAuth access token → `chatgpt.com/backend-api/wham/usage` 拿真实 5h + 周配额。
- `~/.codex/logs_2.sqlite` → 最近一次 turn 的 `codex.turn.reasoning_effort` 和 `model`（实时推理力度；比 `config.toml` 准）。
- `~/.codex/state_5.sqlite` + rollout JSONL → 最近 `task_complete` 事件时间戳（用于 chime 检测）。
- macOS `osascript` 每 5 秒轮询前台 app 名字；是 "Codex" 时累加秒数到日/周桶，持久化到 `~/.codex/.stopwatch_usage.json`。

### `paste.py` — AppleScript 辅助

几个小函数：
- `paste_to_active_window(text)` — `pbcopy` + `osascript "keystroke v using command down"`。
- `press_enter()` — `osascript "keystroke return"` 对应单击按钮 B。
- `clear_input()` — `osascript "keystroke a using command down; key code 51"`，对应双击按钮 B 清空输入。
- `get_active_app()` 返回前台 app 名（log 里 `[→ Code]` 那个标记）。

---

## 安装步骤

### 1. Mac server

```bash
cd mac-server
pip3 install websockets aiohttp python-dotenv

cp .env.example .env
# 填豆包 API key + app id

./run_server.sh
```

Server 日志输出到 stdout（用桌面 `.command` 启动时进 `/tmp/server.log`）。也可以把 `Meme Server.command` 放在桌面，双击就启动。

豆包密钥：在 [volcengine.com](https://www.volcengine.com/) 注册，开通"流式语音识别大模型 2.0"，复制 app ID + API key。

Claude OAuth 配额（只对 Pro/Max 订阅用户有用）：在终端跑 `claude` → `/login` → 选 "Log in with Claude.ai"（不要选 API key）。凭据写到 macOS Keychain 的 `Claude Code-credentials` 项里，server 通过 `security find-generic-password` 读。

### 2. 固件

需要 ESP-IDF v5.5+，`cmake`、`ninja` 在 PATH。Vendor 一次 `userdemo/` 参考固件：

```bash
git clone https://github.com/m5stack/M5StopWatch-UserDemo userdemo
cd userdemo && python fetch_repos.py
cd .. && for p in patches/*.patch; do
  ( cd userdemo && git apply ../$p )
done
```

分区表（`mic-test/partitions.csv`）用 **两个 5 MB app 槽** + 6 MB storage——比 M5 默认布局大，为了塞下嵌入的宠物 sprite 和双 OTA 槽。NVS 还在 `0x9000`，重烧不影响，所以 Wi-Fi 凭据和 Mac hostname 都不会丢。

```bash
cd mic-test
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

第一次 USB 烧录之后，后续更新可以从运行中的 Mac server 拉：`ota_check_task` 查 `/firmware.bin`、跟运行镜像比 SHA-256、不同就后台装。

### 3. 首次开机 WiFi 配网

如果 NVS 里没存 WiFi 凭据，或者所有保存的 profile 都连不上：

1. 设备进 SoftAP 模式，屏幕显示 `WIFI SETUP / Meme-XXXX / 192.168.4.1`。
2. 用手机 / 笔记本连 `Meme-XXXX` 开放热点。
3. 浏览器开 `http://192.168.4.1/` —— 从扫描列表选或者直接输 SSID，输密码，输 Mac 主机名（留空就用编译时默认），保存。
4. 设备重启加入 WiFi。

凭据持久化到 NVS；最多存 5 个 profile，每次开机按顺序试。

---

## 配置

可改的（`mic-test/main/main.cpp`）：

- `DEFAULT_MAC_HOST` — fallback Mac mDNS 主机名（被 SoftAP NVS 项覆盖）
- `IDLE_SLEEP_US` — 无操作后屏幕熄灭的超时（默认 120 s；插 USB 时跳过）
- `SAMPLE_RATE` — I²S 采样率（16 kHz，匹配豆包预期）
- `FRAME_SAMPLES` — I²S 帧大小（320 = 20 ms）
- `CLAUDE_DAILY_TOKEN_CAP`（在 `display.cpp`） — API key 用户的软性日 token 上限；默认 3 M

Server 端旋钮（`mac-server/server.py` / `codex_adapter.py`）：

- `PORT` — server 监听端口（默认 8765）
- `DAILY_BUDGET_MIN` / `WEEKLY_BUDGET_MIN` — Codex.app 聚焦时长 bar 的标度
- `MODEL_PRICING`（在 `claude_adapter.py`） — Anthropic 改价时同步更新

---

## 还没做的

接手项目的话，这些是天然的下一步：

- **WiFi 断线 → SoftAP 自动兜底。** 现在 NVS profile 只在开机时检查一次。家里 WiFi 用着用着断了，设备只会无限重连，不会自动进配网。
- **设备端中文字体支持。** 之前加过又删了（占了原 3.5 MB app 分区的 1.7 MB；现在槽是 5 MB，可以重新考虑加回来）。中文 ASR 结果还能粘到 Mac 上，但 OLED 上显示不了。
- **震动马达。** 接在 `M5IOE1_PIN_9` 上、走 PWM，但固件还没初始化。自然用途：chime 时震一下、按键触感反馈。
- **更多宠物 / 更多吉祥物状态。** 现在三只，petdex 库里一共 2895 个。优先级不高但很容易加 —— `PETS[]` 数组里一行 + 一个生成的 asset 文件。每只新宠物吃 ~820 KB flash。
- **RX8130 RTC 时钟。** 芯片在 I²C 总线上，没初始化。可以做离线模式时钟和持久化 session 计时。
- **后台 server 自动起。** `Meme Server.command` 还得双击。做成 LaunchAgent 可以开机自启。
- **电量曲线准度。** 线性 3.30 V → 0%、4.20 V → 100% —— 粗略指示够用了，但真实 LiPo 放电曲线是非线性的（3.7 V 附近会快速下沉）。
- **支持更多 agent。** 目前有 Claude Code + Codex 适配器。Cursor / Cline / Aider 等也都有把状态写盘的途径，每个大约一个 150 行的适配器。

---

## License

MIT — 见 [LICENSE](LICENSE)。

## 致谢

- **宠物 sprite** — 多宠物来自 [petdex](https://petdex.crafter.run)（kitty、Diana、2B 等）；主界面上的小头像来自 [alvinunreal/openpets](https://github.com/alvinunreal/openpets)
- **参考固件** — [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) 提供了 LVGL / M5GFX / M5PM1 / M5IOE1 的正确引脚映射、初始化序列，包括我们抄来的喇叭 PA 启用模式
- **配额信号布局** — [alexjc-tech/cc-island](https://github.com/alexjc-tech/cc-island) 提供了 Claude / Codex usage 端点接入参考
- **音频流水线模式** — [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 提供了 ES8311 + I²S + 生产者队列的范式
- **流式 ASR 协议** — 二进制 wire format 参考自 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- **LVGL** — 屏幕上每个 label、每根进度条的底层
