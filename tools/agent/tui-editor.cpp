#include "tui-editor.h"

#include <algorithm>
#include <cassert>
#include <cwchar>
#include <cwctype>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <wchar.h>
#endif

char32_t tui_decode_utf8(const std::string & input, size_t pos, size_t & advance) {
    unsigned char c = static_cast<unsigned char>(input[pos]);
    if ((c & 0x80u) == 0u) {
        advance = 1;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u && pos + 1 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        if ((c1 & 0xC0u) != 0x80u) {
            advance = 1;
            return 0xFFFD;
        }
        advance = 2;
        return ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && pos + 2 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
            advance = 1;
            return 0xFFFD;
        }
        advance = 3;
        return ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && pos + 3 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
        unsigned char c3 = static_cast<unsigned char>(input[pos + 3]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
            advance = 1;
            return 0xFFFD;
        }
        advance = 4;
        return ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
    }

    advance = 1;
    return 0xFFFD;
}

void tui_append_utf8(char32_t ch, std::string & out) {
    if (ch <= 0x7F) {
        out.push_back(static_cast<char>(ch));
    } else if (ch <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((ch >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}

size_t tui_prev_utf8_char_pos(const std::string & line, size_t pos) {
    if (pos == 0) {
        return 0;
    }
    pos--;
    while (pos > 0 && (static_cast<unsigned char>(line[pos]) & 0xC0u) == 0x80u) {
        pos--;
    }
    return pos;
}

size_t tui_next_utf8_char_pos(const std::string & line, size_t pos) {
    if (pos >= line.size()) {
        return line.size();
    }
    pos++;
    while (pos < line.size() && (static_cast<unsigned char>(line[pos]) & 0xC0u) == 0x80u) {
        pos++;
    }
    return pos;
}

int tui_codepoint_width(char32_t codepoint) {
#if defined(_WIN32)
    if (codepoint == 0x200D || (codepoint >= 0xE0100 && codepoint <= 0xE01EF)) {
        return 0;
    }
    if (codepoint > 0xFFFF) {
        return (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) || (codepoint >= 0x20000 && codepoint <= 0x3FFFD) ? 2 : 1;
    }
    wchar_t ch = static_cast<wchar_t>(codepoint);
    WORD type = 0;
    if (GetStringTypeW(CT_CTYPE3, &ch, 1, &type)) {
        if (type & C3_NONSPACING) {
            return 0;
        }
        if (type & (C3_FULLWIDTH | C3_IDEOGRAPH)) {
            return 2;
        }
    }
    return 1;
#else
    int w = wcwidth(static_cast<wchar_t>(codepoint));
    return w < 0 ? 1 : w;
#endif
}

int tui_string_width(const std::string & text, int initial_column) {
    int width = initial_column;
    for (size_t i = 0; i < text.size();) {
        size_t advance = 0;
        char32_t cp = tui_decode_utf8(text, i, advance);
        width += cp == '\t' ? 8 - width % 8 : tui_codepoint_width(cp);
        i += advance;
    }
    return width - initial_column;
}

void tui_editor::history_t::add(std::string_view line) {
    if (line.empty()) {
        return;
    }
    if (entries.empty() || entries.back() != line) {
        entries.emplace_back(line);
    }
    end_viewing();
}

bool tui_editor::history_t::prev(std::string & cur_line) {
    if (entries.empty() || viewing_idx == SIZE_MAX) {
        return false;
    }
    if (viewing_idx > 0) {
        viewing_idx--;
    }
    cur_line = entries[viewing_idx];
    return true;
}

bool tui_editor::history_t::next(std::string & cur_line) {
    if (entries.empty() || viewing_idx == SIZE_MAX) {
        return false;
    }
    viewing_idx++;
    if (viewing_idx >= entries.size()) {
        cur_line = backup_line;
        end_viewing();
    } else {
        cur_line = entries[viewing_idx];
    }
    return true;
}

void tui_editor::history_t::begin_viewing(const std::string & line) {
    backup_line = line;
    viewing_idx = entries.size();
}

void tui_editor::history_t::end_viewing() {
    viewing_idx = SIZE_MAX;
    backup_line.clear();
}

bool tui_editor::history_t::is_viewing() const {
    return viewing_idx != SIZE_MAX;
}

tui_editor::tui_editor(bool multiline_input)
    : multiline_input_(multiline_input)
    , lines_(1)
{
}

bool tui_editor::empty() const {
    return lines_.size() == 1 && lines_[0].empty();
}

std::string tui_editor::buffer() const {
    std::string result;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) {
            result.push_back('\n');
        }
        result += lines_[i];
    }
    return result;
}

void tui_editor::set_buffer(const std::string & text) {
    replace_from_flat_buffer(text, text.size());
}

void tui_editor::set_buffer(const std::string & text, size_t cursor_pos) {
    replace_from_flat_buffer(text, cursor_pos);
}

size_t tui_editor::cursor_byte_pos() const {
    size_t pos = 0;
    for (size_t i = 0; i < cursor_line_; ++i) {
        pos += lines_[i].size() + 1;
    }
    return pos + cursor_col_bytes_;
}

void tui_editor::replace_from_flat_buffer(const std::string & text, size_t cursor_pos) {
    lines_.clear();
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines_.push_back(text.substr(start));
            break;
        }
        lines_.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if (lines_.empty()) {
        lines_.push_back("");
    }

    if (cursor_pos == std::string::npos) {
        cursor_pos = text.size();
    }
    cursor_line_ = 0;
    cursor_col_bytes_ = 0;
    size_t remaining = std::min(cursor_pos, text.size());
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (remaining <= lines_[i].size()) {
            cursor_line_ = i;
            cursor_col_bytes_ = remaining;
            normalize_cursor();
            return;
        }
        remaining -= lines_[i].size();
        if (remaining == 0) {
            cursor_line_ = i;
            cursor_col_bytes_ = lines_[i].size();
            return;
        }
        remaining--;
    }
    cursor_line_ = lines_.size() - 1;
    cursor_col_bytes_ = lines_.back().size();
}

void tui_editor::complete_range(size_t start, size_t end, const std::string & replacement) {
    std::string cur = buffer();
    start = std::min(start, cur.size());
    end = std::min(std::max(end, start), cur.size());
    cur.replace(start, end - start, replacement);
    replace_from_flat_buffer(cur, start + replacement.size());
    history_.end_viewing();
}

void tui_editor::normalize_cursor() {
    if (lines_.empty()) {
        lines_.push_back("");
    }
    cursor_line_ = std::min(cursor_line_, lines_.size() - 1);
    cursor_col_bytes_ = std::min(cursor_col_bytes_, lines_[cursor_line_].size());
    while (cursor_col_bytes_ > 0 &&
           (static_cast<unsigned char>(lines_[cursor_line_][cursor_col_bytes_]) & 0xC0u) == 0x80u) {
        cursor_col_bytes_--;
    }
}

void tui_editor::insert_codepoint(char32_t cp) {
    std::string text;
    tui_append_utf8(cp, text);
    insert_text(text);
}

void tui_editor::insert_text(const std::string & text) {
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\n') {
            insert_newline();
            i++;
            continue;
        }
        size_t advance = 0;
        (void) tui_decode_utf8(text, i, advance);
        lines_[cursor_line_].insert(cursor_col_bytes_, text, i, advance);
        cursor_col_bytes_ += advance;
        i += advance;
    }
    history_.end_viewing();
}

void tui_editor::insert_newline() {
    std::string tail = lines_[cursor_line_].substr(cursor_col_bytes_);
    lines_[cursor_line_].erase(cursor_col_bytes_);
    lines_.insert(lines_.begin() + cursor_line_ + 1, tail);
    cursor_line_++;
    cursor_col_bytes_ = 0;
}

void tui_editor::backspace() {
    if (cursor_col_bytes_ > 0) {
        size_t prev = tui_prev_utf8_char_pos(lines_[cursor_line_], cursor_col_bytes_);
        lines_[cursor_line_].erase(prev, cursor_col_bytes_ - prev);
        cursor_col_bytes_ = prev;
        history_.end_viewing();
        return;
    }
    if (cursor_line_ > 0) {
        size_t prev_size = lines_[cursor_line_ - 1].size();
        lines_[cursor_line_ - 1] += lines_[cursor_line_];
        lines_.erase(lines_.begin() + cursor_line_);
        cursor_line_--;
        cursor_col_bytes_ = prev_size;
        history_.end_viewing();
    }
}

void tui_editor::delete_at_cursor() {
    if (cursor_col_bytes_ < lines_[cursor_line_].size()) {
        size_t next = tui_next_utf8_char_pos(lines_[cursor_line_], cursor_col_bytes_);
        lines_[cursor_line_].erase(cursor_col_bytes_, next - cursor_col_bytes_);
        history_.end_viewing();
        return;
    }
    if (cursor_line_ + 1 < lines_.size()) {
        lines_[cursor_line_] += lines_[cursor_line_ + 1];
        lines_.erase(lines_.begin() + cursor_line_ + 1);
        history_.end_viewing();
    }
}

void tui_editor::move_left() {
    if (cursor_col_bytes_ > 0) {
        cursor_col_bytes_ = tui_prev_utf8_char_pos(lines_[cursor_line_], cursor_col_bytes_);
    } else if (cursor_line_ > 0) {
        cursor_line_--;
        cursor_col_bytes_ = lines_[cursor_line_].size();
    }
}

void tui_editor::move_right() {
    if (cursor_col_bytes_ < lines_[cursor_line_].size()) {
        cursor_col_bytes_ = tui_next_utf8_char_pos(lines_[cursor_line_], cursor_col_bytes_);
    } else if (cursor_line_ + 1 < lines_.size()) {
        cursor_line_++;
        cursor_col_bytes_ = 0;
    }
}

void tui_editor::move_home() {
    cursor_col_bytes_ = 0;
}

void tui_editor::move_end() {
    cursor_col_bytes_ = lines_[cursor_line_].size();
}

void tui_editor::move_up() {
    if (cursor_line_ == 0) {
        history_prev();
        return;
    }
    size_t target_width = tui_string_width(lines_[cursor_line_].substr(0, cursor_col_bytes_));
    cursor_line_--;
    cursor_col_bytes_ = 0;
    int width = 0;
    while (cursor_col_bytes_ < lines_[cursor_line_].size()) {
        size_t advance = 0;
        char32_t cp = tui_decode_utf8(lines_[cursor_line_], cursor_col_bytes_, advance);
        int next_width = width + tui_codepoint_width(cp);
        if (next_width > static_cast<int>(target_width)) {
            break;
        }
        width = next_width;
        cursor_col_bytes_ += advance;
    }
}

void tui_editor::move_down() {
    if (cursor_line_ + 1 >= lines_.size()) {
        history_next();
        return;
    }
    size_t target_width = tui_string_width(lines_[cursor_line_].substr(0, cursor_col_bytes_));
    cursor_line_++;
    cursor_col_bytes_ = 0;
    int width = 0;
    while (cursor_col_bytes_ < lines_[cursor_line_].size()) {
        size_t advance = 0;
        char32_t cp = tui_decode_utf8(lines_[cursor_line_], cursor_col_bytes_, advance);
        int next_width = width + tui_codepoint_width(cp);
        if (next_width > static_cast<int>(target_width)) {
            break;
        }
        width = next_width;
        cursor_col_bytes_ += advance;
    }
}

static bool tui_is_space_codepoint(char32_t cp) {
    return std::iswspace(static_cast<wint_t>(cp)) != 0;
}

void tui_editor::move_word_left() {
    std::string cur = buffer();
    size_t pos = cursor_byte_pos();
    while (pos > 0) {
        size_t prev = tui_prev_utf8_char_pos(cur, pos);
        size_t adv = 0;
        char32_t cp = tui_decode_utf8(cur, prev, adv);
        if (!tui_is_space_codepoint(cp)) {
            break;
        }
        pos = prev;
    }
    while (pos > 0) {
        size_t prev = tui_prev_utf8_char_pos(cur, pos);
        size_t adv = 0;
        char32_t cp = tui_decode_utf8(cur, prev, adv);
        if (tui_is_space_codepoint(cp)) {
            break;
        }
        pos = prev;
    }
    replace_from_flat_buffer(cur, pos);
}

void tui_editor::move_word_right() {
    std::string cur = buffer();
    size_t pos = cursor_byte_pos();
    while (pos < cur.size()) {
        size_t adv = 0;
        char32_t cp = tui_decode_utf8(cur, pos, adv);
        if (!tui_is_space_codepoint(cp)) {
            break;
        }
        pos += adv;
    }
    while (pos < cur.size()) {
        size_t adv = 0;
        char32_t cp = tui_decode_utf8(cur, pos, adv);
        if (tui_is_space_codepoint(cp)) {
            break;
        }
        pos += adv;
    }
    while (pos < cur.size()) {
        size_t adv = 0;
        char32_t cp = tui_decode_utf8(cur, pos, adv);
        if (!tui_is_space_codepoint(cp)) {
            break;
        }
        pos += adv;
    }
    replace_from_flat_buffer(cur, pos);
}

void tui_editor::history_prev() {
    if (!history_.is_viewing()) {
        history_.begin_viewing(buffer());
    }
    std::string line;
    if (history_.prev(line)) {
        replace_from_flat_buffer(line);
    }
}

void tui_editor::history_next() {
    if (!history_.is_viewing()) {
        return;
    }
    std::string line;
    if (history_.next(line)) {
        replace_from_flat_buffer(line);
    }
}

void tui_editor::finish_paste() {
    paste_mode_ = false;
    insert_text(paste_buffer_);
    paste_buffer_.clear();
}

tui_editor_action tui_editor::handle_event(const tui_input_event & ev) {
    tui_editor_action action;

    if (paste_mode_) {
        if (ev.key == tui_input_key::PASTE_END) {
            finish_paste();
            action.changed = true;
            return action;
        }
        if (ev.key == tui_input_key::ENTER || ev.key == tui_input_key::ALT_ENTER) {
            paste_buffer_.push_back('\n');
            action.changed = true;
            return action;
        }
        if (ev.key == tui_input_key::TAB) {
            paste_buffer_.push_back('\t');
            return action;
        }
        if (ev.key == tui_input_key::CHARACTER) {
            tui_append_utf8(ev.codepoint, paste_buffer_);
            action.changed = true;
        }
        return action;
    }

    switch (ev.key) {
        case tui_input_key::CHARACTER:
            // Intercept C0 control characters that the terminal sends as raw bytes
            // and the input loop forwards as CHARACTER events.
            // 0x0B = Ctrl+K: kill to end of line
            // 0x15 = Ctrl+U: kill to beginning of line
            if (ev.codepoint == 0x0B) {
                // Kill from cursor to end of current logical line
                lines_[cursor_line_].erase(cursor_col_bytes_);
                history_.end_viewing();
                action.changed = true;
                break;
            }
            if (ev.codepoint == 0x15) {
                // Kill from beginning of current logical line to cursor
                lines_[cursor_line_].erase(0, cursor_col_bytes_);
                cursor_col_bytes_ = 0;
                history_.end_viewing();
                action.changed = true;
                break;
            }
            // Ignore other unhandled C0/C1 control characters to prevent
            // invisible characters from corrupting the buffer.
            if (ev.codepoint < 0x20 || (ev.codepoint >= 0x7F && ev.codepoint < 0xA0)) {
                break;
            }
            insert_codepoint(ev.codepoint);
            action.changed = true;
            break;
        case tui_input_key::ENTER:
        case tui_input_key::ALT_ENTER:
            if ((ev.key == tui_input_key::ENTER) == multiline_input_) {
                insert_newline();
                action.changed = true;
                break;
            }
            action.submitted = true;
            action.submission = buffer();
            history_.add(action.submission);
            replace_from_flat_buffer("");
            action.changed = true;
            break;
        case tui_input_key::BACKSPACE:
            backspace();
            action.changed = true;
            break;
        case tui_input_key::DELETE_KEY:
            delete_at_cursor();
            action.changed = true;
            break;
        case tui_input_key::LEFT:
            move_left();
            action.changed = true;
            break;
        case tui_input_key::RIGHT:
            move_right();
            action.changed = true;
            break;
        case tui_input_key::UP:
            move_up();
            action.changed = true;
            break;
        case tui_input_key::DOWN:
            move_down();
            action.changed = true;
            break;
        case tui_input_key::HOME:
        case tui_input_key::CTRL_A:
            move_home();
            action.changed = true;
            break;
        case tui_input_key::END:
        case tui_input_key::CTRL_E:
            move_end();
            action.changed = true;
            break;
        case tui_input_key::CTRL_LEFT:
            move_word_left();
            action.changed = true;
            break;
        case tui_input_key::CTRL_RIGHT:
            move_word_right();
            action.changed = true;
            break;
        case tui_input_key::PASTE_START:
            paste_mode_ = true;
            paste_buffer_.clear();
            action.changed = true;
            break;
        case tui_input_key::CTRL_D:
            if (empty()) {
                action.eof = true;
            } else {
                delete_at_cursor();
                action.changed = true;
            }
            break;
        case tui_input_key::TAB:
        case tui_input_key::CTRL_C:
        case tui_input_key::ESCAPE:
        case tui_input_key::CTRL_V:
        case tui_input_key::PASTE_END:
        case tui_input_key::RESIZE:
            break;
    }

    normalize_cursor();
    return action;
}

tui_editor_render tui_editor::render(int width, int max_lines) const {
    tui_editor_render out;
    width = std::max(width, 1);
    max_lines = std::max(max_lines, 1);

    struct visual_line {
        std::string text;
        size_t logical_line = 0;
        size_t start_byte = 0;
        size_t end_byte = 0;
        int prefix_width = 0;
    };

    std::vector<visual_line> visual;
    const int prompt_width = width > 2 ? 2 : 0;
    const std::string prompt = prompt_width ? "\xE2\x80\xBA " : "";
    const std::string cont(prompt_width, ' ');

    for (size_t li = 0; li < lines_.size(); ++li) {
        const std::string & line = lines_[li];
        size_t segment_start = 0;
        std::string segment = (li == 0) ? prompt : cont;
        int col = prompt_width;
        int prefix = prompt_width;

        for (size_t pos = 0; pos < line.size();) {
            size_t advance = 0;
            char32_t cp = tui_decode_utf8(line, pos, advance);
            int w = cp == '\t' ? 8 - col % 8 : tui_codepoint_width(cp);
            if (col + w > width && col > prefix) {
                visual.push_back({segment, li, segment_start, pos, prefix});
                segment = cont;
                col = prompt_width;
                prefix = prompt_width;
                segment_start = pos;
                w = cp == '\t' ? 8 - col % 8 : tui_codepoint_width(cp);
            }
            if (cp == '\t') {
                w = std::min(w, width - col);
                segment.append(w, ' ');
            } else {
                segment.append(line, pos, advance);
            }
            col += w;
            pos += advance;
        }
        visual.push_back({segment, li, segment_start, line.size(), prefix});
        if (col >= width) {
            visual.push_back({cont, li, line.size(), line.size(), prompt_width});
        }
    }

    int cursor_visual = 0;
    int cursor_col = prompt_width;
    for (size_t i = 0; i < visual.size(); ++i) {
        const auto & vl = visual[i];
        if (vl.logical_line != cursor_line_) {
            continue;
        }
        if (cursor_col_bytes_ >= vl.start_byte && cursor_col_bytes_ <= vl.end_byte) {
            cursor_visual = static_cast<int>(i);
            cursor_col = vl.prefix_width +
                tui_string_width(lines_[cursor_line_].substr(vl.start_byte, cursor_col_bytes_ - vl.start_byte), vl.prefix_width);
        }
    }

    int first = 0;
    if (static_cast<int>(visual.size()) > max_lines) {
        first = std::max(0, cursor_visual - max_lines + 1);
        if (first + max_lines > static_cast<int>(visual.size())) {
            first = static_cast<int>(visual.size()) - max_lines;
        }
    }

    int last = std::min(static_cast<int>(visual.size()), first + max_lines);
    for (int i = first; i < last; ++i) {
        out.lines.push_back(visual[i].text);
    }
    if (out.lines.empty()) {
        out.lines.push_back(prompt);
    }

    out.cursor_row = std::max(0, cursor_visual - first);
    out.cursor_col = std::max(0, cursor_col);
    if (out.cursor_row >= static_cast<int>(out.lines.size())) {
        out.cursor_row = static_cast<int>(out.lines.size()) - 1;
        out.cursor_col = tui_string_width(out.lines.back());
    }
    return out;
}
