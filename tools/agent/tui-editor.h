#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class tui_input_key {
    CHARACTER,
    ENTER,
    ALT_ENTER,
    TAB,
    ESCAPE,
    BACKSPACE,
    DELETE_KEY,
    LEFT,
    RIGHT,
    UP,
    DOWN,
    HOME,
    END,
    CTRL_A,
    CTRL_C,
    CTRL_E,
    CTRL_D,
    CTRL_V,
    CTRL_LEFT,
    CTRL_RIGHT,
    PASTE_START,
    PASTE_END,
    RESIZE,
};

struct tui_input_event {
    tui_input_key key = tui_input_key::CHARACTER;
    char32_t codepoint = 0;
};

struct tui_editor_action {
    bool changed = false;
    bool submitted = false;
    bool eof = false;
    std::string submission;
};

struct tui_editor_render {
    std::vector<std::string> lines;
    int cursor_row = 0;
    int cursor_col = 0;
};

class tui_editor {
public:
    explicit tui_editor(bool multiline_input);

    tui_editor_action handle_event(const tui_input_event & ev);

    void set_buffer(const std::string & text);
    void set_buffer(const std::string & text, size_t cursor_pos);
    std::string buffer() const;
    bool empty() const;
    bool pasting() const { return paste_mode_; }

    size_t cursor_byte_pos() const;
    void complete_range(size_t start, size_t end, const std::string & replacement);

    tui_editor_render render(int width, int max_lines) const;

private:
    struct history_t {
        std::vector<std::string> entries;
        size_t viewing_idx = SIZE_MAX;
        std::string backup_line;

        void add(std::string_view line);
        bool prev(std::string & cur_line);
        bool next(std::string & cur_line);
        void begin_viewing(const std::string & line);
        void end_viewing();
        bool is_viewing() const;
    };

    void insert_codepoint(char32_t cp);
    void insert_text(const std::string & text);
    void insert_newline();
    void backspace();
    void delete_at_cursor();
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void move_home();
    void move_end();
    void move_word_left();
    void move_word_right();
    void history_prev();
    void history_next();
    void normalize_cursor();
    void replace_from_flat_buffer(const std::string & text, size_t cursor_pos = std::string::npos);
    void finish_paste();

    bool multiline_input_ = false;
    std::vector<std::string> lines_;
    size_t cursor_line_ = 0;
    size_t cursor_col_bytes_ = 0;
    history_t history_;

    bool paste_mode_ = false;
    std::string paste_buffer_;
};

char32_t tui_decode_utf8(const std::string & input, size_t pos, size_t & advance);
void tui_append_utf8(char32_t ch, std::string & out);
size_t tui_prev_utf8_char_pos(const std::string & line, size_t pos);
size_t tui_next_utf8_char_pos(const std::string & line, size_t pos);
int tui_codepoint_width(char32_t codepoint);
int tui_string_width(const std::string & text, int initial_column = 0);
