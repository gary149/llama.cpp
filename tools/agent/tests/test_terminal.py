"""Run the real renderer in a PTY and check the interpreted terminal screen."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import tempfile
import time
import unittest

import pyte


class Terminal:
    def __init__(self, executable, columns=80, rows=24, arguments=None, cwd=None, color=False):
        self.master, self.slave = pty.openpty()
        self.original_mode = termios.tcgetattr(self.slave)
        self.screen = pyte.HistoryScreen(columns, rows, history=10000)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        self.snapshots = []
        self.set_size(columns, rows)
        read_fd, self.control = os.pipe()
        self.fixture = arguments is None
        self.process = subprocess.Popen(
            [executable, *(arguments if arguments is not None else ["--terminal-fixture", f"/dev/fd/{read_fd}"])],
            stdin=self.slave, stdout=self.slave, stderr=subprocess.PIPE,
            pass_fds=(read_fd,), start_new_session=True,
            cwd=cwd,
            env={**os.environ, **({"LLAMA_TEST_COLOR": "1"} if color else {}), **({"LLAMA_TEST_CWD": cwd} if cwd else {}), "TERM": "xterm-256color", "LC_ALL": "en_US.UTF-8" if sys.platform == "darwin" else "C.UTF-8"},
        )
        os.close(read_fd)

    def set_size(self, columns, rows):
        self.snapshots.append({"type": "resize", "offset": len(self.raw), "columns": columns, "rows": rows})
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.screen.resize(lines=rows, columns=columns)
        if hasattr(self, "process"):
            previous = len(self.raw)
            os.kill(self.process.pid, signal.SIGWINCH)
            self.wait(lambda: b"\x1b[H\x1b[2J" in self.raw[previous:], "resize output")

    def send(self, text):
        os.write(self.master, text.encode())

    def event(self, op, **fields):
        os.write(self.control, (json.dumps({"op": op, **fields}) + "\n").encode())

    def read(self, timeout=0.05):
        if select.select([self.master], [], [], timeout)[0]:
            data = os.read(self.master, 65536)
            self.raw.extend(data)
            self.stream.feed(data)

    def wait(self, predicate, description, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read()
            if predicate() and self.raw.endswith(b"\x1b[?2026l"):
                self.snapshots.append({"type": "checkpoint", "offset": len(self.raw), "checkpoint": description, "columns": self.screen.columns,
                                       "rows": self.screen.lines, "screen": self.screen.display,
                                       "cursor": [self.screen.cursor.x, self.screen.cursor.y]})
                return
            if self.process.poll() is not None:
                raise AssertionError(f"renderer exited: {self.process.returncode}")
        raise AssertionError(f"Timed out: {description}\n{self.text()}")

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def command(self):
        response = self.reply("command")
        if not response["received"]:
            raise AssertionError("input did not produce a command")
        return response

    def reply(self, op, **fields):
        self.event(op, **fields)
        if not select.select([self.process.stderr], [], [], 3)[0]:
            raise AssertionError("command response timed out")
        return json.loads(self.process.stderr.readline())

    def close(self):
        try:
            if self.process.poll() is None:
                if self.fixture:
                    self.event("quit")
                else:
                    self.send("\x03/exit\r")
                deadline = time.monotonic() + (3 if self.fixture else 10)
                while self.process.poll() is None and time.monotonic() < deadline:
                    self.read()
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait()
                    raise AssertionError("renderer did not shut down")
            self.read(0)
            if self.process.returncode != 0:
                raise AssertionError(self.process.stderr.read().decode())
            if termios.tcgetattr(self.slave) != self.original_mode:
                raise AssertionError("terminal mode was not restored")
        finally:
            os.close(self.control)
            os.close(self.master)
            os.close(self.slave)
            self.process.stderr.close()


class RendererTests(unittest.TestCase):
    def setUp(self):
        self.terminal = Terminal(ARGS.executable, color=ARGS.color)
        self.addCleanup(self.cleanup_terminal)
        self.terminal.wait(lambda: "fixture-model" in self.terminal.text(), "initial footer")

    def cleanup_terminal(self):
        try:
            self.terminal.close()
        finally:
            directory = Path(ARGS.artifacts)
            directory.mkdir(parents=True, exist_ok=True)
            stem = directory / self.id().split(".")[-1]
            stem.with_suffix(".ansi").write_bytes(self.terminal.raw)
            stem.with_suffix(".txt").write_text(self.terminal.text())
            stem.with_suffix(".json").write_text(json.dumps(self.terminal.snapshots, indent=2))

    def test_clipboard_image_transport(self):
        t = self.terminal
        t.send("\x16\x16")
        t.wait(lambda: "[image][image 2]" in t.text(), "two pasted image markers")
        t.set_size(40, 10)
        t.send("\r")
        command = t.command()
        self.assertEqual(command["text"], "[image][image 2]")
        image = [[1, 2, 3], "image/png"]
        self.assertEqual(command["images"], [image, image])
        self.assertEqual(t.reply("images"), [])

    def test_queued_commands_own_images(self):
        t = self.terminal
        t.send("\x16\r\x16\r")
        for _ in range(2):
            command = t.command()
            self.assertEqual(command["text"], "[image]")
            self.assertEqual(command["images"], [[[1, 2, 3], "image/png"]])
        self.assertEqual(t.reply("images"), [])

    def test_cancel_next_draft_preserves_submitted_image(self):
        t = self.terminal
        t.send("\x16\r\x16\x03")
        command = t.command()
        self.assertEqual(command["images"], [[[1, 2, 3], "image/png"]])
        t.wait(lambda: "[image]" not in t.text(), "next image draft cancelled")
        self.assertEqual(t.reply("images"), [])

    def test_cancel_draft_discards_images(self):
        t = self.terminal
        t.send("\x16")
        t.wait(lambda: "[image]" in t.text(), "image draft before cancellation")
        t.send("\x03")
        t.wait(lambda: "[image]" not in t.text(), "image draft cancelled")
        self.assertEqual(t.reply("images"), [])

    def test_deleted_image_marker_does_not_attach(self):
        t = self.terminal
        t.send("\x16" + "\x7f" * 7 + "text only\r")
        command = t.command()
        self.assertEqual(command["text"], "text only")
        self.assertEqual(command["images"], [])

    def test_unavailable_clipboard_does_not_attach(self):
        t = self.terminal
        t.reply("clipboard", available=False)
        t.send("\x16unchanged\r")
        self.assertEqual(t.command()["text"], "unchanged")
        self.assertEqual(t.reply("images"), [])

    def test_remaining_image_marker_is_renumbered(self):
        t = self.terminal
        t.send("\x16" + "\x7f" * 7 + "\x16\r")
        command = t.command()
        self.assertEqual(command["text"], "[image]")
        self.assertEqual(command["images"], [[[1, 2, 3], "image/png"]])

    def test_editor_cursor_and_submit(self):
        t = self.terminal
        t.send("hello\x1b[D\x7f!")
        t.wait(lambda: "hel!o" in t.text(), "edited input")
        self.assertEqual((t.screen.cursor.x, t.screen.cursor.y), (6, 21))
        t.send("\r")
        self.assertEqual(t.command()["text"], "hel!o")

    def test_large_paste_preserves_submission(self):
        t = self.terminal
        content = "".join(f"line {i}\n" for i in range(15))
        t.send("\x1b[200~" + content + "\x1b[201~\r")
        self.assertEqual(t.command()["text"], content)

    def test_paste_preserves_tabs(self):
        t = self.terminal
        t.send("\x1b[200~first\tsecond\nthird\x1b[201~\r")
        self.assertEqual(t.command()["text"], "first\tsecond\nthird")

    def test_paste_normalizes_crlf(self):
        t = self.terminal
        t.send("\x1b[200~first\r\nsecond\x1b[201~\r")
        self.assertEqual(t.command()["text"], "first\nsecond")

    def test_ctrl_c_clears_then_exits(self):
        t = self.terminal
        t.send("draft")
        t.wait(lambda: "draft" in t.text(), "draft before Ctrl+C")
        t.send("\x03")
        t.wait(lambda: "draft" not in t.text(), "Ctrl+C clears input")
        t.send("\x03")
        self.assertTrue(t.command()["eof"])

    def test_interrupt_generation(self):
        t = self.terminal
        t.event("iteration")
        t.event("text", text="GENERATION ACTIVE")
        t.wait(lambda: "GENERATION ACTIVE" in t.text(), "generation started")
        t.send("\x03")
        t.wait(lambda: "aborting" in t.text(), "generation interrupted")
        self.assertEqual(t.reply("interrupts"), {"count": 1})

    def test_tab_paste_cursor(self):
        t = self.terminal
        t.send("\x1b[200~a\tb\x1b[201~")
        t.wait(lambda: "b" in t.screen.display[21], "tabbed paste display")
        self.assertEqual(t.screen.cursor.x, 9)
        self.assertEqual(t.screen.display[21].rstrip(), "\u203a a     b")

    def test_transcript_cannot_move_cursor(self):
        t = self.terminal
        start = len(t.raw)
        t.event("transcript", text="SAFE\x1b[2JEND\n")
        t.wait(lambda: "END" in t.text(), "terminal controls in tool output")
        self.assertNotIn(b"\x1b[2J", t.raw[start:])
        self.assertIn("SAFE", t.text())

    def test_paste_bypasses_autocomplete(self):
        t = self.terminal
        t.send("/")
        t.wait(lambda: "/compact" in t.text(), "autocomplete before paste")
        t.send("\x1b[200~literal\ntext\x1b[201~\r")
        self.assertEqual(t.command()["text"], "/literal\ntext")

    def test_startup_input_not_consumed(self):
        terminal = Terminal(ARGS.executable)
        try:
            terminal.send("early input\r")
            self.assertEqual(terminal.command()["text"], "early input")
        finally:
            terminal.close()

    def test_shutdown_drains_pending_output(self):
        t = self.terminal
        t.event("text", text="FINAL BUFFERED TEXT")
        t.event("quit")
        t.process.wait(timeout=3)
        t.read()
        self.assertIn("FINAL BUFFERED TEXT", t.text())

    def test_stream_visible_before_completion(self):
        t = self.terminal
        t.event("text", text="streaming before completion")
        t.wait(lambda: "streaming before completion" in t.text(), "live text delta")
        t.event("text", text=" continues")
        t.wait(lambda: "completion continues" in t.text(), "continued text delta")
        t.event("completed")

    def test_reasoning_and_error_styles_do_not_leak(self):
        t = self.terminal
        for op, marker, foreground in [("reasoning", "THINKING MARKER", "brightblack"), ("error", "ERROR MARKER", "red")]:
            t.event(op, text=marker)
            t.wait(lambda: marker in t.text(), op + " style")
            row = next(i for i, line in enumerate(t.screen.display) if marker in line)
            self.assertEqual(t.screen.buffer[row][0].fg, foreground if ARGS.color else "default")
            self.assertEqual(t.screen.buffer[t.screen.cursor.y][0].fg, "default")

    def test_tool_stream_header_and_iterations(self):
        t = self.terminal
        for filename in ("first.txt", "second.txt"):
            t.event("iteration")
            arguments = json.dumps({"file_path": filename, "content": "TOOL CONTENT"})
            t.event("tool_delta", name="write", args=arguments[:25])
            t.event("tool_delta", name="write", args=arguments[25:])
            t.event("tool_start", name="write", args=arguments)
            t.wait(lambda: f"> write {filename}" in t.text(), "tool header " + filename)
            self.assertEqual(t.text().count(f"> write {filename}"), 1)
        self.assertEqual(t.text().count("> write"), 2)
        self.assertEqual(t.text().count("TOOL CONTENT"), 2)

    def test_resize_preserves_input_and_transcript(self):
        t = self.terminal
        t.event("transcript", text="TRANSCRIPT sentinel\n")
        t.send("draft")
        t.wait(lambda: "TRANSCRIPT sentinel" in t.text() and "draft" in t.text(), "initial transcript and draft")
        for columns, rows in [(40, 10), (120, 40), (20, 6), (80, 24)]:
            t.set_size(columns, rows)
            t.wait(lambda: "draft" in t.text(), f"resize {columns}x{rows}")
            self.assertEqual(t.text().count("draft"), 1)
            if columns >= 40:
                self.assertEqual(t.text().count("fixture-model"), 1)
        self.assertIn("TRANSCRIPT sentinel", t.text())
        t.send("\r")
        self.assertEqual(t.command()["text"], "draft")

    def test_tiny_terminal_cursor(self):
        t = self.terminal
        t.set_size(8, 3)
        t.send("abcdef")
        t.wait(lambda: "abcdef" in t.text(), "input in tiny terminal")
        t.send("\r")
        self.assertEqual(t.command()["text"], "abcdef")

    def test_overlay_does_not_erase_transcript(self):
        t = self.terminal
        t.event("transcript", text="LAST TRANSCRIPT LINE\n")
        t.wait(lambda: "LAST TRANSCRIPT LINE" in t.text(), "transcript")
        t.send("/")
        t.wait(lambda: "/compact" in t.text(), "slash overlay")
        self.assertIn("LAST TRANSCRIPT LINE", t.text())
        t.send("\x1b")
        t.wait(lambda: "/compact" not in t.text(), "overlay dismissal")
        self.assertIn("LAST TRANSCRIPT LINE", t.text())

    def test_file_completion(self):
        directory = tempfile.TemporaryDirectory(prefix="agent-completion-")
        self.addCleanup(directory.cleanup)
        Path(directory.name, "alpha.txt").write_text("fixture")
        self.terminal.close()
        self.terminal = t = Terminal(ARGS.executable, cwd=directory.name, color=ARGS.color)
        t.wait(lambda: "fixture-model" in t.text(), "file completion ready")
        t.send("@alp")
        t.wait(lambda: "alpha.txt" in t.text(), "file completion result")
        t.send("\t\r")
        self.assertEqual(t.command()["text"], "@alpha.txt ")

    def test_permission_resize_and_dismiss(self):
        t = self.terminal
        t.event("permission")
        t.wait(lambda: "Permission: write" in t.text(), "permission overlay")
        t.set_size(40, 12)
        t.wait(lambda: "[y]es" in t.text(), "resized permission choices")
        t.send("n")
        t.wait(lambda: "Permission: write" not in t.text(), "permission dismissed")

    def test_eof(self):
        self.terminal.send("\x04")
        self.assertTrue(self.terminal.command()["eof"])

    def test_unicode_cursor(self):
        t = self.terminal
        t.send("a\u754c\u03bb")
        t.wait(lambda: "a\u754c\u03bb" in t.text(), "wide input")
        self.assertEqual(t.screen.cursor.x, 6)
        t.send("\x1b[D\x7f\r")
        self.assertEqual(t.command()["text"], "a\u03bb")

    def test_combining_character_cursor(self):
        t = self.terminal
        t.send("e\u0301")
        t.wait(lambda: "\u00e9" in t.text() or "e\u0301" in t.text(), "combining input")
        self.assertEqual(t.screen.cursor.x, 3)
        t.send("\r")
        self.assertEqual(t.command()["text"], "e\u0301")

    def test_emoji_cursor_and_resize(self):
        t = self.terminal
        t.send("a\U0001f600b")
        t.wait(lambda: "a\U0001f600b" in t.text(), "emoji input")
        self.assertEqual(t.screen.cursor.x, 6)
        t.set_size(5, 8)
        t.set_size(80, 24)
        self.assertEqual(t.screen.cursor.x, 6)
        t.send("\x1b[D\x7f\r")
        self.assertEqual(t.command()["text"], "ab")

    def test_resize_stress_preserves_input(self):
        t = self.terminal
        t.send("persistent draft")
        t.wait(lambda: "persistent draft" in t.text(), "stress input")
        for columns, rows in [(1, 1), (2, 2), (4, 2), (40, 8), (200, 50), (9, 4), (80, 24)]:
            t.set_size(columns, rows)
            self.assertLess(t.screen.cursor.x, columns)
            self.assertLess(t.screen.cursor.y, rows)
        t.send("\r")
        self.assertEqual(t.command()["text"], "persistent draft")

    def test_permission_ignores_pasted_answer(self):
        t = self.terminal
        t.event("permission")
        t.wait(lambda: "Permission: write" in t.text(), "permission before paste")
        t.send("\x1b[200~yes\x1b[201~")
        self.assertFalse(t.reply("permission_result")["received"])
        self.assertIn("Permission: write", t.text())
        t.send("n")
        self.assertEqual(t.reply("permission_result"), {"received": True, "allowed": False})

    def test_exact_width_cursor(self):
        t = self.terminal
        t.set_size(20, 8)
        t.send("x" * 18)
        t.wait(lambda: "x" * 18 in t.text(), "input at right margin")
        self.assertEqual(t.screen.cursor.x, 2)
        self.assertEqual(t.screen.cursor.y, 5)

    def test_permission_in_tiny_window(self):
        t = self.terminal
        t.set_size(18, 4)
        t.event("permission")
        t.wait(lambda: "[y]" in t.text() and "[n]" in t.text(), "small permission choices")
        t.send("n")
        t.wait(lambda: "[y]" not in t.text(), "small permission dismissed")

    def test_resize_does_not_erase_scrollback(self):
        t = self.terminal
        t.event("transcript", text="".join(f"history-{i:04d}\n" for i in range(100)))
        t.wait(lambda: "history-0099" in t.text(), "long transcript")
        start = len(t.raw)
        t.set_size(60, 20)
        t.wait(lambda: "fixture-model" in t.text(), "footer after history resize")
        self.assertNotIn(b"\x1b[3J", t.raw[start:])

    def test_pending_transcript_survives_resize(self):
        t = self.terminal
        t.event("transcript_resize", text="QUEUED BEFORE RESIZE\n")
        t.wait(lambda: "QUEUED BEFORE RESIZE" in t.text(), "pending text after resize")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--artifacts", default="terminal-artifacts")
    parser.add_argument("--color", action="store_true")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
