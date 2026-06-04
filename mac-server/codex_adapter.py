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
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any

CODEX_APP_NAMES = {"Codex"}  # macOS frontmost name.
USAGE_FILE   = os.path.expanduser("~/.codex/.stopwatch_usage.json")
CODEX_CONFIG = os.path.expanduser("~/.codex/config.toml")
CODEX_AUTH   = os.path.expanduser("~/.codex/auth.json")
CODEX_LOGS   = os.path.expanduser("~/.codex/logs_2.sqlite")
CODEX_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
POLL_INTERVAL_S = 5
STATUS_CACHE_S  = 120
QUOTA_CACHE_S   = 60   # don't hammer ChatGPT backend
HTTP_TIMEOUT    = 8

try:
    import certifi
    _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    _SSL_CTX = ssl.create_default_context()

_last_status_ts = 0.0
_last_status: dict[str, Any] = {}
_last_quota_ts = 0.0
_last_quota: dict[str, Any] = {}

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


def _read_codex_live_state() -> dict[str, Any]:
    """Pull the *active* model + reasoning_effort from the most recent turn in
    logs_2.sqlite. The Codex desktop app's reasoning-effort menu writes runtime
    state here, NOT into config.toml — so config.toml only reflects what was
    set when the app last edited the file (often months ago).
    Each turn log line contains `model=X codex.turn.reasoning_effort=Y`."""
    import sqlite3
    out: dict[str, Any] = {}
    try:
        conn = sqlite3.connect(f"file:{CODEX_LOGS}?mode=ro", uri=True, timeout=1.0)
        cur = conn.execute(
            "SELECT feedback_log_body FROM logs "
            "WHERE feedback_log_body LIKE '%codex.turn.reasoning_effort=%' "
            "ORDER BY id DESC LIMIT 1"
        )
        row = cur.fetchone()
        conn.close()
        if row and row[0]:
            body = row[0]
            m = re.search(r"codex\.turn\.reasoning_effort=(\w+)", body)
            if m: out["effort"] = m.group(1).lower()
            m = re.search(r"\bmodel=([\w.\-]+)", body)
            if m: out["model"] = m.group(1)
    except Exception:
        pass
    return out


def _query_codex_status() -> dict[str, Any]:
    """Live state from sqlite (authoritative) layered on top of config.toml."""
    base = _read_codex_config()
    live = _read_codex_live_state()
    if live.get("model"):  base["model"]  = live["model"]
    if live.get("effort"): base["effort"] = live["effort"]
    base["raw_ok"] = "model" in base
    return base


# ---------------------------------------------------------------------------
# Codex 5h / weekly quota — hit chatgpt.com/backend-api/wham/usage with the
# access token Codex desktop wrote to ~/.codex/auth.json. Approach borrowed
# from alexjc-tech/cc-island.
# ---------------------------------------------------------------------------
def _parse_reset_at(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    try:
        s = str(value).replace("Z", "+00:00")
        return int(datetime.datetime.fromisoformat(s).timestamp())
    except (ValueError, TypeError):
        return None


def _http_get_json(url: str, headers: dict[str, str]) -> tuple[int, Any]:
    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT, context=_SSL_CTX) as r:
            body = r.read()
            try:
                return r.status, json.loads(body)
            except json.JSONDecodeError:
                return r.status, None
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, None
    except Exception as e:
        return 0, {"_transport_error": str(e)}


def _fetch_codex_quota_uncached() -> dict[str, Any]:
    """Call wham/usage and shape into our state fields. Returns {} on failure."""
    try:
        with open(CODEX_AUTH) as f:
            tokens = json.load(f).get("tokens") or {}
        token = tokens.get("access_token")
    except (OSError, json.JSONDecodeError):
        return {}
    if not token:
        return {}

    status, obj = _http_get_json(
        CODEX_USAGE_URL,
        headers={"Authorization": f"Bearer {token}"},
    )
    if status != 200 or not isinstance(obj, dict):
        return {}

    rl = obj.get("rate_limit") or {}
    out: dict[str, Any] = {"plan": obj.get("plan_type")}

    def _shape(window: dict[str, Any]) -> tuple[int | None, str, str]:
        used = window.get("used_percent")
        if isinstance(used, (int, float)):
            used = int(used)
        else:
            used = None
        reset_epoch = _parse_reset_at(window.get("reset_at"))
        reset_str = ""
        abs_str = ""
        if reset_epoch:
            dt = datetime.datetime.fromtimestamp(reset_epoch)
            now_dt = datetime.datetime.now()
            same_day = dt.date() == now_dt.date()
            secs = reset_epoch - int(time.time())
            if secs > 0:
                if secs >= 86400:    reset_str = f"in {secs // 86400}d"
                elif secs >= 3600:   reset_str = f"in {secs // 3600}h {(secs % 3600) // 60}m"
                else:                reset_str = f"in {secs // 60}m"
            # Absolute time the mockup uses: HH:MM if same day, "M/D HH:MM" otherwise
            abs_str = dt.strftime("%H:%M") if same_day else dt.strftime("%-m/%-d %H:%M")
        return used, reset_str, abs_str

    p5_used, p5_rel, p5_abs = _shape(rl.get("primary_window") or {})
    pw_used, pw_rel, pw_abs = _shape(rl.get("secondary_window") or {})
    out["five_hour_used_pct"] = p5_used
    out["five_hour_reset"]    = p5_rel
    out["five_hour_reset_abs"] = p5_abs
    out["week_used_pct"]      = pw_used
    out["week_reset"]         = pw_rel
    out["week_reset_abs"]     = pw_abs
    return out


def _get_cached_codex_quota() -> dict[str, Any]:
    global _last_quota_ts, _last_quota
    now = time.time()
    if _last_quota and now - _last_quota_ts < QUOTA_CACHE_S:
        return _last_quota
    fresh = _fetch_codex_quota_uncached()
    if fresh:
        _last_quota = fresh
        _last_quota_ts = now
    return _last_quota


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
    status = _get_cached_codex_status()         # config.toml → model + effort
    quota  = _get_cached_codex_quota()           # wham/usage  → real 5h / week %
    p5_used = quota.get("five_hour_used_pct")
    pw_used = quota.get("week_used_pct")
    return {
        "today_minutes":         today_min,
        "today_budget_minutes":  DAILY_BUDGET_MIN,
        "today_pct":             min(100, today_min * 100 // max(1, DAILY_BUDGET_MIN)),
        "week_minutes":          week_min,
        "week_budget_minutes":   WEEKLY_BUDGET_MIN,
        "week_pct":              min(100, week_min * 100 // max(1, WEEKLY_BUDGET_MIN)),
        "five_hour_left_pct":    (100 - p5_used) if isinstance(p5_used, int) else None,
        "five_hour_used_pct":    p5_used,
        "five_hour_reset":       quota.get("five_hour_reset", ""),
        "five_hour_reset_abs":   quota.get("five_hour_reset_abs", ""),
        "codex_week_left_pct":   (100 - pw_used) if isinstance(pw_used, int) else None,
        "codex_week_used_pct":   pw_used,
        "codex_week_reset":      quota.get("week_reset", ""),
        "codex_week_reset_abs":  quota.get("week_reset_abs", ""),
        "codex_model":           status.get("model", ""),
        "codex_effort":          status.get("effort", ""),
        "codex_status_ok":       bool(status.get("raw_ok")),
        "codex_status_error":    status.get("error", ""),
        "codex_plan":            quota.get("plan", ""),
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
