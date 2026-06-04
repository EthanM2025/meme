#!/usr/bin/env python3
"""
v1 WebSocket server for the StopWatch mic stream.
Receives raw PCM (16-bit LE mono @ 16 kHz) as binary frames.
Saves each session to a WAV file in ./captures/ and prints stats.

Next milestone: pipe the audio into Doubao ASR and paste text into the active window.
"""
import asyncio
import math
import os
import struct
import sys
import time
import wave
from typing import Optional

try:
    import websockets
except ImportError:
    print("Need `pip install websockets`. Run:")
    print("  pip3 install websockets")
    sys.exit(1)

# Load .env (DOUBAO_API_KEY etc.) before importing doubao_asr.
try:
    from dotenv import load_dotenv
    load_dotenv(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))
except ImportError:
    pass  # fall back to env vars set in shell

from doubao_asr import transcribe_wav
from doubao_streaming import StreamingASRSession
from paste import paste_to_active_window, get_active_app, press_enter, clear_input
from claude_adapter import get_state as get_claude_state
from codex_adapter import get_state as get_codex_state, daemon_loop as codex_activity_daemon
import threading
import json as _json

# Force line buffering so logs flush through tee/pipe in real time.
sys.stdout.reconfigure(line_buffering=True)

PORT = 8765
SAMPLE_RATE = 16000
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")
os.makedirs(OUT_DIR, exist_ok=True)

# Latest ASR transcript — picked up by push_state_loop and shipped to the device.
_last_transcript = ""


def _tail_text(path: str, max_bytes: int = 512 * 1024) -> str:
    """Read the end of a rollout JSONL without loading huge sessions."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - max_bytes), os.SEEK_SET)
        return f.read().decode("utf-8", errors="ignore")


def _read_codex_completion_signal() -> Optional[str]:
    """Return Codex's latest native task-complete marker from session rollouts.

    Codex Desktop/CLI persists each thread's event stream in rollout JSONL files,
    and completed turns include an `event_msg` payload with type `task_complete`.
    This is the same native completion surface that feeds app notifications, and
    avoids noisy background activity writes that are unrelated to turn completion.
    """
    try:
        import pathlib
        import sqlite3

        state_path = pathlib.Path.home() / ".codex" / "state_5.sqlite"
        if not state_path.exists():
            return None
        conn = sqlite3.connect(f"file:{state_path}?mode=ro", uri=True, timeout=1.0)
        try:
            rows = conn.execute(
                """
                SELECT id, rollout_path
                  FROM threads
                 WHERE model_provider = 'openai'
                   AND rollout_path IS NOT NULL
                   AND (thread_source IS NULL OR thread_source != 'subagent')
                 ORDER BY updated_at_ms DESC
                 LIMIT 8
                """
            ).fetchall()
        finally:
            conn.close()

        best_signal = None
        best_ts = ""
        for thread_id, rollout_path in rows:
            if not rollout_path or not os.path.exists(rollout_path):
                continue
            for line in reversed(_tail_text(rollout_path).splitlines()):
                try:
                    obj = _json.loads(line)
                except Exception:
                    continue
                payload = obj.get("payload") if isinstance(obj, dict) else None
                if not isinstance(payload, dict) or payload.get("type") != "task_complete":
                    continue
                ts = obj.get("timestamp") or ""
                signal = f"{thread_id}:{ts}"
                if ts >= best_ts:
                    best_ts = ts
                    best_signal = signal
                break
        return best_signal
    except Exception:
        return None


def rms_int16(b: bytes) -> int:
    if not b:
        return 0
    n = len(b) // 2
    samples = struct.unpack(f"<{n}h", b)
    sq = sum(s * s for s in samples)
    return int(math.sqrt(sq / n))


async def asr_in_background(fn: str, secs: float):
    """ASR a closed WAV file. Runs concurrently so the stream keeps flowing."""
    label = os.path.basename(fn)
    print(f"  [{label}] → file ASR task started ({secs:.1f}s clip)")
    try:
        if not (0.3 <= secs <= 60.0):
            print(f"  (skip ASR — clip {secs:.2f}s out of bounds)")
            return
        t0 = time.time()
        res = await transcribe_wav(fn)
        dt = time.time() - t0
        if "error" in res:
            print(f"  [{label}] ✗ ASR error: {res['error']}")
        else:
            txt = res["text"].strip()
            if not txt:
                print(f"  [{label}] ✓ {dt:.1f}s  →  (空)")
            else:
                app = get_active_app()
                print(f"  [{label}] ✓ {dt:.1f}s  →  {txt}    [→ {app}]")
                global _last_transcript
                _last_transcript = txt
                paste_to_active_window(txt)
    except Exception as e:
        import traceback
        print(f"  [{label}] ✗ task exception: {e}")
        traceback.print_exc()


def _open_wav() -> tuple:
    ts = time.strftime("%Y%m%d_%H%M%S")
    fn = os.path.join(OUT_DIR, f"cap_{ts}.wav")
    wf = wave.open(fn, "wb")
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(SAMPLE_RATE)
    return wf, fn


async def push_state_loop(ws):
    """Periodic task: poll Claude/Codex adapters, send compact JSON to the device.

    Both adapters touch disk / spawn subprocesses, so run them in a thread
    pool to avoid hijacking the event loop (which would stall WS frame I/O).

    Also fires a one-off `chime` message whenever Claude/Codex write their
    native task-complete markers, so the device can ding when a task wraps up.
    """
    # Per-connection state (one client at a time today, but scope it anyway).
    # Chime fires when the timestamp of Claude's most recent `stop_reason==end_turn`
    # message changes — that's the native "task complete" signal, so no heuristics.
    last_seen_end_turn_ts: str | None = None
    # Codex chime keys off rollout `task_complete`, not background activity.
    last_seen_codex_done: str | None = None
    codex_done_initialized = False

    while True:
        try:
            claude = await asyncio.to_thread(get_claude_state)
            codex  = await asyncio.to_thread(get_codex_state)
            todos = claude.get("todos", []) or []
            done = sum(1 for t in todos if t.get("status") == "completed")
            # Most-recent completed task → device's "done" card.
            # In-progress task → device's "NEXT" row.
            # Raw text (incl. CJK) is sent — device's font lacks Chinese glyphs so
            # it'll render placeholders for now; CJK font is a follow-up.
            recent_done = ""
            for t in todos:
                if t.get("status") == "completed":
                    recent_done = t.get("content", "") or ""
            current = ""
            for t in todos:
                if t.get("status") == "in_progress":
                    current = t.get("content", "") or ""
                    break
            # Per-todo status string for the device's dot matrix.
            # c=completed, i=in_progress, p=pending (else)
            statuses = ""
            for t in todos[:24]:  # cap so payload stays small
                s = t.get("status")
                statuses += "c" if s == "completed" else ("i" if s == "in_progress" else "p")

            # Arc fill percentages — wire formerly-decorative arcs to real data.
            CONTEXT_WINDOW = 200_000              # tokens; matches Opus/Sonnet 4.x
            SESSION_FULL_MIN = 240                 # 4 h session = full MODE arc
            ctx_pct = min(100, int(claude.get("latest_input_tokens", 0) * 100 / CONTEXT_WINDOW)) if claude.get("latest_input_tokens") else 0
            session_min = 0
            first_ts, last_ts = claude.get("first_turn_ts"), claude.get("last_turn_ts")
            if first_ts and last_ts:
                try:
                    import datetime
                    f = datetime.datetime.fromisoformat(first_ts.replace("Z", "+00:00"))
                    l = datetime.datetime.fromisoformat(last_ts.replace("Z", "+00:00"))
                    session_min = int((l - f).total_seconds() / 60)
                except Exception:
                    pass
            session_pct = min(100, session_min * 100 // SESSION_FULL_MIN)
            # Codex EFFORT arc — map level → fill %.
            effort = (codex.get("codex_effort") or "").lower()
            effort_pct = {"high": 100, "medium": 60, "low": 30}.get(effort, 0)

            payload = {
                "model":                    claude.get("model") or "-",
                "tokens_out":               claude.get("total_output", 0),
                "cost_usd":                 round(claude.get("estimated_cost_usd", 0.0), 2),
                "busy":                     bool(claude.get("busy", False)),
                "claude_ctx_pct":           ctx_pct,
                "claude_session_pct":       session_pct,
                "claude_session_min":       session_min,
                "codex_effort_pct":         effort_pct,
                "todos_done":               done,
                "todos_total":              len(todos),
                "todos_current":            current,
                "todos_done_text":          recent_done,
                "todos_statuses":           statuses,
                "codex_today_min":          int(codex.get("today_minutes", 0)),
                "codex_today_pct":          int(codex.get("today_pct", 0)),
                "codex_week_min":           int(codex.get("week_minutes", 0)),
                "codex_week_pct":           int(codex.get("week_pct", 0)),
                "codex_5h_left_pct":        codex.get("five_hour_left_pct"),
                "codex_5h_used_pct":        codex.get("five_hour_used_pct"),
                "codex_5h_reset":           codex.get("five_hour_reset", ""),
                "codex_week_left_pct":      codex.get("codex_week_left_pct"),
                "codex_week_used_pct":      codex.get("codex_week_used_pct"),
                "codex_week_reset":         codex.get("codex_week_reset", ""),
                "codex_model":              codex.get("codex_model", ""),
                "codex_effort":             codex.get("codex_effort", ""),
                "claude_5h_used_pct":       claude.get("five_hour_used_pct"),
                "claude_5h_reset":          claude.get("five_hour_reset", ""),
                "claude_5h_reset_abs":      claude.get("five_hour_reset_abs", ""),
                "claude_7d_used_pct":       claude.get("seven_day_used_pct"),
                "claude_7d_reset":          claude.get("seven_day_reset", ""),
                "claude_7d_reset_abs":      claude.get("seven_day_reset_abs", ""),
                "claude_today_cost_usd":    round(claude.get("today_cost_usd", 0.0), 2),
                "claude_today_tokens":      int(claude.get("today_output_tokens", 0)),
                "mac_hhmm":                 time.strftime("%H:%M"),
                "codex_5h_reset_abs":       codex.get("five_hour_reset_abs", ""),
                "codex_week_reset_abs":     codex.get("codex_week_reset_abs", ""),
                "codex_status_ok":          codex.get("codex_status_ok", False),
                "transcript":               "",
            }
            await ws.send(_json.dumps(payload))

            # Fire Claude chime ONLY when end_turn ts moves STRICTLY FORWARD.
            # claude_adapter picks "latest JSONL" by mtime, which can flip between
            # multiple active sessions — that made ts go backwards between polls
            # and we re-chimed. ISO timestamps sort lexicographically so plain >.
            cur_end_turn_ts = claude.get("last_end_turn_ts")
            if last_seen_end_turn_ts is None:
                last_seen_end_turn_ts = cur_end_turn_ts
            elif cur_end_turn_ts and cur_end_turn_ts > last_seen_end_turn_ts:
                await ws.send(_json.dumps({"type": "chime", "agent": "claude"}))
                print(f"[{time.strftime('%H:%M:%S')}] 🔔 claude done ({cur_end_turn_ts})")
                last_seen_end_turn_ts = cur_end_turn_ts

            cur_codex_done = await asyncio.to_thread(_read_codex_completion_signal)
            if not codex_done_initialized:
                last_seen_codex_done = cur_codex_done
                codex_done_initialized = True
            elif cur_codex_done and cur_codex_done != last_seen_codex_done:
                await ws.send(_json.dumps({"type": "chime", "agent": "codex"}))
                print(f"[{time.strftime('%H:%M:%S')}] 🔔 codex done ({cur_codex_done})")
                last_seen_codex_done = cur_codex_done
        except (websockets.exceptions.ConnectionClosed, asyncio.CancelledError):
            return
        except Exception as e:
            print(f"  push_state_loop: {e}")
        await asyncio.sleep(2.0)


async def handler(ws):
    """Push-to-talk protocol:
      - Text frame {"type":"start"}  → open a new WAV
      - Binary frames                → PCM samples appended to current WAV
      - Text frame {"type":"end"}    → close WAV, fire ASR
    No timer-based segmentation; we trust the device's button events.
    """
    import json
    global _last_transcript
    addr = ws.remote_address
    print(f"\n[{time.strftime('%H:%M:%S')}] ← client {addr}  (push-to-talk mode)")

    # State pusher: poll Claude/Codex adapters every 2s, ship a compact JSON to
    # the device. Was previously disabled while debugging mid-recording WS drops;
    # firmware fix (producer/consumer queue + 16KB buffer) made this safe again.
    pusher = asyncio.create_task(push_state_loop(ws))

    wf = None             # still save WAV alongside for debugging
    fn = None
    seg_bytes = 0
    seg_start = 0.0
    last_report = 0.0
    asr: Optional[StreamingASRSession] = None
    last_partial = ""

    def on_partial(text: str):
        # Mirror partials to server log (full text, CJK + all). Device has no
        # CJK font, so strip non-ASCII before sending — Chinese ASR output
        # still lives in the server log + the pasted text in VS Code.
        nonlocal last_partial
        if text == last_partial:
            return
        last_partial = text
        print(f"  [stream-asr] partial: {text}")
        ascii_only = text.encode("ascii", errors="ignore").decode("ascii")
        truncated = ascii_only[-40:] if len(ascii_only) > 40 else ascii_only
        async def _safe_send():
            try:
                await ws.send(_json.dumps({"transcript": truncated}))
            except Exception:
                pass  # device WS likely already dropped; not fatal
        asyncio.create_task(_safe_send())

    try:
        async for msg in ws:
            if isinstance(msg, str):
                try:
                    data = json.loads(msg)
                except Exception:
                    print(f"  text(unparsed): {msg!r}")
                    continue
                t = data.get("type")
                if t == "start":
                    # Open WAV (debug copy) + kick off streaming ASR in background.
                    if wf is not None:
                        wf.close()
                    wf, fn = _open_wav()
                    seg_bytes = 0
                    seg_start = time.time()
                    last_report = seg_start
                    last_partial = ""
                    print(f"[{time.strftime('%H:%M:%S')}] ▶ start → {os.path.basename(fn)} (streaming ASR)")
                    asr = StreamingASRSession()
                    asr.set_partial_callback(on_partial)
                    asr.start_background()  # non-blocking; PCM buffers until ready
                elif t == "submit":
                    print(f"[{time.strftime('%H:%M:%S')}] ↩ submit -> Enter")
                    press_enter()
                    continue
                elif t == "clear":
                    print(f"[{time.strftime('%H:%M:%S')}] ✗ clear -> Cmd+A + Delete")
                    clear_input()
                    continue
                elif t == "end":
                    if wf is not None:
                        wf.close()
                        wf = None
                    secs = seg_bytes / (SAMPLE_RATE * 2)
                    print(f"[{time.strftime('%H:%M:%S')}] ■ end ({secs:.1f}s) → finalize ASR")
                    if asr is None:
                        # streaming wasn't set up — fall back to file
                        if fn:
                            asyncio.create_task(asr_in_background(fn, secs))
                    else:
                        t0 = time.time()
                        try:
                            text = await asr.finalize(timeout_s=8.0)
                        except Exception as e:
                            text = ""
                            print(f"  ✗ finalize: {e}")
                        dt = time.time() - t0
                        app = get_active_app()
                        if text:
                            _last_transcript = text
                            print(f"  ✓ {dt:.2f}s → {text}  [→ {app}]")
                            paste_to_active_window(text)
                        else:
                            print(f"  (empty result, fallback to file ASR: {fn})")
                            if fn:
                                asyncio.create_task(asr_in_background(fn, secs))
                        asr = None
                    fn = None
                    seg_bytes = 0
                else:
                    print(f"  unknown text: {msg!r}")
                continue

            # Binary PCM frame. Save to WAV + forward to streaming ASR.
            # If device skipped `start` (observed bug), auto-open a WAV so the
            # audio still gets captured and ASR'd via fallback on disconnect/end.
            if wf is None:
                wf, fn = _open_wav()
                seg_bytes = 0
                seg_start = time.time()
                last_report = seg_start
                print(f"[{time.strftime('%H:%M:%S')}] ▶ (implicit start, no /start frame) → {os.path.basename(fn)}")
            if wf is not None:
                wf.writeframes(msg)
            if asr is not None:
                try:
                    await asr.send_audio(bytes(msg))
                except Exception as e:
                    print(f"  ✗ stream send: {e}")
            seg_bytes += len(msg)
            now = time.time()
            if now - last_report >= 0.5:
                seg_secs = seg_bytes / (SAMPLE_RATE * 2)
                rms = rms_int16(msg[-1280:])
                bar = "#" * min(40, rms * 40 // 2000) + "." * max(0, 40 - rms * 40 // 2000)
                print(f"  rec {seg_secs:4.1f}s  rms={rms:5d}  |{bar}|")
                last_report = now
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        pusher.cancel()
        # Mid-recording disconnect: device WS often drops before sending /end.
        # If streaming was running, give it a brief chance to return a result;
        # if that comes up empty (or no streaming), fall back to file ASR.
        if wf is not None:
            wf.close()
            wf = None
            secs = seg_bytes / (SAMPLE_RATE * 2)
            print(f"[{time.strftime('%H:%M:%S')}] ✕ client gone mid-recording ({secs:.1f}s)")
            text = ""
            if asr is not None:
                try:
                    text = await asr.finalize(timeout_s=3.0)
                except Exception as e:
                    print(f"  ✗ streaming finalize: {e}")
                asr = None
            if text:
                _last_transcript = text
                app = get_active_app()
                print(f"  ✓ streaming → {text}  [→ {app}]")
                paste_to_active_window(text)
            elif fn:
                print(f"  → fallback file ASR")
                asyncio.create_task(asr_in_background(fn, secs))
        else:
            if asr is not None:
                try: await asr.close()
                except Exception: pass
            print(f"[{time.strftime('%H:%M:%S')}] ✕ client gone")


# Path to the built firmware image — served at GET /firmware.bin so the
# device's esp_https_ota can pull and self-update over the LAN.
FIRMWARE_BIN = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "mic-test", "build", "mic_test.bin",
)


async def http_or_ws(connection, request):
    """websockets.serve `process_request` hook — intercepts non-WS HTTP
    requests so we can serve /firmware.bin without standing up a second port.
    Returns a Response to short-circuit; returns None to fall through to WS."""
    path = request.path
    if path == "/firmware.bin":
        try:
            with open(FIRMWARE_BIN, "rb") as f:
                data = f.read()
            print(f"[{time.strftime('%H:%M:%S')}] → /firmware.bin ({len(data)} bytes)")
            from websockets.http11 import Response
            from websockets.datastructures import Headers
            return Response(
                200, "OK",
                Headers([("Content-Type", "application/octet-stream"),
                         ("Content-Length", str(len(data)))]),
                data,
            )
        except FileNotFoundError:
            from websockets.http11 import Response
            from websockets.datastructures import Headers
            return Response(404, "Not Found", Headers(), b"firmware.bin not built\n")
    return None


async def main():
    print(f"WebSocket PCM ingest listening on ws://0.0.0.0:{PORT}/")
    print(f"Captures will be saved under {OUT_DIR}")
    print(f"OTA firmware served at GET /firmware.bin → {FIRMWARE_BIN}")
    # Codex.app focus-time tracker (every 5s polls macOS frontmost app and
    # accumulates seconds when Codex.app is on top). Persists to
    # ~/.codex/.stopwatch_usage.json. Used to feed Codex page DAY/WK rows.
    threading.Thread(target=codex_activity_daemon, daemon=True,
                     name="codex_activity").start()
    print("Press Ctrl+C to stop.\n")
    async with websockets.serve(handler, "0.0.0.0", PORT, max_size=2**20,
                                 process_request=http_or_ws):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbye")
