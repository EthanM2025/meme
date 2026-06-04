"""
Claude Code state adapter — reads ~/.claude/projects/<project>/<session>.jsonl
and surfaces:
  - latest model
  - cumulative token usage (input / cache / output)
  - cost estimate
  - latest TodoWrite list (for the mockup's "task queue" widget)
  - busy/idle (last record was assistant-with-tool_use → busy)

Designed to be polled (`get_state()`) — no FS-watcher dependency.
"""
from __future__ import annotations

import datetime
import glob
import json
import os
import ssl
import subprocess
import time
import urllib.error
import urllib.request
from typing import Any, Optional

CLAUDE_PROJECTS  = os.path.expanduser("~/.claude/projects")
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CLAUDE_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
CLAUDE_OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"  # Claude Code public client id
CLAUDE_USER_AGENT = "claude-code/2.1.121"
HTTP_TIMEOUT  = 8
QUOTA_CACHE_S = 60

try:
    import certifi
    _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    _SSL_CTX = ssl.create_default_context()

_last_quota_ts = 0.0
_last_quota: dict[str, Any] = {}

# Per-1M-token USD rates. Table refreshed from alexjc-tech/cc-island (mid-2026).
# Includes the gpt-5.x family for when we eventually parse Codex session logs.
# Tuple order matches cc-island for easy cross-reference: (in, out, cache_create, cache_read).
MODEL_PRICING = {
    "claude-opus-4-8":      {"in":  5.0, "cache_read": 0.50, "cache_create": 6.25, "out": 25.0},
    "claude-opus-4-7":      {"in":  5.0, "cache_read": 0.50, "cache_create": 6.25, "out": 25.0},
    "claude-opus-4-6":      {"in":  5.0, "cache_read": 0.50, "cache_create": 6.25, "out": 25.0},
    "claude-opus-4-5":      {"in":  5.0, "cache_read": 0.50, "cache_create": 6.25, "out": 25.0},
    "claude-sonnet-4-6":    {"in":  3.0, "cache_read": 0.30, "cache_create": 3.75, "out": 15.0},
    "claude-sonnet-4-5":    {"in":  3.0, "cache_read": 0.30, "cache_create": 3.75, "out": 15.0},
    "claude-haiku-4-5":     {"in":  1.0, "cache_read": 0.10, "cache_create": 1.25, "out":  5.0},
    "claude-haiku-4-5-20251001": {"in": 1.0, "cache_read": 0.10, "cache_create": 1.25, "out": 5.0},
    # Codex / OpenAI side — kept here for when claude_adapter eventually
    # also reads ~/.codex/logs_*.sqlite for combined cost tracking.
    "gpt-5.5":              {"in":  5.0,  "cache_read": 0.50,  "cache_create": 5.0,   "out": 30.0},
    "gpt-5.4":              {"in":  2.5,  "cache_read": 0.25,  "cache_create": 2.5,   "out": 15.0},
    "gpt-5.2":              {"in":  1.75, "cache_read": 0.175, "cache_create": 1.75,  "out": 14.0},
    "gpt-5.4-mini":         {"in":  0.75, "cache_read": 0.075, "cache_create": 0.75,  "out":  4.5},
    "gpt-5-codex":          {"in":  1.25, "cache_read": 0.125, "cache_create": 1.25,  "out": 10.0},
}


def find_latest_session() -> str | None:
    """Pick the *.jsonl with the most recent mtime across all projects."""
    files = glob.glob(os.path.join(CLAUDE_PROJECTS, "*", "*.jsonl"))
    if not files:
        return None
    return max(files, key=os.path.getmtime)


def _model_cost_usd(model: str, in_tok: int, cache_create: int, cache_read: int, out: int) -> float:
    p = MODEL_PRICING.get(model)
    if not p:
        return 0.0
    return (
        in_tok       / 1_000_000 * p["in"]
        + cache_create / 1_000_000 * p["cache_create"]
        + cache_read   / 1_000_000 * p["cache_read"]
        + out          / 1_000_000 * p["out"]
    )


def parse_session(jsonl_path: str) -> dict:
    """Read the whole session log and aggregate."""
    state: dict[str, Any] = {
        "session_path": jsonl_path,
        "model": None,
        "total_input": 0,
        "total_cache_create": 0,
        "total_cache_read": 0,
        "total_output": 0,
        "estimated_cost_usd": 0.0,
        "today_output_tokens": 0,
        "today_cost_usd": 0.0,
        "turn_count": 0,
        "first_turn_ts": None,
        "last_turn_ts": None,
        "latest_input_tokens": 0,  # input + cache_read of the most recent turn (approx context window use)
        "todos": [],
        "busy": False,            # True if the latest assistant turn used a tool (i.e. work in progress)
    }
    last_assistant_tool_use = False
    # Native "task complete" signal from Claude Code: the last assistant
    # message whose stop_reason == "end_turn" (i.e. Claude finished talking
    # rather than pausing for a tool call). We track its timestamp so the
    # server can detect *new* completions across polls.
    last_end_turn_ts: str | None = None
    today_midnight = datetime.datetime.now().replace(
        hour=0, minute=0, second=0, microsecond=0).timestamp()

    with open(jsonl_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue

            if d.get("type") == "assistant":
                m = d.get("message", {})
                model = m.get("model")
                if model:
                    state["model"] = model
                usage = m.get("usage", {}) or {}
                in_t   = usage.get("input_tokens", 0) or 0
                cc     = usage.get("cache_creation_input_tokens", 0) or 0
                cr     = usage.get("cache_read_input_tokens", 0) or 0
                out_t  = usage.get("output_tokens", 0) or 0
                state["total_input"]        += in_t
                state["total_cache_create"] += cc
                state["total_cache_read"]   += cr
                state["total_output"]       += out_t
                state["estimated_cost_usd"] += _model_cost_usd(model or "", in_t, cc, cr, out_t)
                state["turn_count"] += 1
                ts = d.get("timestamp")
                if ts:
                    if state["first_turn_ts"] is None:
                        state["first_turn_ts"] = ts
                    state["last_turn_ts"] = ts
                # Today-only spend tally (filter by timestamp >= midnight).
                if ts:
                    try:
                        turn_ts = datetime.datetime.fromisoformat(ts.replace("Z", "+00:00"))
                        if turn_ts.astimezone().timestamp() >= today_midnight:
                            state["today_output_tokens"] += out_t
                            state["today_cost_usd"] += _model_cost_usd(
                                model or "", in_t, cc, cr, out_t)
                    except (ValueError, TypeError):
                        pass
                # Snapshot of context window use as of the latest turn.
                state["latest_input_tokens"] = in_t + cr

                # Detect tool use → "busy" hint.
                content = m.get("content") or []
                last_assistant_tool_use = any(
                    isinstance(b, dict) and b.get("type") == "tool_use" for b in content
                )
                # Native task-complete signal — record timestamp of every end_turn.
                if m.get("stop_reason") == "end_turn" and ts:
                    last_end_turn_ts = ts

            elif d.get("type") == "user":
                # If a user message comes in after an assistant tool_use, it's likely a tool_result.
                # We treat "non-tool-result user message" as resetting busy. For simplicity, any
                # later user/system message resets the flag — the FE is just showing a hint.
                m = d.get("message", {})
                content = m.get("content") or []
                if isinstance(content, list):
                    has_tool_result = any(
                        isinstance(b, dict) and b.get("type") == "tool_result" for b in content
                    )
                else:
                    has_tool_result = False
                if not has_tool_result:
                    last_assistant_tool_use = False

                # TodoWrite tool inputs are in assistant tool_use blocks (above). But the latest
                # *active* todo list comes from scanning all tool_use entries for TodoWrite. Done
                # in a second pass via toolUseResult — but simplest is to also pick from
                # tool_use input on the assistant side.

    state["busy"] = last_assistant_tool_use
    state["last_end_turn_ts"] = last_end_turn_ts

    # Second pass — pluck the latest TodoWrite tool_use's input.todos.
    # We do a backwards scan from EOF.
    with open(jsonl_path, "rb") as f:
        try:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            chunk = 65536
            tail = b""
            pos = size
            while pos > 0 and len(tail) < 1_000_000:
                read_n = min(chunk, pos)
                pos -= read_n
                f.seek(pos)
                tail = f.read(read_n) + tail
                if b"TodoWrite" in tail:
                    break
        except Exception:
            tail = b""

    todos = []
    for line in tail.split(b"\n"):
        if b"TodoWrite" not in line:
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") != "assistant":
            continue
        for block in (d.get("message", {}).get("content") or []):
            if isinstance(block, dict) and block.get("type") == "tool_use" and block.get("name") == "TodoWrite":
                inp = block.get("input", {}) or {}
                todos = inp.get("todos", []) or []
    state["todos"] = todos

    return state


# ---------------------------------------------------------------------------
# Claude Code 5h / 7d quota — hit api.anthropic.com/api/oauth/usage with the
# OAuth token Claude Code stashed in the macOS keychain. Approach borrowed
# from alexjc-tech/cc-island.
# ---------------------------------------------------------------------------
def _security(args: list[str]) -> Optional[subprocess.CompletedProcess]:
    try:
        return subprocess.run(["/usr/bin/security", *args],
                              capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None


def _claude_keychain_account() -> Optional[str]:
    out = _security(["find-generic-password", "-s", "Claude Code-credentials"])
    if not out or out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        line = line.strip()
        if not line.startswith('"acct"'):
            continue
        if "=" not in line:
            return None
        v = line.split("=", 1)[1]
        if v.startswith('"') and v.endswith('"') and len(v) >= 2:
            return v[1:-1] or None
    return None


def _read_claude_creds() -> Optional[dict]:
    account = _claude_keychain_account()
    if not account:
        return None
    out = _security(["find-generic-password", "-s", "Claude Code-credentials",
                     "-a", account, "-w"])
    if not out or out.returncode != 0:
        return None
    try:
        oauth = (json.loads(out.stdout.strip()).get("claudeAiOauth") or {})
    except json.JSONDecodeError:
        return None
    if not oauth.get("accessToken") or not oauth.get("refreshToken"):
        return None
    return {"account": account, "oauth": oauth}


def _write_claude_creds(account: str, oauth: dict) -> bool:
    payload = json.dumps({"claudeAiOauth": oauth})
    out = _security(["add-generic-password", "-U",
                     "-s", "Claude Code-credentials", "-a", account, "-w", payload])
    return bool(out and out.returncode == 0)


def _http(method: str, url: str, headers: dict, body=None) -> tuple[int, Any]:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT, context=_SSL_CTX) as r:
            raw = r.read()
            try:
                return r.status, json.loads(raw)
            except json.JSONDecodeError:
                return r.status, None
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, None
    except Exception as e:
        return 0, {"_transport_error": str(e)}


def _refresh_claude_token(refresh_token: str) -> Optional[dict]:
    status, obj = _http(
        "POST", CLAUDE_TOKEN_URL,
        headers={"Content-Type": "application/json"},
        body={"grant_type": "refresh_token",
              "refresh_token": refresh_token,
              "client_id": CLAUDE_OAUTH_CLIENT_ID},
    )
    if status != 200 or not isinstance(obj, dict):
        return None
    if not obj.get("access_token") or not obj.get("refresh_token"):
        return None
    expires_in = obj.get("expires_in") or 28800
    return {
        "access_token":  obj["access_token"],
        "refresh_token": obj["refresh_token"],
        "expires_at":    int((time.time() + expires_in) * 1000),
    }


def _parse_reset_at(value: Any) -> Optional[int]:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    try:
        s = str(value).replace("Z", "+00:00")
        return int(datetime.datetime.fromisoformat(s).timestamp())
    except (ValueError, TypeError):
        return None


def _format_reset(reset_epoch: Optional[int]) -> str:
    if not reset_epoch:
        return ""
    secs = reset_epoch - int(time.time())
    if secs <= 0:
        return ""
    if secs >= 86400:  return f"in {secs // 86400}d"
    if secs >= 3600:   return f"in {secs // 3600}h {(secs % 3600) // 60}m"
    return f"in {secs // 60}m"


def _format_reset_abs(reset_epoch: Optional[int]) -> str:
    if not reset_epoch:
        return ""
    dt = datetime.datetime.fromtimestamp(reset_epoch)
    if dt.date() == datetime.datetime.now().date():
        return dt.strftime("%H:%M")
    return dt.strftime("%-m/%-d %H:%M")


def _probe_claude_usage(token: str) -> tuple[str, Any]:
    """Return (kind, value). kind ∈ {ok, scope, err}."""
    status, obj = _http(
        "GET", CLAUDE_USAGE_URL,
        headers={
            "Authorization":   f"Bearer {token}",
            "anthropic-beta":  "oauth-2025-04-20",
            "Accept":          "application/json",
            "Content-Type":    "application/json",
            "User-Agent":      CLAUDE_USER_AGENT,
        },
    )
    if status == 401:                              return "err", "unauthorized"
    if status == 403:                              return "scope", "re-login"
    if status == 429:                              return "err", "rate limited"
    if status != 200 or not isinstance(obj, dict): return "err", f"http {status}"
    if isinstance(obj.get("error"), dict) and obj["error"].get("type") == "rate_limit_error":
        return "err", "rate limited"

    def _win(key):
        d = obj.get(key) or {}
        raw = d.get("utilization", d.get("used_percent", 0)) or 0
        used = int(round(float(raw))) if isinstance(raw, (int, float)) else 0
        reset_epoch = _parse_reset_at(d.get("resets_at"))
        return {"used_pct": used,
                "reset": _format_reset(reset_epoch),
                "reset_abs": _format_reset_abs(reset_epoch)}

    return "ok", {"five_hour": _win("five_hour"), "seven_day": _win("seven_day")}


def _fetch_claude_quota_uncached() -> dict:
    """env token → keychain token → refresh + retry. Returns {} on failure."""
    # 1) env override (Claude Desktop sets this for child procs)
    env_token = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN")
    if env_token:
        kind, val = _probe_claude_usage(env_token)
        if kind == "ok":
            return val

    creds = _read_claude_creds()
    if not creds:
        return {}
    oauth = creds["oauth"]

    # 2) keychain access token
    kind, val = _probe_claude_usage(oauth["accessToken"])
    if kind == "ok":
        return val

    # 3) refresh + retry
    refreshed = _refresh_claude_token(oauth["refreshToken"])
    if not refreshed:
        return {}
    oauth = dict(oauth)
    oauth["accessToken"]  = refreshed["access_token"]
    oauth["refreshToken"] = refreshed["refresh_token"]
    oauth["expiresAt"]    = refreshed["expires_at"]
    _write_claude_creds(creds["account"], oauth)

    kind, val = _probe_claude_usage(refreshed["access_token"])
    return val if kind == "ok" else {}


def _get_cached_claude_quota() -> dict:
    global _last_quota_ts, _last_quota
    now = time.time()
    if _last_quota and now - _last_quota_ts < QUOTA_CACHE_S:
        return _last_quota
    fresh = _fetch_claude_quota_uncached()
    if fresh:
        _last_quota = fresh
        _last_quota_ts = now
    return _last_quota


def get_state() -> dict:
    fn = find_latest_session()
    if not fn:
        base = {"error": "no session log found"}
    else:
        base = parse_session(fn)
    # Stitch in real 5h / 7d quota (from oauth/usage).
    q = _get_cached_claude_quota()
    if q:
        base["five_hour_used_pct"]    = q.get("five_hour", {}).get("used_pct")
        base["five_hour_reset"]       = q.get("five_hour", {}).get("reset", "")
        base["five_hour_reset_abs"]   = q.get("five_hour", {}).get("reset_abs", "")
        base["seven_day_used_pct"]    = q.get("seven_day", {}).get("used_pct")
        base["seven_day_reset"]       = q.get("seven_day", {}).get("reset", "")
        base["seven_day_reset_abs"]   = q.get("seven_day", {}).get("reset_abs", "")
    return base


def pretty_print(state: dict) -> None:
    print(f"--- Claude Code session  ({state.get('session_path','?').split('/')[-1]}) ---")
    print(f"  model:  {state.get('model')}")
    print(f"  turns:  {state.get('turn_count')}    busy: {state.get('busy')}")
    print(f"  in:     {state.get('total_input'):>10,}  tokens")
    print(f"  cc:     {state.get('total_cache_create'):>10,}  tokens (cache create)")
    print(f"  cr:     {state.get('total_cache_read'):>10,}  tokens (cache read)")
    print(f"  out:    {state.get('total_output'):>10,}  tokens")
    print(f"  cost ≈  ${state.get('estimated_cost_usd', 0):.2f} USD")
    todos = state.get("todos", [])
    if todos:
        done = sum(1 for t in todos if t.get("status") == "completed")
        in_prog = sum(1 for t in todos if t.get("status") == "in_progress")
        print(f"  todos:  {done}/{len(todos)} done, {in_prog} in progress")
        for t in todos[:5]:
            mark = "✓" if t.get("status") == "completed" else ("▸" if t.get("status") == "in_progress" else "·")
            print(f"    {mark}  {t.get('content','')}")
        if len(todos) > 5:
            print(f"    ... +{len(todos)-5} more")
    else:
        print(f"  todos:  (none in recent history)")


if __name__ == "__main__":
    import sys
    if "--watch" in sys.argv:
        while True:
            os.system("clear")
            pretty_print(get_state())
            time.sleep(3)
    else:
        pretty_print(get_state())
