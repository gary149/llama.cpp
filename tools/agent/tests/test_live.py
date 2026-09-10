"""Opt-in PTY smoke test against a running OpenAI-compatible model server."""

import argparse
import json
import re
from pathlib import Path
import tempfile

from test_terminal import Terminal


parser = argparse.ArgumentParser()
parser.add_argument("--executable", required=True)
parser.add_argument("--server-url", required=True)
parser.add_argument("--artifacts", default="terminal-live-artifacts")
args = parser.parse_args()
artifacts = Path(args.artifacts)
artifacts.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="agent-live-") as workspace:
    session = Path(workspace, "session.jsonl")
    session.write_text("\n".join(json.dumps(entry) for entry in [
        {"type": "session", "version": 1, "cwd": workspace},
        {"type": "message", "message": {"role": "user", "content": "Remember this session marker."}},
        {"type": "message", "message": {"role": "assistant", "content": "RESUMED ANSWER SENTINEL"}},
    ]) + "\n")
    terminal = Terminal(str(Path(args.executable).resolve()), arguments=[
        "--backend", "http", "--server-url", args.server_url,
        "--session", str(session), "--no-mcp", "--no-skills", "--no-agents-md",
        "-n", "1024", "--color", "off",
    ], cwd=workspace)
    try:
        terminal.wait(lambda: "tok/s" in terminal.text(), "agent ready", timeout=30)
        terminal.set_size(100, 32)
        terminal.wait(lambda: "RESUMED ANSWER SENTINEL" in terminal.text(), "resumed conversation after resize")
        terminal.send("What is seventeen plus twenty-five? Answer with digits only.\r")
        terminal.set_size(50, 16)
        terminal.wait(lambda: "42" in terminal.text(), "live model answer", timeout=120)
        terminal.set_size(100, 32)
        terminal.wait(lambda: "tok/s" in terminal.text(), "live footer after resize")
        terminal.send("Use the write tool to create smoke.txt containing exactly LIVE_TOOL_OK. Then answer DONE.\r")
        terminal.wait(lambda: "Permission:" in terminal.text(), "live write permission", timeout=120)
        if "Permission: write" not in terminal.text():
            lines = [line.rstrip() for line in terminal.screen.display]
            if "Permission: bash" not in lines:
                raise AssertionError("model requested an unexpected tool")
            command = lines[lines.index("Permission: bash") + 1]
            allowed = r"(?:printf|echo -n) ['\"]LIVE_TOOL_OK['\"] > (?:\./)?smoke\.txt(?: && cat (?:\./)?smoke\.txt)?"
            if not re.fullmatch(allowed, command):
                raise AssertionError("model requested an unexpected shell command: " + command)
        terminal.set_size(60, 20)
        terminal.send("y")
        terminal.wait(lambda: Path(workspace, "smoke.txt").exists(), "live tool execution", timeout=120)
        assert Path(workspace, "smoke.txt").read_text().strip() == "LIVE_TOOL_OK"
        terminal.wait(lambda: "DONE" in terminal.text().split("[Permission granted")[-1] or "Permission:" in terminal.text(), "live completion or verification request", timeout=120)
        if "Permission:" in terminal.text():
            terminal.send("\x03")
            terminal.wait(lambda: "[Cancelled by user]" in terminal.text(), "cancel extra verification request", timeout=30)
        else:
            terminal.wait(lambda: terminal.screen.display[-1].startswith("  "), "tool turn completed", timeout=30)
        terminal.send("Count from one to five hundred, one number per line. Do not use tools.\r")
        terminal.wait(lambda: re.match(r"^[|/\\-] ", terminal.screen.display[-1]), "live generation to interrupt", timeout=30)
        start = len(terminal.raw)
        terminal.send("\x03")
        terminal.wait(lambda: b"[Cancelled by user]" in terminal.raw[start:], "live cancellation", timeout=30)
        print("PASS: session resume, model answer, resize, permission, write tool, and cancellation")
    finally:
        try:
            terminal.close()
        finally:
            (artifacts / "live.ansi").write_bytes(terminal.raw)
            (artifacts / "live.json").write_text(json.dumps(terminal.snapshots, indent=2))
            (artifacts / "live.txt").write_text(terminal.text())
