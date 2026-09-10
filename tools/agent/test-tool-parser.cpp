#include "agent-tool-parser.h"

#include <cassert>

int main() {
    std::vector<common_chat_tool> tools = {
        {
            "read",
            "Read a file",
            R"({"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]})",
        },
    };

    json messages = json::array({
        {{"role", "system"}, {"content", "base"}},
        {{"role", "user"}, {"content", "inspect"}},
    });
    json injected = agent_inject_tool_protocol_prompt(messages, tools);
    assert(injected[0]["content"].get<std::string>().find("<tool_call>") != std::string::npos);
    assert(injected[0]["content"].get<std::string>().find("read") != std::string::npos);

    common_chat_msg parsed = agent_parse_tool_protocol_response(
        "I will inspect it.\n<tool_call>{\"name\":\"read\",\"arguments\":{\"file_path\":\"foo.txt\"}}</tool_call>",
        "thinking",
        tools);

    assert(parsed.role == "assistant");
    assert(parsed.reasoning_content == "thinking");
    assert(parsed.content == "I will inspect it.");
    assert(parsed.tool_calls.size() == 1);
    assert(parsed.tool_calls[0].name == "read");
    assert(parsed.tool_calls[0].arguments == R"({"file_path":"foo.txt"})");

    common_chat_msg parsed_array = agent_parse_tool_protocol_response(
        R"(<tool_calls>[{"name":"read","arguments":{"file_path":"bar.txt"}}]</tool_calls>)",
        "",
        tools);
    assert(parsed_array.content.empty());
    assert(parsed_array.tool_calls.size() == 1);
    assert(parsed_array.tool_calls[0].arguments == R"({"file_path":"bar.txt"})");

    common_chat_msg parsed_unterminated = agent_parse_tool_protocol_response(
        R"(<tool_calls>[{"name":"read","arguments":{"file_path":"a.txt"}},{"name":"read","arguments":{"file_path":"b.txt"}}])",
        "",
        tools);
    assert(parsed_unterminated.content.empty());
    assert(parsed_unterminated.tool_calls.size() == 2);
    assert(parsed_unterminated.tool_calls[1].arguments == R"({"file_path":"b.txt"})");

    return 0;
}
