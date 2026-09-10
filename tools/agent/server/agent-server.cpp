#include "agent-routes.h"
#include "agent-session.h"

#include "../local-inference-backend.h"
#ifdef LLAMA_AGENT_HAS_HTTP_BACKEND
#include "../http-inference-backend.h"
#endif

#include "common.h"
#include "chat.h"
#include "json.h"
#include "log.h"

// The server headers alias `json` to common_json and the agent headers alias it to nlohmann::ordered_json.
// Rename the token only while the server headers are parsed so both can share this translation unit.
#define json common_json
#include "server-context.h"
#include "server-http.h"
#undef json

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"

// MCP support (Unix only)
#ifndef _WIN32
#include "../mcp/mcp-server-manager.h"
#include "../mcp/mcp-tool-wrapper.h"
#endif

#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

static std::function<void(int)> shutdown_handler;
static std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

static inline void signal_handler(int signal) {
    if (is_terminating.test_and_set()) {
        fprintf(stderr, "Received second interrupt, terminating immediately.\n");
        exit(1);
    }
    shutdown_handler(signal);
}

// Wrapper to handle exceptions in handlers
static server_http_context::handler_t ex_wrapper(server_http_context::handler_t func) {
    return [func = std::move(func)](const server_http_req & req) -> server_http_res_ptr {
        try {
            return func(req);
        } catch (const std::invalid_argument & e) {
            auto res = std::make_unique<server_http_res>();
            res->status = 400;
            res->data = json{{"error", e.what()}}.dump();
            return res;
        } catch (const std::exception & e) {
            auto res = std::make_unique<server_http_res>();
            res->status = 500;
            res->data = json{{"error", e.what()}}.dump();
            LOG_ERR("Handler exception: %s\n", e.what());
            return res;
        } catch (...) {
            auto res = std::make_unique<server_http_res>();
            res->status = 500;
            res->data = json{{"error", "Unknown error"}}.dump();
            return res;
        }
    };
}

int main(int argc, char ** argv) {
    common_params params;

    std::string backend_mode = "auto";
    std::string server_url;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--server-url") {
            if (i + 1 >= argc) {
                fprintf(stderr, "--server-url requires a URL\n");
                return 1;
            }
            server_url = argv[i + 1];
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (arg == "--backend") {
            if (i + 1 >= argc) {
                fprintf(stderr, "--backend requires a value\n");
                return 1;
            }
            backend_mode = argv[i + 1];
            if (backend_mode != "auto" && backend_mode != "local" && backend_mode != "http") {
                fprintf(stderr, "--backend must be one of: auto, local, http\n");
                return 1;
            }
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        }
    }

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }

    // Set defaults for agent server
    if (params.n_parallel < 0) {
        params.n_parallel = 4;
        params.kv_unified = true;
    }

    if (params.model_alias.empty() && !params.model.get_name().empty()) {
        params.model_alias.insert(params.model.get_name());
    }

    common_init();

    const bool use_http_backend = backend_mode != "local" && !server_url.empty();
    if (backend_mode == "http" && server_url.empty()) {
        LOG_ERR("--backend http requires --server-url\n");
        return 1;
    }

    std::unique_ptr<server_context> ctx_server;
    std::unique_ptr<local_inference_backend> local_backend;
#ifdef LLAMA_AGENT_HAS_HTTP_BACKEND
    std::unique_ptr<http_inference_backend> http_backend;
#endif
    inference_backend * inference = nullptr;

    if (!use_http_backend) {
        // Initialize server context (holds LLM inference)
        ctx_server = std::make_unique<server_context>();
    }

    llama_backend_init();
    llama_numa_init(params.numa);

    LOG_INF("llama-agent-server starting\n");
    LOG_INF("system info: n_threads = %d, n_threads_batch = %d, total_threads = %d\n",
            params.cpuparams.n_threads, params.cpuparams_batch.n_threads,
            std::thread::hardware_concurrency());
    LOG_INF("\n");
    LOG_INF("%s\n", common_params_get_system_info(params).c_str());
    LOG_INF("\n");

    // Initialize HTTP context
    server_http_context ctx_http;
    if (!ctx_http.init(params)) {
        LOG_ERR("Failed to initialize HTTP server\n");
        return 1;
    }

    if (use_http_backend) {
#ifndef LLAMA_AGENT_HAS_HTTP_BACKEND
        LOG_ERR("HTTP backend is not available in this build\n");
        return 1;
#else
        http_inference_backend_config http_cfg;
        http_cfg.base_url = server_url;
        if (!params.model_alias.empty()) {
            http_cfg.model = *params.model_alias.begin();
        } else if (!params.model.get_name().empty()) {
            http_cfg.model = params.model.get_name();
        }
        http_backend = std::make_unique<http_inference_backend>(std::move(http_cfg));
        inference = http_backend.get();
#endif
    } else {
        local_backend = std::make_unique<local_inference_backend>(*ctx_server, params);
        inference = local_backend.get();
    }

    // Create session manager (manages agent sessions)
    agent_session_manager session_mgr(*inference);

    // Create and register routes
    agent_routes routes(session_mgr);

    ctx_http.get("/health", ex_wrapper(routes.get_health));
    ctx_http.get("/v1/agent/health", ex_wrapper(routes.get_health));

    ctx_http.post("/v1/agent/session", ex_wrapper(routes.post_session));
    ctx_http.get("/v1/agent/session/:id", ex_wrapper(routes.get_session));
    ctx_http.post("/v1/agent/session/:id/delete", ex_wrapper(routes.delete_session));
    ctx_http.get("/v1/agent/sessions", ex_wrapper(routes.get_sessions));

    ctx_http.post("/v1/agent/session/:id/chat", ex_wrapper(routes.post_chat));
    ctx_http.get("/v1/agent/session/:id/messages", ex_wrapper(routes.get_messages));

    ctx_http.get("/v1/agent/session/:id/permissions", ex_wrapper(routes.get_permissions));
    ctx_http.post("/v1/agent/permission/:id", ex_wrapper(routes.post_permission));

    ctx_http.get("/v1/agent/tools", ex_wrapper(routes.get_tools));
    ctx_http.get("/v1/agent/session/:id/stats", ex_wrapper(routes.get_stats));

    // Setup cleanup
    auto clean_up = [&ctx_http, &ctx_server]() {
        LOG_INF("Cleaning up before exit...\n");
        ctx_http.stop();
        if (ctx_server) {
            ctx_server->terminate();
        }
        llama_backend_free();
    };

    // Start HTTP server before loading model (to serve /health)
    if (!ctx_http.start()) {
        clean_up();
        LOG_ERR("Failed to start HTTP server\n");
        return 1;
    }

    if (ctx_server) {
        // Load the model
        LOG_INF("Loading model...\n");

        if (!ctx_server->load_model(params)) {
            clean_up();
            if (ctx_http.thread.joinable()) {
                ctx_http.thread.join();
            }
            LOG_ERR("Failed to load model\n");
            return 1;
        }

        LOG_INF("Model loaded successfully\n");
    } else {
        LOG_INF("Using HTTP inference backend: %s\n", server_url.c_str());
    }

    ctx_http.is_ready.store(true);

    // Initialize MCP servers (Unix only)
    // MCP manager must be declared here to outlive session manager (tools hold pointer to it)
#ifndef _WIN32
    mcp_server_manager mcp_mgr;
    int mcp_tools_count = 0;
    std::string working_dir = ".";  // Default working directory for MCP config search
    std::string mcp_config = find_mcp_config(working_dir);
    if (!mcp_config.empty()) {
        LOG_INF("Loading MCP config from: %s\n", mcp_config.c_str());
        if (mcp_mgr.load_config(mcp_config)) {
            int started = mcp_mgr.start_servers();
            if (started > 0) {
                register_mcp_tools(mcp_mgr);
                mcp_tools_count = static_cast<int>(mcp_mgr.list_all_tools().size());
                LOG_INF("MCP: %d servers started, %d tools registered\n", started, mcp_tools_count);
            }
        }
    }
#else
    int mcp_tools_count = 0;
#endif

    // Setup signal handlers
    shutdown_handler = [&ctx_http, &ctx_server](int) {
        ctx_http.stop();
        if (ctx_server) {
            ctx_server->terminate();
        }
    };

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset(&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);
#elif defined(_WIN32)
    auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
        return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
    };
    SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

    LOG_INF("\n");
    LOG_INF("============================================\n");
    LOG_INF("llama-agent-server is listening on %s\n", ctx_http.listening_address.c_str());
    LOG_INF("============================================\n");
    LOG_INF("\n");
    if (mcp_tools_count > 0) {
        LOG_INF("MCP tools: %d\n", mcp_tools_count);
    }
    LOG_INF("API Endpoints:\n");
    LOG_INF("  POST /v1/agent/session           - Create a new session\n");
    LOG_INF("  GET  /v1/agent/session/:id       - Get session info\n");
    LOG_INF("  POST /v1/agent/session/:id/chat  - Send message (streaming SSE)\n");
    LOG_INF("  GET  /v1/agent/session/:id/messages - Get conversation history\n");
    LOG_INF("  GET  /v1/agent/tools             - List available tools\n");
    LOG_INF("  GET  /health                     - Health check\n");
    LOG_INF("\n");

    // Start the main inference loop, or keep the HTTP API alive when inference is remote.
    if (ctx_server) {
        ctx_server->start_loop();
    } else if (ctx_http.thread.joinable()) {
        ctx_http.thread.join();
    }

    // Clean up after shutdown
    clean_up();

    if (ctx_http.thread.joinable()) {
        ctx_http.thread.join();
    }

    LOG_INF("llama-agent-server stopped\n");
    return 0;
}
