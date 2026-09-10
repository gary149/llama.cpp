# Terminal rendering tests

[Watch the Linux terminal test recording](media/README.md), including all scenarios in both color modes, the live Qwen smoke test, and test-suite results.

The C++ fixture runs the production renderer and input loop in a pseudoterminal. A separate control pipe injects agent events and reads submitted commands. Python interprets the emitted ANSI bytes with pyte and checks screen contents, cursor positions, input preservation, and terminal mode restoration. No model is needed for these regression tests.

```sh
python3 -m venv .venv
.venv/bin/pip install -r tools/agent/tests/requirements.txt
cmake -S . -B build -DLLAMA_BUILD_TESTS=ON -DLLAMA_HTTPLIB=ON \
  -DLLAMA_AGENT_TERMINAL_TESTS=ON \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build --target llama-agent test-agent-tui -j 4
ctest --test-dir build -R '^agent-(tui|terminal)' --output-on-failure
```

Tests run with color enabled and disabled. Each terminal test saves an `.ansi` byte capture, `.json` checkpoint screens and cursor positions, and a `.txt` final screen under `build/tools/agent/terminal-artifacts`. Failures include the observed screen. Tests wait for observable output or a bounded command response.

Menu regressions compare transcript rows before opening and after dismissal, and reject scrollback growth during repeated cycles. They cover Escape, Backspace, no matching commands, submission, resize while open, and a permission response that commits output while closing. The live model smoke test also checks menu spacing after a response.

Covered regressions include editing and submission, EOF, wide and combining Unicode, right-margin cursor placement, bracketed paste contents and tabs, CRLF normalization, live text before completion, slash and file completion, autocomplete preserving transcript, resize sweeps down to 1x1, permission overlays, pasted permission answers, Ctrl+C, pending output during resize, tool streams across iterations, terminal control bytes in tool output, color leakage, and scrollback clearing. C++ tests also cover multiline mode and JSON escapes split at every byte boundary. The fixture also checks normal shutdown and restoration of the original terminal attributes.

Clipboard tests use an injected image provider to check attachment transport, unavailable images, deleted markers, cancelled drafts, and ownership across queued commands. They do not read the operating system clipboard or decode images. On Windows, the C++ suite allocates an isolated console and checks native key input, Unicode cursor placement, resize handling, submission, EOF, and mode restoration.

The emoji regression covers a supplementary-plane character through cursor placement, narrow-window resize, and deletion. It does not cover joined emoji sequences or terminal-specific grapheme shaping.

## Live model smoke test

This opt-in test runs the actual agent against an existing server. It resumes a temporary session, resizes the terminal, asks an arithmetic question, approves a write in the temporary directory, verifies the resulting file, tests cancellation, and exits. It disables MCP, skills, and AGENTS.md discovery for that temporary workspace. Extra model verification requests are cancelled.

```sh
.venv/bin/python tools/agent/tests/test_live.py \
  --executable build/bin/llama-agent \
  --server-url http://127.0.0.1:8097 \
  --artifacts build/tools/agent/live-artifacts
```

## Browser replay and screenshots

Replay the captures in xterm.js, compare screen and cursor checkpoints against pyte, and save screenshots. The replay also rejects footer text in scrollback. Unicode strings are normalized for comparison because the interpreters represent combining characters differently.

```sh
npm install --prefix /tmp/tui-browser @xterm/xterm@5.5.0 playwright@1.55.0
/tmp/tui-browser/node_modules/.bin/playwright install chromium
node tools/agent/tests/replay_terminal.mjs build/tools/agent/terminal-artifacts /tmp/tui-browser
```

Pass a Chromium executable path as the last argument to use an installed browser. Open the generated `index.html` for a screenshot gallery. Checkpoint differences produce `.difference.json` files. The agent-terminal workflow runs C++ tests on Linux, macOS, and Windows, PTY tests on Linux and macOS, and browser replay on Linux. It uploads captures and screenshots even when tests fail.

For memory and undefined-behavior checks, configure a separate build with `-DLLAMA_SANITIZE_ADDRESS=ON -DLLAMA_SANITIZE_UNDEFINED=ON`, build `test-agent-tui`, then run `ctest --test-dir build -R 'agent-(tui|terminal)' --output-on-failure`.

## Input modes

Enter submits and Alt+Enter inserts a newline. With `--multiline-input`, Enter inserts a newline and Alt+Enter submits. Ctrl+C clears a draft, exits an empty idle editor, or interrupts generation. Ctrl+D exits an empty editor. Bracketed pastes preserve their full contents and cannot approve a permission prompt.

## Verification limits

The PTY harness currently requires a POSIX host. Linux PTY tests, xterm.js replay, screenshot inspection, sanitizer runs, and a real Qwen server smoke test have been exercised locally. Cross-compilation checks are not native runtime tests. Native macOS and Windows runs, terminal-specific clipboard/image protocols, and complex emoji behavior still need verification. Passing this suite is regression evidence, not proof that rendering is correct in every terminal.
