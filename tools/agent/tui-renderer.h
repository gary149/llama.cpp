#pragma once

#include "inference-backend.h"
#include "agent-loop-internal.h"
#include "permission-async.h"
#include "tui-editor.h"
#include "tui-events.h"
#include "tui-select-list.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <termios.h>
#endif

struct tui_command {
    std::string text;
    std::vector<std::pair<std::vector<uint8_t>, std::string>> images;
    bool eof = false;
};

struct tui_footer_state {
    std::string working_dir;
    std::string session_path;
    inference_backend_meta meta;
    session_stats stats;
    int32_t last_prompt_tokens = 0;
    bool generating = false;
    size_t spinner_frame = 0;
    std::string transient_label;
};

std::vector<std::string> tui_render_footer(const tui_footer_state & state, int width, bool color);
tui_event tui_event_from_agent_event(const agent_event & event);

class tui_renderer {
public:
    struct config {
        FILE * out = stdout;
        bool color = true;
        bool multiline_input = false;
        std::string working_dir;
        std::string session_path;
        inference_backend_meta meta;
        permission_manager_async * permissions = nullptr;
        std::function<void()> interrupt;
    };

    explicit tui_renderer(config cfg);
    ~tui_renderer();

    tui_renderer(const tui_renderer &) = delete;
    tui_renderer & operator=(const tui_renderer &) = delete;

    void post_event(tui_event event);
    void post_agent_event(const agent_event & event);
    void post_transcript(std::string text, tui_transcript_style style = tui_transcript_style::NORMAL);
    void post_stats(const session_stats & stats, int32_t last_prompt_tokens);
    void set_generating(bool generating);

    bool wait_for_command(tui_command & command, int timeout_ms = -1);
    void shutdown();

private:
    enum class overlay_kind {
        NONE,
        SLASH,
        FILE,
        PERMISSION,
    };

    void render_loop();
    void input_loop();

    void handle_tui_event(const tui_event & event);
    void handle_input_event(const tui_input_event & event);
    void submit_command(std::string text);

    void append_transcript_line(const std::string & text, tui_transcript_style style);
    void append_transcript_raw(const std::string & bytes);
    void flush_text_buffer(bool force_newline);
    void handle_tool_call_delta(const tui_event & event);
    void render_tool_start(const tui_event & event);
    void render_tool_result(const tui_event & event);

    void update_autocomplete();
    void start_file_completion_worker(const std::string & query, uint64_t generation);
    void complete_selected();
    void dismiss_overlay();
    void open_permission_overlay(const tui_event & event);
    void answer_permission(char ch);

    std::vector<std::string> current_region_lines(int & cursor_row, int & cursor_col);
    void redraw_managed_region(bool full_redraw = false);
    void clear_managed_region();
    void flush_pending_transcript();
    void repaint_screen();

    void query_terminal_size();
    void install_resize_handler();
    void restore_resize_handler();
    void setup_raw_mode();
    void restore_raw_mode();

    std::string sgr(tui_transcript_style style) const;
    std::string reset_sgr() const;

    config cfg_;
    FILE * out_ = stdout;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_started_{false};
    std::thread render_thread_;
    std::thread input_thread_;

    tui_event_queue<tui_event> events_;
    tui_event_queue<tui_input_event> input_events_;
    tui_event_queue<tui_command> commands_;

    tui_editor editor_;
    tui_select_list select_list_;
    overlay_kind overlay_ = overlay_kind::NONE;
    bool ignore_paste_ = false;
    size_t completion_start_ = 0;
    size_t completion_end_ = 0;
    std::string completion_prefix_;
    std::string last_file_query_;
    uint64_t file_completion_generation_ = 0;
    struct completion_worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex autocomplete_threads_mu_;
    std::vector<completion_worker> autocomplete_workers_;

    permission_request active_permission_;
    std::string active_permission_id_;

    tui_footer_state footer_;
    bool color_ = true;
    int term_rows_ = 24;
    int term_cols_ = 80;
    bool force_full_redraw_ = true;
    bool managed_visible_ = false;
    bool managed_dirty_ = true;
    std::vector<std::string> previous_region_;
    int last_drawn_top_ = 0;  // absolute row the region was last drawn at (pre-resize coords)

    std::string transcript_buffer_;
    tui_transcript_style buffer_style_ = tui_transcript_style::NORMAL;
    std::vector<std::string> pending_transcript_;
    std::vector<std::string> transcript_history_;  // committed transcript, for repaint on resize
    bool needs_full_repaint_ = false;
    static constexpr size_t kMaxTranscriptHistory = 500;
    std::map<size_t, tool_stream_state> tool_delta_states_;

    std::chrono::steady_clock::time_point last_spinner_tick_;
    std::chrono::steady_clock::time_point last_idle_tick_;
    std::chrono::steady_clock::time_point transient_until_;

#if defined(_WIN32)
    unsigned long saved_input_mode_ = 0;
    unsigned long saved_output_mode_ = 0;
    unsigned int saved_output_cp_ = 0;
    bool saved_input_mode_valid_ = false;
    bool saved_output_mode_valid_ = false;
#else
    termios saved_termios_{};
    bool saved_termios_valid_ = false;
    struct sigaction previous_sigwinch_{};
    bool sigwinch_installed_ = false;
#endif
};
