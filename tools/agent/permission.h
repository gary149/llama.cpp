#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

enum class permission_state {
    ALLOW,         // Auto-execute
    ASK,           // Prompt user
    DENY,          // Block
    ALLOW_SESSION, // User chose "always" for this session
    DENY_SESSION   // User chose "never" for this session
};

enum class permission_type {
    BASH,
    FILE_READ,
    FILE_WRITE,
    FILE_EDIT,
    GLOB,
    EXTERNAL_DIR,  // Operation outside working directory
};

struct permission_request {
    permission_type type = permission_type::BASH;
    std::string tool_name;
    std::string description;
    std::string details;  // command, file path, etc.
    bool is_dangerous = false;
};

// Generate a session override key for a permission request.
// MCP tools are keyed by name only (arguments vary per call).
// Other tools are keyed by name:details for more granular control.
inline std::string permission_override_key(const std::string & tool_name, const std::string & details) {
    if (tool_name.rfind("mcp__", 0) == 0) {
        return tool_name;
    }
    return tool_name + ":" + details;
}

class permission_policy {
public:
    permission_policy();

    void set_project_root(const std::string & path);

    permission_state classify(
            const permission_request & request,
            bool yolo_mode,
            const std::map<std::string, permission_state> & session_overrides) const;

    bool is_external_path(const std::string & path) const;
    bool is_dangerous_bash_command(const std::string & cmd) const;

private:
    std::string project_root_;
    std::map<permission_type, permission_state> defaults_;
    std::vector<std::string> dangerous_patterns_;
    std::vector<std::string> safe_patterns_;

    static bool is_compound_command(const std::string & cmd);
    // Prefix match: used for safe patterns, where a false positive would wrongly auto-allow.
    static bool matches_pattern(const std::string & cmd, const std::vector<std::string> & patterns);
    // Substring match: used for dangerous patterns, so embedded forms in compound
    // commands (e.g. "cd build && rm -rf /") are still flagged.
    static bool contains_pattern(const std::string & cmd, const std::vector<std::string> & patterns);
    bool is_path_in_project(const std::string & path) const;
};

enum class permission_response {
    ALLOW_ONCE,
    DENY_ONCE,
    ALLOW_ALWAYS,
    DENY_ALWAYS,
};

class permission_manager {
public:
    permission_manager();

    // Set project root for external directory checks
    void set_project_root(const std::string & path);

    // Enable yolo mode (skip all permission prompts)
    void set_yolo_mode(bool enabled) { yolo_mode_ = enabled; }

    // Check if a tool execution is allowed
    permission_state check_permission(const permission_request & request);

    // Interactive prompt for permission (returns user's choice)
    permission_response prompt_user(const permission_request & request);

    // Record a tool call for doom-loop detection
    void record_tool_call(const std::string & tool, const std::string & args_hash);

    // Check for doom-loop (3+ identical calls)
    bool is_doom_loop(const std::string & tool, const std::string & args_hash) const;

    // Clear session state
    void clear_session();

private:
    std::string project_root_;
    bool yolo_mode_ = false;
    std::map<std::string, permission_state> session_overrides_;
    permission_policy policy_;

    // Recent tool calls for doom-loop detection
    struct tool_call_record {
        std::string tool;
        std::string args_hash;
        int count;
    };
    std::vector<tool_call_record> recent_calls_;

public:
    // Check if a file path is sensitive (should be blocked)
    static bool is_sensitive_file(const std::string & path);

    // Check if path is outside working directory
    bool is_external_path(const std::string & path) const;

    // Check if a bash command matches the shared dangerous-command policy
    bool is_dangerous_bash_command(const std::string & cmd) const;
};
