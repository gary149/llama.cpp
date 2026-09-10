#pragma once

#include "tui-editor.h"

#include <string>
#include <vector>

class tui_select_list {
public:
    void set_items(std::vector<std::string> items);
    void clear();
    bool visible() const;
    bool empty() const;

    void move_up();
    void move_down();
    const std::string * selected() const;

    std::vector<std::string> render(int width, int max_visible) const;

private:
    std::vector<std::string> items_;
    size_t selected_ = 0;
};
