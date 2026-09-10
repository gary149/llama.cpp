#include "tui-select-list.h"

#include <algorithm>
#include <utility>

void tui_select_list::set_items(std::vector<std::string> items) {
    items_ = std::move(items);
    selected_ = 0;
}

void tui_select_list::clear() {
    items_.clear();
    selected_ = 0;
}

bool tui_select_list::visible() const {
    return !items_.empty();
}

bool tui_select_list::empty() const {
    return items_.empty();
}

void tui_select_list::move_up() {
    if (items_.empty()) {
        return;
    }
    selected_ = selected_ == 0 ? items_.size() - 1 : selected_ - 1;
}

void tui_select_list::move_down() {
    if (items_.empty()) {
        return;
    }
    selected_ = (selected_ + 1) % items_.size();
}

const std::string * tui_select_list::selected() const {
    if (items_.empty()) {
        return nullptr;
    }
    return &items_[selected_];
}

std::vector<std::string> tui_select_list::render(int width, int max_visible) const {
    std::vector<std::string> lines;
    if (items_.empty()) {
        return lines;
    }

    width = std::max(width, 8);
    max_visible = std::max(max_visible, 1);

    int start = 0;
    if (selected_ >= static_cast<size_t>(max_visible)) {
        start = static_cast<int>(selected_) - max_visible + 1;
    }
    int end = std::min(static_cast<int>(items_.size()), start + max_visible);

    for (int i = start; i < end; ++i) {
        std::string prefix = i == static_cast<int>(selected_) ? "> " : "  ";
        std::string row = prefix + items_[i];
        int row_width = tui_string_width(row);
        if (row_width > width) {
            while (!row.empty() && tui_string_width(row + "...") > width) {
                row.resize(tui_prev_utf8_char_pos(row, row.size()));
            }
            row += "...";
        }
        lines.push_back(row);
    }
    return lines;
}
