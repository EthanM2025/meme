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

import glob
import json
import os
import time
from typing import Any, Optional

CLAUDE_PROJECTS = os.path.expanduser("~/.claude/projects")

# Rough per-1M-token pricing in USD for Claude API public list prices (2026-05-25).
# These are approximate — Claude Code subscription pricing differs but this gives a
# usage-cost feel that's useful on the device. Update as Anthropic publishes new rates.
MODEL_PRICING = {
    "claude-opus-4-7":      {"in":  15.0, "cache_read": 1.5,  "cache_create": 18.75, "out": 75.0},
    "claude-sonnet-4-6":    {"in":   3.0, "cache_read": 0.3,  "cache_create":  3.75, "out": 15.0},
    "claude-haiku-4-5":     {"in":   1.0, "cache_read": 0.1,  "cache_create":  1.25, "out":  5.0},
    "claude-haiku-4-5-20251001": {"in": 1.0, "cache_read": 0.1, "cache_create": 1.25, "out": 5.0},
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
        "turn_count": 0,
        "last_turn_ts": None,
        "todos": [],
        "busy": False,            # True if the latest assistant turn used a tool (i.e. work in progress)
    }
    last_assistant_tool_use = False

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
                state["last_turn_ts"] = d.get("timestamp")

                # Detect tool use → "busy" hint.
                content = m.get("content") or []
                last_assistant_tool_use = any(
                    isinstance(b, dict) and b.get("type") == "tool_use" for b in content
                )

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


def get_state() -> dict:
    fn = find_latest_session()
    if not fn:
        return {"error": "no session log found"}
    return parse_session(fn)


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
