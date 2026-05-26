"""
Codex (the Electron desktop app) activity tracker.

No public quota API exists. Instead we poll macOS for the frontmost app
and accumulate "Codex.app focused" seconds into a daily + weekly counter,
persisted in ~/.codex/.stopwatch_usage.json.

Run as a daemon (`python3 codex_adapter.py --daemon`) — pings every 5s.
Other code calls `get_state()` to read current totals.
"""
from __future__ import annotations

import datetime
import json
import os
import re
import subprocess
import sys
import time
from typing import Any

CODEX_APP_NAMES = {"Codex"}  # macOS frontmost name.
USAGE_FILE = os.path.expanduser("~/.codex/.stopwatch_usage.json")
CODEX_CONFIG = os.path.expanduser("~/.codex/config.toml")
POLL_INTERVAL_S = 5
STATUS_CACHE_S = 120

_last_status_ts = 0.0
_last_status: dict[str, Any] = {}

# Daily/weekly Codex.app focus-time budgets so the device UI bars have a scale.
DAILY_BUDGET_MIN  = 60 * 5    # 5 hours
WEEKLY_BUDGET_MIN = 60 * 25   # 25 hours


def _read_codex_config() -> dict[str, Any]:
    """Parse ~/.codex/config.toml. The v3 Codex app stopped exposing /status
    via the CLI; the config.toml is the authoritative source for the user's
    active model + reasoning effort. Quota fields (5h/week pct) currently
    have no public local source — populated only if we later wire a different
    pipeline (e.g. proxying the app's HTTP rate-limit calls)."""
    out: dict[str, Any] = {"raw_ok": False, "checked_at": int(time.time())}
    try:
        with open(CODEX_CONFIG, "r") as f:
            text = f.read()
    except FileNotFoundError:
        out["error"] = f"{CODEX_CONFIG} not found"
        return out
    except Exception as e:
        out["error"] = f"read config: {e}"
        return out

    m = re.search(r'^\s*model\s*=\s*"([^"]+)"', text, re.M)
    if m:
        out["model"] = m.group(1).strip()
    m = re.search(r'^\s*model_reasoning_effort\s*=\s*"([^"]+)"', text, re.M)
    if m:
        out["effort"] = m.group(1).strip().lower()

    # raw_ok = at least model resolved
    out["raw_ok"] = "model" in out
    return out


def _query_codex_status() -> dict[str, Any]:
    """Back-compat wrapper. Real CLI-based query is gone; we read config.toml."""
    return _read_codex_config()


def _get_cached_codex_status() -> dict[str, Any]:
    global _last_status_ts, _last_status
    now = time.time()
    if _last_status and now - _last_status_ts < STATUS_CACHE_S:
        return _last_status
    try:
        status = _query_codex_status()
        if status.get("raw_ok"):
            _last_status = status
            _last_status_ts = now
            return status
    except Exception as e:
        return {"raw_ok": False, "error": str(e), "checked_at": int(now)}
    return {"raw_ok": False, "checked_at": int(now)}


def _frontmost_app() -> str:
    """Return macOS frontmost app name, or '' on failure."""
    try:
        out = subprocess.check_output(
            [
                "osascript",
                "-e",
                'tell application "System Events" to name of first application process whose frontmost is true',
            ],
            stderr=subprocess.DEVNULL,
            timeout=2,
        )
        return out.decode("utf-8").strip()
    except Exception:
        return ""


def _week_key(ts: float) -> str:
    """ISO week-start date (Monday)."""
    dt = datetime.datetime.fromtimestamp(ts)
    monday = dt - datetime.timedelta(days=dt.weekday())
    return monday.strftime("%Y-%m-%d")


def _day_key(ts: float) -> str:
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d")


def _load() -> dict[str, Any]:
    try:
        with open(USAGE_FILE, "r") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def _save(state: dict[str, Any]) -> None:
    os.makedirs(os.path.dirname(USAGE_FILE), exist_ok=True)
    tmp = USAGE_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, USAGE_FILE)


def daemon_loop() -> None:
    print(f"Codex activity tracker — polling every {POLL_INTERVAL_S}s")
    print(f"Persisting to {USAGE_FILE}")
    state = _load()
    state.setdefault("days",  {})  # day -> seconds
    state.setdefault("weeks", {})  # ISO Monday -> seconds
    state.setdefault("last_seen_ts", time.time())

    while True:
        time.sleep(POLL_INTERVAL_S)
        now = time.time()
        front = _frontmost_app()
        is_codex = front in CODEX_APP_NAMES
        if is_codex:
            dk = _day_key(now)
            wk = _week_key(now)
            state["days"][dk]  = state["days"].get(dk,  0) + POLL_INTERVAL_S
            state["weeks"][wk] = state["weeks"].get(wk, 0) + POLL_INTERVAL_S
        state["last_seen_ts"] = now
        state["last_front"]   = front
        # Save once per minute, not per tick.
        if int(now) % 60 < POLL_INTERVAL_S:
            _save(state)
            today = state["days"].get(_day_key(now), 0) // 60
            week  = state["weeks"].get(_week_key(now), 0) // 60
            print(f"  [{datetime.datetime.fromtimestamp(now):%H:%M:%S}]  today={today}min  week={week}min  (front={front!r}{' ✓Codex' if is_codex else ''})")


def get_state() -> dict[str, Any]:
    """Read-only snapshot for other modules / UI feed."""
    raw = _load()
    now = time.time()
    today_min = raw.get("days",  {}).get(_day_key(now),  0) // 60
    week_min  = raw.get("weeks", {}).get(_week_key(now), 0) // 60
    status = _get_cached_codex_status()
    return {
        "today_minutes":         today_min,
        "today_budget_minutes":  DAILY_BUDGET_MIN,
        "today_pct":             min(100, today_min * 100 // max(1, DAILY_BUDGET_MIN)),
        "week_minutes":          week_min,
        "week_budget_minutes":   WEEKLY_BUDGET_MIN,
        "week_pct":              min(100, week_min * 100 // max(1, WEEKLY_BUDGET_MIN)),
        "five_hour_left_pct":     status.get("five_hour_left_pct"),
        "five_hour_used_pct":     status.get("five_hour_used_pct"),
        "five_hour_reset":        status.get("five_hour_reset", ""),
        "codex_week_left_pct":    status.get("week_left_pct"),
        "codex_week_used_pct":    status.get("week_used_pct"),
        "codex_week_reset":       status.get("week_reset", ""),
        "codex_model":            status.get("model", ""),
        "codex_effort":           status.get("effort", ""),
        "codex_status_ok":        bool(status.get("raw_ok")),
        "codex_status_error":     status.get("error", ""),
        "last_seen_ts":          raw.get("last_seen_ts"),
        "last_front":            raw.get("last_front"),
    }


def pretty_print(s: dict[str, Any]) -> None:
    bar_today = "#" * (s["today_pct"] * 40 // 100) + "." * (40 - s["today_pct"] * 40 // 100)
    bar_week  = "#" * (s["week_pct"]  * 40 // 100) + "." * (40 - s["week_pct"]  * 40 // 100)
    print(f"--- Codex 活动 ---")
    print(f"  今日: {s['today_minutes']:>3}/{s['today_budget_minutes']} min  ({s['today_pct']:>3}%)  |{bar_today}|")
    print(f"  本周: {s['week_minutes']:>3}/{s['week_budget_minutes']} min  ({s['week_pct']:>3}%)  |{bar_week}|")
    if s.get("codex_status_ok"):
        print(f"  5h limit: {s['five_hour_left_pct']}% left, resets {s.get('five_hour_reset')}")
        print(f"  Weekly:   {s['codex_week_left_pct']}% left, resets {s.get('codex_week_reset')}")
    else:
        print(f"  Codex status: unavailable {s.get('codex_status_error', '')}")
    if s.get("last_front"):
        print(f"  最近前台: {s['last_front']}")


if __name__ == "__main__":
    if "--daemon" in sys.argv:
        try:
            daemon_loop()
        except KeyboardInterrupt:
            print("\nbye")
    else:
        pretty_print(get_state())
