#pragma once

#include "agent-loop.h"
#include "permission.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

enum class tui_event_type {
    TEXT_DELTA,
    REASONING_DELTA,
    TOOL_CALL_DELTA,
    TOOL_START,
    TOOL_RESULT,
    COMPACTION_COMPLETED,
    COMPLETED,
    FAILURE,
    PERMISSION_REQUIRED,
    PERMISSION_RESOLVED,
    ITERATION_START,
    AUTOCOMPLETE_RESULTS,
    TRANSCRIPT,
    STATS,
    RESIZE,
    SHUTDOWN,
};

enum class tui_transcript_style {
    NORMAL,
    INFO,
    FAILURE,
    REASONING,
    TOOL_STREAM,
    USER_INPUT,
};

struct tui_event {
    tui_event_type type = tui_event_type::TEXT_DELTA;

    std::string text;
    tui_transcript_style transcript_style = tui_transcript_style::NORMAL;

    std::string tool_name;
    std::string tool_args;
    std::string tool_output;
    bool        tool_success = true;
    size_t      tool_call_index = std::string::npos;
    int64_t     elapsed_ms = 0;

    int32_t messages_kept = 0;
    session_stats stats;
    int32_t last_prompt_tokens = 0;

    permission_request perm;
    std::string perm_id;
    bool perm_allowed = false;

    std::vector<std::string> autocomplete_items;
    uint64_t autocomplete_generation = 0;
    std::string autocomplete_query;
};

template <typename T>
class tui_event_queue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_) {
                return;
            }
            q_.push(std::move(value));
        }
        cv_.notify_one();
    }

    bool try_pop(T & out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    bool wait_pop(T & out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mu_);
        if (timeout_ms < 0) {
            cv_.wait(lock, [&] { return closed_ || !q_.empty(); });
        } else {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                [&] { return closed_ || !q_.empty(); });
        }
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closed_;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::queue<T> q_;
    bool closed_ = false;
};
