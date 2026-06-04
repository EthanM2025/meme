"""
Paste text into the currently focused window on macOS.
Uses: pbcopy to set clipboard, then osascript to send Cmd+V keystroke.

First run will prompt for Accessibility permission:
  System Settings → Privacy & Security → Accessibility → enable Terminal/iTerm/Python.
"""
import subprocess


def paste_to_active_window(text: str) -> None:
    if not text:
        return

    # 1) Set clipboard
    p = subprocess.Popen(["pbcopy"], stdin=subprocess.PIPE)
    p.communicate(text.encode("utf-8"))

    # 2) Send Cmd+V via System Events. -e blocks until the keystroke is delivered.
    subprocess.run(
        [
            "osascript",
            "-e",
            'tell application "System Events" to keystroke "v" using command down',
        ],
        check=False,
    )


def press_enter() -> None:
    """Simulate a single Return keypress on the focused window."""
    subprocess.run(
        ["osascript", "-e",
         'tell application "System Events" to keystroke return'],
        check=False,
    )


def clear_input() -> None:
    """Wipe the focused input field: Cmd+A then Delete.
    Triggered by a double-press on the device's right button."""
    subprocess.run(
        ["osascript", "-e",
         'tell application "System Events"\n'
         '  keystroke "a" using command down\n'
         '  key code 51\n'   # 51 = delete/backspace
         'end tell'],
        check=False,
    )


def get_active_app() -> str:
    """Returns the name of the frontmost app, for logging."""
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
        return "?"


if __name__ == "__main__":
    import sys
    # Smoke test: paste a known string into wherever the cursor is.
    msg = " ".join(sys.argv[1:]) or "测试粘贴 — 你看到这行说明 Accessibility 权限 OK ✓"
    print(f"前台 App: {get_active_app()}")
    print(f"即将粘贴: {msg!r}")
    print("3 秒后开始，请把光标放到要粘贴的输入框里...")
    import time
    for i in range(3, 0, -1):
        print(f"  {i}…")
        time.sleep(1)
    paste_to_active_window(msg)
    print("已粘贴")
