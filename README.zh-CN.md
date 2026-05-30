# Meme — 给 AI 编程助手的语音输入小设备

*[English](README.md)*

把 [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) 改造成桌面 AI 编程伴侣的"按住说话"小手柄。专门给 Claude Code / Codex / Cursor 这类基于文本输入的 AI agent 用。

**按住侧边按钮说话，松手出字。** 语音通过 WiFi 流式传到 Mac 上的小 server，由豆包流式 ASR 识别，结果自动粘贴到当前聚焦窗口——Claude Code、Codex、VS Code、Slack 都行。另一个按钮发回车。圆形 AMOLED 屏上同时显示编程会话的实时状态：当前模型、输出 token 数、API 等价花费、todo 圆点矩阵、今日 Codex.app 使用时长，以及一只随录音状态变化的像素吉祥物。

## 架构

```
┌──────────────────────────────────┐         WiFi WebSocket          ┌──────────────────────────────┐
│  Meme 固件（ESP-IDF）             │ ───── PCM (16 kHz, 16-bit) ───→ │  Mac server（Python）        │
│                                  │                                 │                              │
│  ES8311 mic → I2S → ws_send_task │ ←──── 状态 JSON (2s 一次) ───── │  豆包流式 ASR ──→            │
│  CO5300 OLED → LVGL              │                                 │     osascript 粘贴           │
│  CST820 触摸（滑动切页）          │                                 │                              │
│  M5PM1 PMIC（电池）              │                                 │  Claude/Codex 适配器 →       │
└──────────────────────────────────┘                                 │     状态推送到设备           │
                                                                     └──────────────────────────────┘
```

两个页面，滑动切换：

- **Claude Code 页** — 模型（如 `opus-4.7`）、BUSY/IDLE、输出 token 数、API 等价花费 USD、todo 圆点矩阵（橙色=完成，青色=进行中，灰色=待办）
- **CodeX 页** — 模型（如 `gpt-5.5`）、推理力度（HIGH/MEDIUM/LOW）、5小时/周配额占位、今日/本周 Codex.app 聚焦时长、像素小章鱼动画

两页底部都有电池图标 + 充电状态，用的 LVGL 原生 symbol。

---

## 硬件

| 部件 | 详情 |
|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2 |
| PSRAM | **8 MB OPI**（板子默认是 QSPI 配置——错的，`sdkconfig.defaults` 里修了）|
| Flash | 16 MB |
| 显示 | CO5300 1.06 英寸 AMOLED 圆屏 466×466，QSPI |
| 录音 | ES8311 codec + MEMS 麦克风，I²S（16 kHz 单声道 PCM）|
| 触摸 | CST820 单点电容 |
| 电源 | M5PM1 PMIC（电池监测 + 充电使能）|
| IO 扩展 | M5IOE1（地址 `0x4F`，不是老版 M5GFX 0.2.21 里写的 `0x6F`）|
| 其他 I²C | RX8130 RTC `0x32`、BMI270 IMU `0x68`——目前都没用 |
| 按键 | GPIO 1, GPIO 2 |

Arduino 工具链上踩到三个 bug 全部绕过了（详见 `patches/`）：M5GFX 0.2.21 的 M5IOE1 地址错误、M5Unified 0.2.15 缺 StopWatch 板型、板子默认 PSRAM 模式错误。改用 ESP-IDF 后三个都不存在。

---

## 固件模块

全部在 `mic-test/main/` 下。ESP-IDF v5.5.4、LVGL 9.5、M5GFX（打过补丁），完全不用 Arduino。

### `main.cpp` — 启动编排

按顺序拉起 I²C → PMIC → IOE → 显示 → 触摸 → WiFi → I²S → ES8311 → 按键 → WebSocket → mDNS → 后台任务。运行两个 FreeRTOS task：

- **`mic_task`** — 按住说话主循环。每 20 ms 从 ES8311 读一帧；攒满 5 帧（100 ms）入队给 WS 发送任务；按 A 按下发 `{"type":"start"}`，松开发 `{"type":"end"}`，按 B 发 `{"type":"submit"}`。同时处理 idle 休眠（120 秒无操作 → 屏幕变暗）。
- **`battery_task`** — 每 10 秒读 PMIC Vbat + Vin，按线性映射换算百分比（3.30 V → 0%，4.20 V → 100%），推到屏幕。Vin > 4.5 V 表示插着 USB → 显示充电图标。

`MAC_WS_URI` 这个宏是**每台 Mac 唯一需要改的一行**。我们走 mDNS（`<hostname>.local`），所以 Mac IP 在 DHCP / WiFi 之间变化设备会自动跟上。

### `wifi.cpp` — STA + SoftAP 配网

三段式流程：

1. `wifi_init` 拉起 WiFi 协议栈、注册事件 handler、打开 NVS。
2. `wifi_try_connect_from_nvs(timeout_ms)` 读 NVS 里存的 `ssid` / `pass` 试 STA 模式，超时返回 false。
3. 失败后 `wifi_start_provisioning` 切到 APSTA 模式，在 `http://192.168.4.1/` 上挂一个带实时 SSID 扫描的 HTML 表单，接收用户填的凭证，写 NVS，1.5 秒后 `esp_restart()`。

**踩过的隐蔽 bug**：`esp_netif_create_default_wifi_sta()` 不是幂等的——调用两次会 assert 报 duplicate `if_key`。加了 `esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL` 守卫。

### `ws_client.cpp` — WebSocket + 生产者/消费者音频队列

管理跟 Mac server 的连接，解析回传的状态 JSON。

**辛苦得来的架构**：PCM 发送跑在**单独的 FreeRTOS task**（`ws_send_task`，优先级 6）上，消费一个深度 16 的队列。`ws_send_pcm()` 只做 `malloc + memcpy + xQueueSend(timeout=0)` 立即返回。mic_task 永远不阻塞在网络上。如果不这么搞，WiFi 一卡 mic_task 就被回压（I²S 截止时间很紧），然后 esp_websocket_client 库就会把连接断掉——每 1-5 秒一次。

buffer size 调成 `16384`（默认 4096 装不下 PCM batch + 偶尔回传的状态 JSON）。

回传的状态 JSON 被解析后调用各 setter（`ui_set_model`、`ui_set_claude_metrics`、`ui_set_codex_activity`、`ui_set_todos_statuses`、`ui_set_battery`、`ui_set_transcript` 等）。

### `display.cpp` — LVGL UI

三个屏幕：Claude（默认）、Codex、Setup（只在 SoftAP 时显示）。

启动时通过 M5GFX（它包了 LGFX 的 `Panel_AMOLED`）拉起 CO5300，分配两个 PSRAM 的 LVGL 行缓冲区（60 行 × 468 宽 × 2 字节 ≈ 56 KB 每个），启 LVGL tick 定时器（10 ms）+ LVGL handler task，注册一个 flush 回调通过 `M5GFX::writePixels` 推像素。

每个页面共用同一套骨架：标题 + 下划线 + 录音红点 + 两个并列弧形仪表 + 中央吉祥物 + section 标题 + 指标行 + 底部信息。具体每页的数据来源见上面页面说明。

`ui_set_battery` 按百分比挑 LVGL symbol（`LV_SYMBOL_BATTERY_*` 或 `LV_SYMBOL_CHARGE`），按状态染色（充电绿 / 低于 15% 红 / 其余灰）。

### `buttons.cpp` — 防抖的按键事件

20 ms 节奏轮询 GPIO 1（按键 A）和 GPIO 2（按键 B）。对外暴露 `button_a_just_pressed()`、`button_a_just_released()`、`button_b_just_pressed()`，方便 mic task 写干净的状态机。低电平有效带内部上拉，~30 ms 防抖。

### `cst820.cpp` — 触摸驱动

CST820 电容触摸控制器驱动（I²C 地址 `0x15`）。读 X/Y，暴露 `read()` / `isPressed()` / `getX()` / `getY()`。display 把它接成 LVGL pointer indev，左滑→下一页，右滑→上一页。

### `claude_pet_assets.cpp` / `codex_pet_assets.cpp` — 精灵帧

像素小章鱼来自 [openpets](https://github.com/alvinunreal/openpets) 的默认 pet 精灵图（1536×1872 webp，8×9 网格 192×208 帧）。我抽出两段动画：

- **idle** — 慢慢眨眼循环（第 0 行 8 帧，每帧 ~700 ms）
- **running** — 快速摆动循环（第 7 行 8 帧，每帧 ~100 ms）

每一帧从 webp 转成 LVGL 的 RGB565 + alpha 字节格式，输出成 C 常量。`set_codex_pet_recording(bool)` 切换两套帧——按 A 时小章鱼"从冷静变兴奋"。

Claude 页和 Codex 页用同一只章鱼；用户要求同精灵同造型，只换动作，不要换物种。

---

## 服务器模块

全部在 `mac-server/` 下。Python 3.9+、asyncio、websockets 库。

### `server.py` — WebSocket server + 按住说话 handler

监听 `ws://0.0.0.0:8765/`。每个连接：

- 启一个 `push_state_loop` 任务，每 2 秒通过 `asyncio.to_thread` 调 Claude + Codex 适配器（避免阻塞 I/O 拖垮事件循环），把紧凑的状态 JSON 推给设备。
- 处理文本帧：`start`、`end`、`submit`。
- 处理二进制帧：追加到 `captures/` 下的 WAV 文件，同时转发给流式 ASR。

ASR 路径有三层兜底：

1. **流式 ASR** — 收到 `start` 时开 Doubao WebSocket，PCM 来一帧推一帧，收到 `end` 时返回。最佳情况：松手后 ~200 ms 出文字。
2. **隐式 start** — 如果 PCM 来了但前面没有 `start` 帧（早期踩到的设备固件怪 bug），server 也照样开 WAV。
3. **录音中途断线** — 如果设备 WS 在 `end` 之前掉了，`finally` 块尝试 flush + finalize 流式 session；如果返回空，就拿录到的 WAV 走文件式 ASR。

### `doubao_streaming.py` — 豆包 seedasr 2.0 流式客户端

WebSocket 协议跟字节跳动规范走：4 字节头（版本/类型/序列/压缩）+ 4 字节大端 payload 长度 + gzip 过的 JSON 或 PCM。消息类型：`0x1` 初始 config、`0x2` 音频帧（flag `0x2` 表示最后一帧）、`0xF` server 错误。

**关键架构规则**：`start()`（WS 连接 + config 握手）通过 `start_background()` 改成 fire-and-forget。握手期间到的 PCM 暂存在 `_pending_pcm` 队列，握手完成（`_ready` 置位）后按顺序回放。如果不这么做，最差 30 秒的握手会卡住 WS 消息循环，整套都没法用。

utterance 按 `(start_ms, end_ms, text)` 三元组做 key 累积，免得 VAD 切句时彼此覆盖。

### `doubao_asr.py` — 文件式 ASR 兜底

标准豆包 `seedasr.auc` 的提交+轮询路径。当流式返回空或者根本没连上时用。初始轮询激进（100 ms），逐步退到 500 ms 上限。

### `claude_adapter.py` — Claude Code session 解析器

读最近修改的 `~/.claude/projects/*/*.jsonl`，聚合：模型、输入/cache_create/cache_read/输出 token、按当前 Anthropic 公开价目算的 API 等价花费、turn 数、最后一轮时间戳、busy 标志（最新一条 assistant turn 是否用了工具）、TodoWrite 列表（尾部扫描）。

注意算出来的 cost 是 **API 列表价**，不是订阅用户实际花的钱。我们还是显示它——看着"我又薅到了 $X 等价的 API 服务"是这个项目的一部分乐趣。

### `codex_adapter.py` — Codex 桌面 app 活动跟踪

两个数据源：

1. `~/.codex/config.toml` 读 `model` 和 `model_reasoning_effort`。
2. 每 5 秒 `osascript` 查前台 app 名字，是 "Codex" 时往日/周计数桶里累加秒数，持久化到 `~/.codex/.stopwatch_usage.json`。

最早是通过 PTY 起 Codex CLI 然后解析 `/status` 输出拿 5h/week 配额。Codex v3+ 把 CLI 改成跳转到桌面 app，这条路就废了。我们把配额条留成 `--%` 占位。

### `paste.py` — AppleScript 辅助函数

两个小函数：`paste_to_active_window(text)` 通过 `osascript "tell System Events to keystroke ..."` 模拟 cmd-V，`press_enter()` 用于提交按键。`get_active_app()` 返回前台 app 名（用于日志里的 `[→ Code]` 提示）。

---

## 上手

### 1. Mac server

```bash
cd mac-server
pip3 install websockets aiohttp python-dotenv

cp .env.example .env
# 编辑 .env 填上你的豆包 API key + app id

./run_server.sh
```

server log 输出到 stdout（用桌面 `.command` 启动时同时落到 `/tmp/server.log`）。可选：把 `Meme Server.command` 放到桌面，双击启动。

豆包 key：到 [volcengine.com](https://www.volcengine.com/) 注册，开通"流式语音识别大模型 2.0"，复制 app ID + API key。

### 2. 固件

需要 ESP-IDF v5.5+、`cmake`、`ninja` 在 PATH 里。一次性把 `userdemo/` 拉下来：

```bash
git clone https://github.com/m5stack/M5StopWatch-UserDemo userdemo
cd userdemo && python fetch_repos.py
cd .. && for p in patches/*.patch; do
  ( cd userdemo && git apply ../$p )
done
```

编辑 `mic-test/main/main.cpp` 第 28 行左右：

```c
#define MAC_WS_URI "ws://<你的 Mac 名字>.local:8765/"
```

主机名通过 `scutil --get LocalHostName` 拿，或者 **系统设置 → 通用 → 共享 → 本地主机名**。因为走 mDNS，设备会跟着 Mac 走 WiFi 切换 / IP 变化——只有改 Mac 名字时才需要重烧。

```bash
cd mic-test
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

### 3. 第一次开机配 WiFi

如果 NVS 里没存凭证（或者 8 秒内连不上）：

1. 设备进 SoftAP 模式，屏幕显示 `WIFI SETUP / Meme-XXXX / 192.168.4.1`。
2. 手机或笔电连 `Meme-XXXX` 开放热点。
3. 浏览器开 `http://192.168.4.1/`——从扫描列表里选或者手填 SSID、输密码、保存。
4. 设备自动重启加入新网络。

凭证持久化到 NVS。

---

## 可调参数

`mic-test/main/main.cpp` 里：

- `MAC_WS_URI` — Mac mDNS 主机名
- `IDLE_SLEEP_US` — 无操作多久后屏幕变暗（默认 120 秒）
- `SAMPLE_RATE` — I²S 采样率（16 kHz，匹配豆包要求）
- `FRAME_SAMPLES` — I²S 帧大小（320 = 20 ms）

服务器侧（`mac-server/server.py` / `codex_adapter.py`）：

- `PORT` — server 监听端口（默认 8765）
- `DAILY_BUDGET_MIN` / `WEEKLY_BUDGET_MIN` — Codex.app 聚焦时长进度条满刻度
- `claude_adapter.py` 里 `MODEL_PRICING` — Anthropic 改价时同步更新

屏幕进度条满刻度（`display.cpp`）：

- `CLAUDE_TOKENS_BAR_MAX` — 100M tokens 满
- `CLAUDE_COST_BAR_MAX` — $5000 满

---

## 还没做的

接手项目的话，这些是天然的下一步：

- **Codex 5h / 周真实配额数据源。** v3+ Codex CLI 跳转到桌面所以 PTY 抓不到了。挖 `~/.codex/logs_2.sqlite`（~240 MB）里的 rate-limit 事件，或者反代桌面 app 的 HTTPS 请求，是两条可能的路。目前进度条停在 `--%`。
- **Mac 主机名进 NVS。** `MAC_WS_URI` 还是 `#define`。在 SoftAP 表单上加第三栏（启动时从 NVS 读拼 URL），就能彻底消灭最后一个硬编码。
- **WiFi 断线 → SoftAP 自动兜底。** 现在 NVS 凭证只在开机时检查一次。家里 WiFi 用着用着断了，设备只会无限重连，不会自动进配网。
- **设备端中文字体支持。** 之前加过又删了（占了 4MB app 分区里的 1.7MB）。中文 ASR 结果还能粘到 Mac 上，但 OLED 上显示不了。要做就上 ~3000 常用字子集，或者扩大 flash 分区。
- **更多吉祥物状态。** 现在只有 idle / running。低电、出错、休眠、编辑中…都容易加，sprite sheet 一共 72 帧。
- **RX8130 RTC 时钟。** 芯片在 I²C 总线上但没初始化。可以做 Codex 页底部时钟或 session 持续时间。
- **OTA 升级。** 每次改固件都要插 USB。ESP-IDF 支持 WiFi OTA，分区表得改成留两个 app 分区。
- **后台 server 自动起。** `Meme Server.command` 还得双击。做成 LaunchAgent 可以开机自启。
- **电量曲线准度。** 线性 3.30 V → 0%、4.20 V → 100%——粗略指示够用了，但真实 LiPo 放电曲线是非线性的（3.7 V 附近会快速下沉）。
- **支持更多 agent。** 目前有 Claude Code + Codex 适配器。Cursor / Cline / Aider 等也都有把状态写盘的途径，每个大约一个 150 行的适配器。
- **硬件按键 → server 动作。** 现在按键 B 是硬编码回车。多次按 / 长按可以触发其他动作（切模型、`/clear`、切换 agent 等）。

---

## License

MIT — 见 [LICENSE](LICENSE)。

## 致谢

- **像素吉祥物** — 默认 pet sprite 来自 [alvinunreal/openpets](https://github.com/alvinunreal/openpets)（MIT）
- **参考固件** — [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) 提供了 LVGL / M5GFX / M5PM1 / M5IOE1 的正确引脚映射 + 初始化序列
- **音频流水线模式** — [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 提供了 ES8311 + I²S + 生产者队列的范式
- **流式 ASR 协议** — 二进制 wire format 参考自 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- **LVGL** — 屏幕上每个 label、每根进度条的底层
