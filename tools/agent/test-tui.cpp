#include "tui-editor.h"
#include "tui-events.h"
#include "tui-renderer.h"
#include "tui-select-list.h"
#include "console.h"

#include <cstdlib>
#include <clocale>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef ERROR
#endif

static void require(bool ok, const char * message) {
    if (!ok) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

static tui_input_event ch(char32_t cp) {
    return {tui_input_key::CHARACTER, cp};
}

static void test_editor_submit_and_history() {
    tui_editor editor(false);
    editor.handle_event(ch('h'));
    editor.handle_event(ch('i'));
    auto action = editor.handle_event({tui_input_key::ENTER, 0});
    require(action.submitted, "editor submit did not return submitted action");
    require(action.submission == "hi", "editor submission text mismatch");
    require(editor.empty(), "editor did not clear after submit");

    editor.handle_event({tui_input_key::UP, 0});
    require(editor.buffer() == "hi", "editor history up did not restore last entry");
}

static void test_editor_utf8_and_multiline_render() {
    tui_editor editor(false);
    editor.handle_event(ch('a'));
    editor.handle_event(ch(0x03BB)); // lambda
    editor.handle_event({tui_input_key::ALT_ENTER, 0});
    editor.handle_event(ch('b'));
    require(editor.buffer() == "a\xCE\xBB\nb", "editor UTF-8/multiline buffer mismatch");

    tui_editor_render render = editor.render(20, 10);
    require(render.lines.size() == 2, "editor render line count mismatch");
    require(render.cursor_row == 1, "editor cursor row mismatch");
    require(render.cursor_col == 3, "editor cursor column mismatch");
}

static void test_paste_preserves_text() {
    tui_editor editor(false);
    editor.handle_event({tui_input_key::PASTE_START, 0});
    for (int i = 0; i < 12; ++i) {
        editor.handle_event(ch('x'));
        editor.handle_event({tui_input_key::ENTER, 0});
    }
    editor.handle_event({tui_input_key::PASTE_END, 0});
    require(editor.buffer().size() == 24, "large paste lost content");
}

static void test_multiline_mode() {
    tui_editor editor(true);
    editor.set_buffer("first");
    require(!editor.handle_event({tui_input_key::ENTER, 0}).submitted, "multiline Enter submitted early");
    require(editor.buffer() == "first\n", "multiline Enter did not insert a newline");
    editor.handle_event(ch('x'));
    auto action = editor.handle_event({tui_input_key::ALT_ENTER, 0});
    require(action.submitted && action.submission == "first\nx", "multiline Alt+Enter did not submit");
}

static void test_select_list() {
    tui_select_list list;
    list.set_items({"/clear", "/compact", "/stats"});
    require(list.visible(), "select list not visible after set_items");
    require(*list.selected() == "/clear", "select list initial selection mismatch");
    list.move_down();
    require(*list.selected() == "/compact", "select list move_down mismatch");
    list.move_up();
    require(*list.selected() == "/clear", "select list move_up mismatch");
    auto rows = list.render(20, 2);
    require(rows.size() == 2, "select list render visible count mismatch");
}

static void test_event_queue() {
    tui_event_queue<int> queue;
    std::thread producer([&]() {
        queue.push(42);
    });
    int value = 0;
    require(queue.wait_pop(value, 1000), "event queue did not deliver item");
    require(value == 42, "event queue delivered wrong value");
    producer.join();
}

static void test_split_json_escapes() {
    const std::string input = R"({"content":"A\n\uD83D\uDE00\tZ"})";
    const std::string expected = "A\n\xF0\x9F\x98\x80\tZ";
    for (size_t split = 0; split <= input.size(); ++split) {
        tool_stream_state state;
        state.accumulated_args = input.substr(0, split);
        bool complete = decode_field_incremental(state, "content");
        state.accumulated_args += input.substr(split);
        if (!complete) {
            complete = decode_field_incremental(state, "content");
        }
        require(complete && state.content_buffer == expected, "split JSON escape was corrupted");
    }
    bool complete = false;
    require(extract_partial_json_string(input, "content", complete) == expected && complete, "JSON surrogate pair was corrupted");
}

static void test_footer_and_agent_event_projection() {
    tui_footer_state footer;
    footer.working_dir = "/tmp/project";
    footer.session_path = "/tmp/session.jsonl";
    footer.meta.model_name = "test-model";
    footer.meta.n_ctx = 100;
    footer.stats.total_input = 10;
    footer.stats.total_output = 5;
    footer.stats.total_predicted_ms = 1000.0;
    footer.last_prompt_tokens = 50;
    footer.generating = true;
    auto lines = tui_render_footer(footer, 80, false);
    require(lines.size() == 2, "footer did not render two lines");
    require(lines[1].find("test-model") != std::string::npos, "footer missing model");
    require(lines[1].find("\xE2\x86\x91 10 \xE2\x86\x93 5") != std::string::npos,
            "footer missing token arrows");
    require(lines[1].find("5.0 tok/s") != std::string::npos, "footer missing speed");
    require(lines[1].find("50%/1K") != std::string::npos, "footer missing context fill");

    session_stats stats;
    stats.total_input = 7;
    stats.total_output = 3;
    stats.total_cached = 2;
    stats.total_predicted_ms = 500.0;
    tui_event ev = tui_event_from_agent_event(agent_event::completed(agent_stop_reason::COMPLETED, stats, 77));
    require(ev.type == tui_event_type::COMPLETED, "completed event type mismatch");
    require(ev.stats.total_input == 7, "completed event input stats mismatch");
    require(ev.stats.total_output == 3, "completed event output stats mismatch");
    require(ev.stats.total_cached == 2, "completed event cached stats mismatch");
    require(ev.last_prompt_tokens == 77, "completed event prompt-token stat mismatch");

    tui_event tool = tui_event_from_agent_event(agent_event::tool_call_delta(1, "write", "{\"file_path\""));
    require(tool.type == tui_event_type::TOOL_CALL_DELTA, "tool delta event type mismatch");
    require(tool.tool_call_index == 1, "tool delta index mismatch");
    require(tool.tool_name == "write", "tool delta name mismatch");
}

static int terminal_fixture(const char * control_path) {
    std::setlocale(LC_ALL, "");
    std::ifstream control(control_path);
    require(control.good(), "cannot open fixture control pipe");
    tui_renderer::config cfg;
    cfg.color = std::getenv("LLAMA_TEST_COLOR") != nullptr;
    const char * working_dir = std::getenv("LLAMA_TEST_CWD");
    cfg.working_dir = working_dir ? working_dir : "/fixture/project";
    cfg.meta.model_name = "fixture-model";
    cfg.meta.n_ctx = 4096;
    permission_manager_async permissions;
    std::atomic<int> interrupts{0};
    std::atomic<bool> clipboard_available{true};
    console::set_paste_image_callback([&](std::vector<uint8_t> & bytes, std::string & mime) {
        bytes = {1, 2, 3};
        mime = "image/png";
        return clipboard_available.load();
    });
    cfg.permissions = &permissions;
    cfg.interrupt = [&]() { ++interrupts; };
    tui_renderer renderer(cfg);
    std::string permission_id;
    std::string line;
    while (std::getline(control, line)) {
        auto request = nlohmann::json::parse(line);
        std::string op = request.at("op");
        if (op == "quit") {
            break;
        } else if (op == "command") {
            tui_command command;
            bool received = renderer.wait_for_command(command, 2000);
            std::cerr << nlohmann::json({{"received", received}, {"text", command.text}, {"eof", command.eof}, {"images", command.images}}).dump() << std::endl;
        } else if (op == "permission_result") {
            auto response = permissions.wait_for_response(permission_id, 200);
            std::cerr << nlohmann::json({{"received", response.has_value()}, {"allowed", response && response->allowed}}).dump() << std::endl;
        } else if (op == "interrupts") {
            std::cerr << nlohmann::json({{"count", interrupts.load()}}).dump() << std::endl;
        } else if (op == "clipboard") {
            clipboard_available.store(request.at("available"));
            std::cerr << "{}" << std::endl;
        } else if (op == "images") {
            auto images = console::take_pending_images();
            nlohmann::json result = nlohmann::json::array();
            for (const auto & image : images) {
                result.push_back({{"bytes", image.first}, {"mime", image.second}});
            }
            std::cerr << result.dump() << std::endl;
        } else {
            tui_event event;
            if (op == "text") {
                event.type = tui_event_type::TEXT_DELTA;
            } else if (op == "reasoning") {
                event.type = tui_event_type::REASONING_DELTA;
            } else if (op == "error") {
                event.type = tui_event_type::FAILURE;
            } else if (op == "transcript" || op == "transcript_resize") {
                event.type = tui_event_type::TRANSCRIPT;
            } else if (op == "completed") {
                event.type = tui_event_type::COMPLETED;
            } else if (op == "iteration") {
                event.type = tui_event_type::ITERATION_START;
            } else if (op == "tool_delta" || op == "tool_start") {
                event.type = op == "tool_delta" ? tui_event_type::TOOL_CALL_DELTA : tui_event_type::TOOL_START;
                event.tool_name = request.at("name");
                event.tool_args = request.at("args");
                event.tool_call_index = request.value("index", 0);
            } else if (op == "permission") {
                event.type = tui_event_type::PERMISSION_REQUIRED;
                event.perm.tool_name = "write";
                event.perm.description = "Write fixture.txt";
                permission_id = permissions.request_permission(event.perm);
                event.perm_id = permission_id;
            } else {
                require(false, "unknown fixture operation");
            }
            event.text = request.value("text", "");
            renderer.post_event(std::move(event));
            if (op == "transcript_resize") {
                tui_event resize;
                resize.type = tui_event_type::RESIZE;
                renderer.post_event(std::move(resize));
            }
        }
    }
    renderer.shutdown();
    return 0;
}

#if defined(_WIN32)
static void test_native_console() {
    FreeConsole(); // do not resize the parent terminal
    require(AllocConsole() != 0, "cannot allocate an isolated test console");
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE * terminal = std::fopen("CONOUT$", "w");
    require(terminal != nullptr, "cannot open test console output");
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    require(GetConsoleMode(input, &input_mode) && GetConsoleMode(output, &output_mode), "cannot read test console modes");

    auto resize = [&](SHORT columns, SHORT rows) {
        SMALL_RECT minimum = {0, 0, 0, 0};
        require(SetConsoleWindowInfo(output, TRUE, &minimum) != 0, "cannot shrink test viewport");
        require(SetConsoleScreenBufferSize(output, {columns, rows}) != 0, "cannot resize test buffer");
        SMALL_RECT viewport = {0, 0, static_cast<SHORT>(columns - 1), static_cast<SHORT>(rows - 1)};
        require(SetConsoleWindowInfo(output, TRUE, &viewport) != 0, "cannot resize test viewport");
        INPUT_RECORD event{};
        event.EventType = WINDOW_BUFFER_SIZE_EVENT;
        event.Event.WindowBufferSizeEvent.dwSize = {columns, rows};
        DWORD count = 0;
        require(WriteConsoleInputW(input, &event, 1, &count) && count == 1, "cannot deliver final resize event");
    };
    auto screen = [&]() {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        require(GetConsoleScreenBufferInfo(output, &info) != 0, "cannot read test console dimensions");
        std::wstring text(static_cast<size_t>(info.dwSize.X) * info.dwSize.Y, L' ');
        DWORD count = 0;
        require(ReadConsoleOutputCharacterW(output, text.data(), static_cast<DWORD>(text.size()), {0, 0}, &count) != 0, "cannot read test screen");
        text.resize(count);
        return text;
    };
    auto wait_for = [&](const std::function<bool()> & predicate, const char * message) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(predicate(), message);
    };
    auto key = [&](WORD vk, wchar_t ch, DWORD modifiers = 0) {
        INPUT_RECORD record{};
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.bKeyDown = TRUE;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.wVirtualKeyCode = vk;
        record.Event.KeyEvent.uChar.UnicodeChar = ch;
        record.Event.KeyEvent.dwControlKeyState = modifiers;
        DWORD count = 0;
        require(WriteConsoleInputW(input, &record, 1, &count) && count == 1, "cannot inject test key");
    };

    resize(80, 24);
    {
        tui_renderer::config cfg;
        cfg.out = terminal;
        cfg.color = false;
        cfg.meta.model_name = "native-console-model";
        tui_renderer renderer(cfg);
        wait_for([&]() { return screen().find(L"native-console-model") != std::wstring::npos; }, "native footer was not rendered");
        key('A', L'a');
        key(0, L'\x754c');
        wait_for([&]() { return screen().find(L"a\x754c") != std::wstring::npos; }, "native Unicode input was not rendered");
        CONSOLE_SCREEN_BUFFER_INFO info{};
        GetConsoleScreenBufferInfo(output, &info);
        require(info.dwCursorPosition.X == 5, "native Unicode cursor width mismatch");
        resize(60, 12);
        wait_for([&]() {
            GetConsoleScreenBufferInfo(output, &info);
            return info.dwCursorPosition.Y == 9 && screen().find(L"native-console-model") != std::wstring::npos;
        }, "native resize event did not redraw the editor");
        key(VK_RETURN, L'\r');
        tui_command command;
        require(renderer.wait_for_command(command, 3000) && command.text == "a\xE7\x95\x8C", "native input submission was corrupted");
        key('D', 4, LEFT_CTRL_PRESSED);
        require(renderer.wait_for_command(command, 3000) && command.eof, "native Ctrl+D did not produce EOF");
        renderer.shutdown();
    }
    DWORD restored = 0;
    require(GetConsoleMode(input, &restored) && restored == input_mode, "native input mode was not restored");
    require(GetConsoleMode(output, &restored) && restored == output_mode, "native output mode was not restored");
    std::fclose(terminal);
    FreeConsole();
    AttachConsole(ATTACH_PARENT_PROCESS);
}
#endif

int main(int argc, char ** argv) {
    std::setlocale(LC_ALL, "");
    if (argc == 3 && std::string(argv[1]) == "--terminal-fixture") {
        return terminal_fixture(argv[2]);
    }
    test_editor_submit_and_history();
    require(tui_codepoint_width('a') == 1, "ASCII width mismatch");
    require(tui_codepoint_width(0x754C) == 2, "wide character width mismatch");
    require(tui_codepoint_width(0x0301) == 0, "combining character width mismatch");
    test_editor_utf8_and_multiline_render();
    test_paste_preserves_text();
    test_multiline_mode();
    test_select_list();
    test_event_queue();
    test_split_json_escapes();
    test_footer_and_agent_event_projection();
#if defined(_WIN32)
    test_native_console();
#endif
    return 0;
}
