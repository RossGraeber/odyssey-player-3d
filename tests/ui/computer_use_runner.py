"""Computer-use loop driving the running odyssey.exe for milestone UI tests.

Invoked by the ui-tooling-runner subagent (which is in turn delegated by
ui-integration-tester). Reads one task from a scenario Markdown file,
runs the Anthropic computer-use loop against the live desktop, and prints
the canonical two-line verdict the orchestrator expects:

    RESULT: PASS|FAIL <one-line reason>
    SCREENSHOT: <path>

Everything else goes to stderr so the orchestrator can grep stdout.
"""

from __future__ import annotations

import argparse
import base64
import io
import math
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import mss
import pyautogui
from PIL import Image
from anthropic import Anthropic

# Computer-use API constraints. Sonnet 4.6 / Opus 4.6 use the older 1568-px
# scaling rule; Opus 4.7 has 1:1 coords up to 2576 on the long edge but we
# default to Sonnet so we always downscale.
MAX_LONG_EDGE = 1568
MAX_PIXELS = 1_150_000
DEFAULT_MODEL = "claude-sonnet-4-6"
BETA_HEADER = "computer-use-2025-11-24"
TOOL_TYPE = "computer_20251124"

SCREENSHOT_DIR = Path(__file__).parent / "screenshots"
SCREENSHOT_DIR.mkdir(exist_ok=True)


# ---------------------------------------------------------------------------
# Scenario parsing
# ---------------------------------------------------------------------------

@dataclass
class Task:
    task_id: str
    instruction: str
    expected: str


def parse_scenario(path: Path, task_id: str) -> Task:
    text = path.read_text(encoding="utf-8")
    # Find the H3 block matching the task id, then read until the next H3 or H2.
    pattern = re.compile(
        rf"^###\s+{re.escape(task_id)}\s*$(.*?)(?=^##\s|^###\s|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        raise SystemExit(f"task '{task_id}' not found in {path}")

    body = m.group(1).strip()
    expected_match = re.search(r"\*\*Expected:\*\*\s*(.+)", body)
    if not expected_match:
        raise SystemExit(f"task '{task_id}' missing **Expected:** line")

    instruction = body[: expected_match.start()].strip()
    expected = expected_match.group(1).strip()
    return Task(task_id=task_id, instruction=instruction, expected=expected)


# ---------------------------------------------------------------------------
# Screen capture + scaling
# ---------------------------------------------------------------------------

def primary_monitor_size() -> tuple[int, int]:
    with mss.mss() as sct:
        m = sct.monitors[1]
        return m["width"], m["height"]


def scale_factor(width: int, height: int) -> float:
    long_edge = max(width, height)
    total = width * height
    return min(1.0, MAX_LONG_EDGE / long_edge, math.sqrt(MAX_PIXELS / total))


def capture_scaled(scale: float) -> tuple[bytes, Path]:
    with mss.mss() as sct:
        raw = sct.grab(sct.monitors[1])
    img = Image.frombytes("RGB", raw.size, raw.rgb)
    if scale < 1.0:
        new = (int(img.width * scale), int(img.height * scale))
        img = img.resize(new, Image.LANCZOS)

    buf = io.BytesIO()
    img.save(buf, format="PNG")
    png = buf.getvalue()

    out = SCREENSHOT_DIR / f"shot_{int(time.time() * 1000)}.png"
    out.write_bytes(png)
    return png, out


# ---------------------------------------------------------------------------
# Action execution
# ---------------------------------------------------------------------------

# pyautogui safety: failsafe corner is annoying for full-screen apps.
pyautogui.FAILSAFE = False
pyautogui.PAUSE = 0.05


def execute_action(action: dict, scale: float) -> tuple[str | None, Path | None]:
    """Returns (text_summary, screenshot_path). Either may be None."""
    name = action.get("action")

    def coord(key: str = "coordinate") -> tuple[int, int]:
        x, y = action[key]
        return int(x / scale), int(y / scale)

    if name == "screenshot":
        png, path = capture_scaled(scale)
        return None, path

    if name in {"left_click", "right_click", "middle_click",
                "double_click", "triple_click"}:
        x, y = coord()
        button = {"left_click": "left", "right_click": "right",
                  "middle_click": "middle",
                  "double_click": "left", "triple_click": "left"}[name]
        clicks = {"double_click": 2, "triple_click": 3}.get(name, 1)
        modifier = action.get("text")
        if modifier:
            with pyautogui.hold(modifier):
                pyautogui.click(x, y, clicks=clicks, button=button)
        else:
            pyautogui.click(x, y, clicks=clicks, button=button)
        return f"clicked {name} at ({x},{y})", None

    if name == "mouse_move":
        x, y = coord()
        pyautogui.moveTo(x, y)
        return f"moved to ({x},{y})", None

    if name in {"left_mouse_down", "left_mouse_up"}:
        x, y = coord()
        if name == "left_mouse_down":
            pyautogui.mouseDown(x, y, button="left")
            return f"mouseDown at ({x},{y})", None
        pyautogui.mouseUp(x, y, button="left")
        return f"mouseUp at ({x},{y})", None

    if name == "left_click_drag":
        x1, y1 = coord("start_coordinate")
        x2, y2 = coord("coordinate")
        pyautogui.moveTo(x1, y1)
        pyautogui.dragTo(x2, y2, button="left")
        return f"dragged ({x1},{y1})->({x2},{y2})", None

    if name == "type":
        pyautogui.typewrite(action["text"], interval=0.01)
        return f"typed {len(action['text'])} chars", None

    if name == "key":
        # Claude sends X11-style key combos like "ctrl+s" or "Return".
        keys = [k.strip().lower() for k in action["text"].split("+")]
        # Map a few common X11 names to pyautogui names.
        x11_to_py = {"return": "enter", "escape": "esc",
                     "backspace": "backspace", "page_up": "pageup",
                     "page_down": "pagedown", "super": "win"}
        keys = [x11_to_py.get(k, k) for k in keys]
        if len(keys) == 1:
            pyautogui.press(keys[0])
        else:
            pyautogui.hotkey(*keys)
        return f"pressed {action['text']}", None

    if name == "hold_key":
        duration = float(action.get("duration", 1))
        keys = [k.strip().lower() for k in action["text"].split("+")]
        pyautogui.keyDown(keys[-1])
        time.sleep(duration)
        pyautogui.keyUp(keys[-1])
        return f"held {action['text']} for {duration}s", None

    if name == "scroll":
        x, y = coord()
        amount = int(action.get("scroll_amount", 3))
        direction = action.get("scroll_direction", "down")
        sign = -1 if direction == "down" else 1 if direction == "up" else 0
        if sign:
            pyautogui.moveTo(x, y)
            pyautogui.scroll(sign * amount * 100)
        return f"scrolled {direction} {amount}", None

    if name == "wait":
        duration = float(action.get("duration", 1))
        time.sleep(duration)
        return f"waited {duration}s", None

    return f"unsupported action: {name}", None


def tool_result_block(tool_use_id: str, summary: str | None,
                      screenshot_png: bytes | None,
                      is_error: bool = False) -> dict:
    content: list[dict] = []
    if summary:
        content.append({"type": "text", "text": summary})
    if screenshot_png is not None:
        content.append({
            "type": "image",
            "source": {
                "type": "base64",
                "media_type": "image/png",
                "data": base64.standard_b64encode(screenshot_png).decode(),
            },
        })
    if not content:
        content = [{"type": "text", "text": "ok"}]
    return {
        "type": "tool_result",
        "tool_use_id": tool_use_id,
        "content": content,
        "is_error": is_error,
    }


# ---------------------------------------------------------------------------
# Agent loop
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are validating one specific UI behaviour of a Windows desktop video
player called Odyssey Player 3D. The application is already running on
the visible desktop. You will:

1. Take screenshots, observe the application's state, and perform mouse /
   keyboard actions as needed to complete the test instruction.
2. After EVERY action, take a screenshot and explicitly evaluate whether
   the expected outcome occurred. Show your reasoning in plain prose
   ("I have evaluated step X..."). If wrong, retry; only proceed when
   correct.
3. When you have a definitive verdict, end your reply with exactly one
   line in this format and nothing after it:

       VERDICT: PASS - <one short reason>
   or
       VERDICT: FAIL - <one short reason>

Be conservative — when in doubt, FAIL with the reason "could not verify".
Never close, kill, or reboot the application unless the test instruction
explicitly asks for it. Never type passwords or interact with anything
outside the player window.
"""


def run_agent_loop(client: Anthropic, model: str, task: Task,
                   scenario_path: Path, max_iter: int = 20) -> tuple[bool, str, Path | None]:
    sw, sh = primary_monitor_size()
    scale = scale_factor(sw, sh)
    scaled_w, scaled_h = int(sw * scale), int(sh * scale)

    print(f"[runner] display {sw}x{sh}, scaled {scaled_w}x{scaled_h}, scale={scale:.3f}",
          file=sys.stderr)

    tools = [{
        "type": TOOL_TYPE,
        "name": "computer",
        "display_width_px": scaled_w,
        "display_height_px": scaled_h,
    }]

    user_prompt = (
        f"# Task: {task.task_id}\n\n"
        f"## Instruction\n{task.instruction}\n\n"
        f"## Expected outcome\n{task.expected}\n\n"
        f"Begin by taking a screenshot to observe the current state."
    )
    messages: list[dict] = [{"role": "user", "content": user_prompt}]

    last_screenshot: Path | None = None

    for i in range(max_iter):
        resp = client.beta.messages.create(
            model=model,
            max_tokens=2048,
            system=SYSTEM_PROMPT,
            tools=tools,
            messages=messages,
            betas=[BETA_HEADER],
        )
        messages.append({"role": "assistant", "content": resp.content})

        tool_results: list[dict] = []
        final_text = ""
        for block in resp.content:
            if block.type == "tool_use":
                summary, shot_path = execute_action(block.input, scale)
                if shot_path:
                    last_screenshot = shot_path
                # Always send a fresh screenshot back, even for non-screenshot
                # actions, so the model can verify per the system prompt.
                if shot_path is None:
                    png, shot_path = capture_scaled(scale)
                    last_screenshot = shot_path
                else:
                    png = shot_path.read_bytes()
                tool_results.append(tool_result_block(block.id, summary, png))
            elif block.type == "text":
                final_text += block.text + "\n"

        if not tool_results:
            # Done — model produced final text. Parse for VERDICT line.
            verdict_match = re.search(
                r"^VERDICT:\s*(PASS|FAIL)\s*-?\s*(.*)$",
                final_text, re.MULTILINE | re.IGNORECASE,
            )
            if not verdict_match:
                return False, "no VERDICT line in final reply", last_screenshot
            ok = verdict_match.group(1).upper() == "PASS"
            reason = verdict_match.group(2).strip() or "(no reason given)"
            return ok, reason, last_screenshot

        messages.append({"role": "user", "content": tool_results})

    return False, f"max iterations ({max_iter}) hit without verdict", last_screenshot


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--scenario", required=True, type=Path,
                   help="path to the scenario .md file")
    p.add_argument("--task", required=True,
                   help="H3 task id within the scenario file")
    p.add_argument("--model", default=os.environ.get("ODYSSEY_UI_MODEL",
                                                     DEFAULT_MODEL))
    p.add_argument("--max-iter", type=int, default=20)
    args = p.parse_args()

    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("RESULT: FAIL ANTHROPIC_API_KEY is not set", file=sys.stdout)
        print("SCREENSHOT: none", file=sys.stdout)
        return 77

    if not args.scenario.exists():
        print(f"RESULT: FAIL scenario file not found: {args.scenario}",
              file=sys.stdout)
        print("SCREENSHOT: none", file=sys.stdout)
        return 2

    task = parse_scenario(args.scenario, args.task)
    client = Anthropic()
    ok, reason, shot = run_agent_loop(client, args.model, task, args.scenario,
                                      max_iter=args.max_iter)

    print(f"RESULT: {'PASS' if ok else 'FAIL'} {reason}")
    print(f"SCREENSHOT: {shot if shot else 'none'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
