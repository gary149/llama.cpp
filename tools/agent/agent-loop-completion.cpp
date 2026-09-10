#include "agent-loop.h"

#include <functional>

// Streaming version of generate_completion
inference_result agent_loop::generate_completion_streaming(
    agent_event_callback on_event,
    std::function<bool()> should_stop) {

    auto chat_tools = tool_registry::instance().to_chat_tools();
    inference_request request = build_inference_request(chat_tools);

    inference_result result = backend_.complete(
        request,
        [on_event](const inference_event & event) {
            switch (event.type) {
                case inference_event_type::TEXT_DELTA:
                    if (!event.diff.content_delta.empty()) {
                        on_event(agent_event::text_delta(event.diff.content_delta));
                    }
                    break;
                case inference_event_type::REASONING_DELTA:
                    if (!event.diff.reasoning_content_delta.empty()) {
                        on_event(agent_event::reasoning_delta(event.diff.reasoning_content_delta));
                    }
                    break;
                case inference_event_type::ERROR:
                    if (!event.error.empty()) {
                        on_event(agent_event::error(event.error));
                    }
                    break;
                case inference_event_type::TOOL_CALL_DELTA:
                    if (event.diff.tool_call_index != std::string::npos) {
                        on_event(agent_event::tool_call_delta(
                            event.diff.tool_call_index,
                            event.diff.tool_call_delta.name,
                            event.diff.tool_call_delta.arguments));
                    }
                    break;
                case inference_event_type::PROMPT_PROGRESS:
                    break;
            }
        },
        should_stop);

    last_prompt_tokens_ = result.prompt_tokens;
    last_completion_overflowed_ = result.context_overflow;
    return result;
}
