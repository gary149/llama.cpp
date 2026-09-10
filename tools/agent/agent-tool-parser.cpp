#include "agent-tool-parser.h"
#include "agent-loop-internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>

static std::string trim_copy(const std::string & input) {
    size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) {
        first++;
    }
    size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) {
        last--;
    }
    return input.substr(first, last - first);
}

static bool ends_with(const std::string & value, const std::string & suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Return the index one past the bracket that closes the JSON object or array starting at `pos`, or npos if unbalanced
static size_t find_balanced_json_end(const std::string & s, size_t pos) {
    if (pos >= s.size() || (s[pos] != '{' && s[pos] != '[')) {
        return std::string::npos;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return std::string::npos;
}

std::string agent_render_tool_protocol_prompt(const std::vector<common_chat_tool> & tools) {
    if (tools.empty()) {
        return "";
    }

    std::ostringstream out;
    out << "\n# Tool Call Protocol\n\n";
    out << "When you need to use a tool, emit exactly one XML-style block and no prose after it:\n\n";
    out << "<tool_call>{\"name\":\"tool_name\",\"arguments\":{...}}</tool_call>\n\n";
    out << "Use only the tools listed below. `arguments` must be a JSON object matching the schema.\n";
    out << "After a tool result is returned, continue normally or emit another tool call if needed.\n\n";
    out << "## Available Tool Schemas\n\n";

    for (const auto & tool : tools) {
        out << "### " << tool.name << "\n";
        if (!tool.description.empty()) {
            out << tool.description << "\n";
        }
        out << "Parameters:\n";
        out << tool.parameters << "\n\n";
    }

    return out.str();
}

json agent_inject_tool_protocol_prompt(
    const json & messages,
    const std::vector<common_chat_tool> & tools) {

    std::string prompt = agent_render_tool_protocol_prompt(tools);
    if (prompt.empty()) {
        return messages;
    }

    json out = messages;
    if (out.is_array() && !out.empty() &&
        out[0].value("role", "") == "system" &&
        out[0].contains("content") &&
        out[0]["content"].is_string()) {
        out[0]["content"] = out[0]["content"].get<std::string>() + prompt;
        return out;
    }

    json system_msg = {
        {"role", "system"},
        {"content", prompt},
    };

    if (!out.is_array()) {
        out = json::array();
    }
    out.insert(out.begin(), system_msg);
    return out;
}

static void append_tool_call_from_json(
    const json & item,
    const std::set<std::string> & allowed_tools,
    std::vector<common_chat_tool_call> & out) {

    json fn = item;
    if (item.contains("function") && item["function"].is_object()) {
        fn = item["function"];
    }

    std::string name = fn.value("name", "");
    if (name.empty() || (!allowed_tools.empty() && allowed_tools.find(name) == allowed_tools.end())) {
        return;
    }

    common_chat_tool_call call;
    call.id = item.value("id", "call_" + std::to_string(out.size()));
    call.name = name;

    if (fn.contains("arguments")) {
        const auto & arguments = fn["arguments"];
        call.arguments = arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
    } else {
        call.arguments = "{}";
    }

    out.push_back(std::move(call));
}

static void append_tool_calls_from_json(
    const json & parsed,
    const std::set<std::string> & allowed_tools,
    std::vector<common_chat_tool_call> & out) {

    if (parsed.is_array()) {
        for (const auto & item : parsed) {
            append_tool_call_from_json(item, allowed_tools, out);
        }
        return;
    }

    if (parsed.is_object() && parsed.contains("tool_calls") && parsed["tool_calls"].is_array()) {
        for (const auto & item : parsed["tool_calls"]) {
            append_tool_call_from_json(item, allowed_tools, out);
        }
        return;
    }

    append_tool_call_from_json(parsed, allowed_tools, out);
}

common_chat_msg agent_parse_tool_protocol_response(
    const std::string & content,
    const std::string & reasoning_content,
    const std::vector<common_chat_tool> & tools) {

    std::set<std::string> allowed_tools;
    for (const auto & tool : tools) {
        allowed_tools.insert(tool.name);
    }

    common_chat_msg msg;
    msg.role = "assistant";
    msg.reasoning_content = reasoning_content;

    std::string visible_content = content;
    std::vector<common_chat_tool_call> calls;

    auto extract_tag = [&](const std::string & open_tag, const std::string & close_tag) {
        size_t search_from = 0;
        while (true) {
            size_t start = visible_content.find(open_tag, search_from);
            if (start == std::string::npos) {
                break;
            }
            size_t body_start = start + open_tag.size();
            size_t end = visible_content.find(close_tag, body_start);

            if (end == std::string::npos) {
                // No closing tag: recover a complete JSON payload by bracket balancing
                size_t json_start = visible_content.find_first_of("{[", body_start);
                if (json_start == std::string::npos) {
                    break;
                }
                size_t json_end = find_balanced_json_end(visible_content, json_start);
                if (json_end == std::string::npos) {
                    break;
                }
                std::string body = trim_copy(visible_content.substr(json_start, json_end - json_start));
                try {
                    append_tool_calls_from_json(json::parse(body), allowed_tools, calls);
                } catch (...) {
                    search_from = body_start;
                    continue;
                }
                visible_content.erase(start, json_end - start);
                search_from = start;
                continue;
            }

            std::string body = trim_copy(visible_content.substr(body_start, end - body_start));
            try {
                append_tool_calls_from_json(json::parse(body), allowed_tools, calls);
            } catch (...) {
                // Leave malformed blocks as visible content.
                search_from = body_start;
                continue;
            }

            visible_content.erase(start, end + close_tag.size() - start);
            search_from = start;
        }
    };

    extract_tag("<tool_call>", "</tool_call>");
    extract_tag("<tool_calls>", "</tool_calls>");

    if (calls.empty()) {
        std::string trimmed = trim_copy(content);
        if ((!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[')) &&
            (trimmed.find("\"tool_calls\"") != std::string::npos ||
             trimmed.find("\"name\"") != std::string::npos ||
             trimmed.find("\"function\"") != std::string::npos)) {
            try {
                append_tool_calls_from_json(json::parse(trimmed), allowed_tools, calls);
                if (!calls.empty()) {
                    visible_content.clear();
                }
            } catch (...) {
            }
        }
    }

    msg.content = trim_copy(visible_content);
    msg.tool_calls = std::move(calls);

    if (ends_with(msg.content, "<tool_call>") || ends_with(msg.content, "<tool_calls>")) {
        msg.content.clear();
    }

    return msg;
}

static size_t json_string_field_start(const std::string & input, const std::string & key) {
    size_t pos = input.find("\"" + key + "\"");
    if (pos == std::string::npos) {
        return 0;
    }
    pos += key.size() + 2;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
    if (pos == input.size() || input[pos++] != ':') {
        return 0;
    }
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
    return pos < input.size() && input[pos] == '"' ? pos + 1 : 0;
}

static bool decode_json_string_part(const std::string & input, size_t & pos, std::string & output) {
    while (pos < input.size()) {
        if (input[pos] == '"') {
            ++pos;
            return true;
        }
        if (input[pos] != '\\') {
            output += input[pos++];
            continue;
        }
        if (pos + 1 >= input.size()) {
            return false;
        }
        size_t length = input[pos + 1] == 'u' ? 6 : 2;
        if (pos + length > input.size()) {
            return false;
        }
        if (length == 6) {
            auto hex = input.substr(pos + 2, 4);
            unsigned cp = std::strtoul(hex.c_str(), nullptr, 16);
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                length = 12;
                if (pos + length > input.size()) {
                    return false;
                }
            }
        }
        try {
            output += json::parse("\"" + input.substr(pos, length) + "\"").get<std::string>();
        } catch (const json::exception &) {
            return false;
        }
        pos += length;
    }
    return false;
}

std::string extract_partial_json_string(const std::string & input, const std::string & key, bool & complete) {
    std::string result;
    size_t pos = json_string_field_start(input, key);
    complete = pos > 0 && decode_json_string_part(input, pos, result);
    return result;
}

bool decode_field_incremental(tool_stream_state & state, const std::string & key) {
    if (state.content_scan_pos == 0) {
        state.content_scan_pos = json_string_field_start(state.accumulated_args, key);
        state.content_raw_end = state.content_scan_pos;
    }
    return state.content_scan_pos > 0 && decode_json_string_part(state.accumulated_args, state.content_raw_end, state.content_buffer);
}
