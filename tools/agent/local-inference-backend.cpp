#include "local-inference-backend.h"

#include "common.h"
#include "chat.h"
#include "json.h"
#include "log.h"

// The server headers alias `json` to common_json and the agent headers alias it to nlohmann::ordered_json.
// Rename the token only while the server headers are parsed so both can share this translation unit.
#define json common_json
#include "server-common.h"
#include "server-context.h"
#include "server-schema.h"
#include "server-task.h"
#undef json

#include <algorithm>
#include <stdexcept>

static inference_timings to_inference_timings(const server_slot_stats & stats) {
    inference_timings out;
    if (!stats.is_set()) {
        return out;
    }
    out.cache_n = (int32_t) stats.n_prompt_cached;
    out.prompt_n = (int32_t) stats.n_prompt_processed;
    out.prompt_ms = stats.t_prompt_ms();
    out.prompt_per_token_ms = stats.t_prompt_per_token_ms();
    out.prompt_per_second = stats.n_prompt_tps();
    out.predicted_n = (int32_t) stats.n_gen;
    out.predicted_ms = stats.t_gen_ms();
    out.predicted_per_token_ms = stats.t_gen_per_token_ms();
    out.predicted_per_second = stats.n_gen_tps();
    out.draft_n = (int32_t) stats.n_draft_tokens;
    out.draft_n_accepted = (int32_t) stats.n_draft_accepted;
    return out;
}

static inference_prompt_progress to_inference_progress(const result_prompt_progress & progress) {
    inference_prompt_progress out;
    out.total = progress.total;
    out.cache = progress.cache;
    out.processed = progress.processed;
    out.time_ms = progress.time_ms;
    return out;
}

local_inference_backend::local_inference_backend(server_context & server_ctx, const common_params & params)
    : server_ctx_(server_ctx)
    , params_(params) {
}

const inference_backend_meta & local_inference_backend::meta() const {
    // Populate once (first call happens during inference, i.e. after model load) and
    // freeze; the values from server_ctx_ are stable after load. Immutable afterwards,
    // so the const-ref returned here is safe to read concurrently without a lock.
    std::call_once(meta_once_, [this]() {
        auto server_meta = server_ctx_.get_meta();
        meta_.model_name = server_meta.model_name;
        meta_.build_info = server_meta.build_info;
        meta_.n_ctx = server_meta.slot_n_ctx;
        meta_.has_vision = server_meta.has_inp_image;
        meta_.has_audio = server_meta.has_inp_audio;
        meta_.image_support_known = true;
        meta_.is_llama_server = false;
        meta_.total_slots = params_.n_parallel > 0 ? params_.n_parallel : 1;
    });
    return meta_;
}

const llama_vocab * local_inference_backend::vocab() {
    if (vocab_ != nullptr) {
        return vocab_;
    }

    auto * lctx = server_ctx_.get_llama_context();
    if (lctx == nullptr) {
        throw std::runtime_error("llama context is not available");
    }
    vocab_ = llama_model_get_vocab(llama_get_model(lctx));
    if (vocab_ == nullptr) {
        throw std::runtime_error("llama vocab is not available");
    }
    return vocab_;
}

inference_result local_inference_backend::complete(
    const inference_request & request,
    std::function<void(const inference_event &)> on_event,
    std::function<bool()> should_stop) {

    inference_result out;

    auto stop = should_stop ? std::move(should_stop) : std::function<bool()>([]() { return false; });
    auto emit = [&](const inference_event & event) {
        if (on_event) {
            on_event(event);
        }
    };

    server_response_reader rd = server_ctx_.get_response_reader();
    try {
        std::lock_guard<std::mutex> lock(post_mutex_);

        auto server_meta = server_ctx_.get_meta();

        common_json body = common_json::object();
        body["messages"] = common_json::parse(request.messages.dump());
        if (!request.tools.empty()) {
            body["tools"] = common_chat_tools_to_json_oaicompat(request.tools);
        }
        body["stream"] = request.stream;
        body["timings_per_token"] = request.timings_per_token;
        body["cache_prompt"] = request.cache_prompt;
        body["return_progress"] = request.return_progress;
        body["stream_options"] = common_json::object({{"include_usage", true}});
        if (request.n_predict >= 0) {
            body["n_predict"] = request.n_predict;
        }
        if (request.n_keep > 0) {
            body["n_keep"] = request.n_keep;
        }
        if (!request.parse_tool_calls) {
            body["tool_choice"] = "none";
        }

        std::vector<raw_buffer> files;
        common_json data = oaicompat_chat_params_parse(body, server_meta.chat_params, files);

        server_task task = server_task(SERVER_TASK_TYPE_COMPLETION);
        task.id = rd.get_new_id();
        task.index = 0;
        task.id_slot = request.id_slot;
        task.params = server_schema::eval_llama_cmpl_schema(
            vocab(), params_, server_meta.logit_bias_eog, data);
        task.params.chat_parser_params.parse_tool_calls = request.parse_tool_calls;
        task.params.cache_prompt = request.cache_prompt;
        task.params.return_progress = request.return_progress;
        task.params.timings_per_token = request.timings_per_token;

        task.cli = true;
        task.cli_prompt = data.at("prompt").get<std::string>();
        task.cli_files = std::move(files);

        rd.post_task(std::move(task));
    } catch (const std::exception & e) {
        out.error = e.what();
        emit({inference_event_type::ERROR, {}, {}, out.error, {}});
        return out;
    }

    std::string full_content;
    std::string full_reasoning;

    server_task_result_ptr result;
    try {
        result = rd.next(stop);
    } catch (const std::exception & e) {
        out.error = e.what();
        emit({inference_event_type::ERROR, {}, {}, out.error, {}});
        return out;
    }

    while (result) {
        if (stop()) {
            out.cancelled = true;
            break;
        }

        if (result->is_error()) {
            auto * err = dynamic_cast<server_task_result_error *>(result.get());
            if (err && err->err_type == ERROR_TYPE_EXCEED_CONTEXT_SIZE) {
                out.context_overflow = true;
                out.prompt_tokens = err->n_prompt_tokens;
                return out;
            }
            if (err) {
                out.error = err->err_msg;
            } else {
                common_json err_data = result->to_json();
                out.error = err_data.value("message", "Unknown inference error");
            }
            emit({inference_event_type::ERROR, {}, {}, out.error, {}});
            return out;
        }

        if (auto * partial = dynamic_cast<server_task_result_cmpl_partial *>(result.get())) {
            out.timings = to_inference_timings(partial->stats);
            out.prompt_tokens = partial->n_prompt_tokens;
            out.cached_prompt_tokens = std::max(out.cached_prompt_tokens, partial->n_prompt_tokens_cache);

            if (partial->is_progress) {
                inference_event event;
                event.type = inference_event_type::PROMPT_PROGRESS;
                event.progress = to_inference_progress(partial->progress);
                event.data = json::parse(partial->progress.to_json().dump());
                emit(event);
                out.cached_prompt_tokens = std::max(out.cached_prompt_tokens, partial->progress.cache);
            }

            for (const auto & diff : partial->oaicompat_msg_diffs) {
                if (!diff.content_delta.empty()) {
                    inference_event event;
                    event.type = inference_event_type::TEXT_DELTA;
                    event.diff = diff;
                    emit(event);
                    full_content += diff.content_delta;
                }
                if (!diff.reasoning_content_delta.empty()) {
                    inference_event event;
                    event.type = inference_event_type::REASONING_DELTA;
                    event.diff = diff;
                    emit(event);
                    full_reasoning += diff.reasoning_content_delta;
                }
                if (diff.tool_call_index != std::string::npos) {
                    inference_event event;
                    event.type = inference_event_type::TOOL_CALL_DELTA;
                    event.diff = diff;
                    emit(event);
                }
            }
        }

        if (auto * final_result = dynamic_cast<server_task_result_cmpl_final *>(result.get())) {
            out.timings = to_inference_timings(final_result->stats);
            out.prompt_tokens = final_result->n_prompt_tokens;
            out.cached_prompt_tokens = std::max(out.cached_prompt_tokens, final_result->n_prompt_tokens_cache);
            out.cached_prompt_tokens = std::max(out.cached_prompt_tokens, final_result->n_tokens_cached);

            if (!final_result->oaicompat_msg.empty()) {
                out.message = final_result->oaicompat_msg;
            } else if (!final_result->content.empty()) {
                out.message.role = "assistant";
                out.message.content = final_result->content;
            }
            break;
        }

        try {
            result = rd.next(stop);
        } catch (const std::exception & e) {
            LOG_WRN("Failed to parse model output: %s\n", e.what());
            out.error = e.what();
            break;
        }
    }

    if (out.message.empty()) {
        out.message.role = "assistant";
        out.message.content = full_content;
        out.message.reasoning_content = full_reasoning;
    }

    return out;
}
