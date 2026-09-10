#include "common.h"
#include "arg.h"
#include "fit.h"
#include "base64.hpp"
#include "console.h"

#include "agent-loop.h"
#include "agent-resources.h"
#include "clipboard-image.h"
#include "config-dir.h"
#include "local-inference-backend.h"
#ifdef LLAMA_AGENT_HAS_HTTP_BACKEND
#include "http-inference-backend.h"
#include "http.h"
#endif
#include "terminal-image.h"
#include "tui-renderer.h"
#include "tool-registry.h"
#include "permission.h"
#include "log.h"
#include "server-context.h"

#ifndef _WIN32
#include "mcp/mcp-server-manager.h"
#include "mcp/mcp-tool-wrapper.h"
#endif

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <signal.h>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#undef ERROR
#include <io.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace fs = std::filesystem;

#ifdef LLAMA_AGENT_HAS_HTTP_BACKEND
struct spawned_llama_server {
    int pid = -1;
    std::string url;

    void stop() {
#if !defined(_WIN32)
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
            pid = -1;
        }
#endif
    }

    ~spawned_llama_server() {
        stop();
    }
};

#if !defined(_WIN32)
static int find_free_loopback_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        close(fd);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

static fs::path find_sibling_llama_server(char ** argv) {
    fs::path exe_path;
#if defined(__linux__)
    try {
        exe_path = fs::read_symlink("/proc/self/exe");
    } catch (...) {
    }
#endif
    if (exe_path.empty() && argv && argv[0]) {
        exe_path = fs::absolute(argv[0]);
    }
    if (exe_path.empty()) {
        return {};
    }

    fs::path candidate = exe_path.parent_path() / "llama-server";
    if (fs::exists(candidate)) {
        return candidate;
    }
    return {};
}

// Polls the spawned server's /props until ready, but bails out immediately if the
// child process exits first (e.g. a forwarded flag it rejected, or OOM) instead of
// blocking for the full timeout. On early child exit the pid is cleared so the
// caller's stop() cannot signal a recycled pid.
static bool wait_for_spawned_server_ready(spawned_llama_server & spawned, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (spawned.pid > 0) {
            int status = 0;
            if (waitpid(spawned.pid, &status, WNOHANG) == spawned.pid) {
                spawned.pid = -1; // reaped; don't let stop() kill a recycled pid
                return false;
            }
        }
        try {
            auto [cli, parts] = common_http_client(spawned.url);
            cli.set_connection_timeout(std::chrono::milliseconds(200));
            cli.set_read_timeout(std::chrono::milliseconds(200));
            auto res = cli.Get("/props");
            if (res && res->status >= 200 && res->status < 300) {
                return true;
            }
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

static bool try_auto_spawn_llama_server(
    int argc,
    char ** argv,
    const common_params & params,
    spawned_llama_server & spawned,
    std::string & reason) {

    fs::path server_path = find_sibling_llama_server(argv);
    if (server_path.empty()) {
        reason = "llama-server executable not found next to llama-agent";
        return false;
    }

    int port = find_free_loopback_port();
    if (port <= 0) {
        reason = "could not allocate a loopback port";
        return false;
    }

    std::vector<std::string> args;
    args.push_back(server_path.string());
    args.push_back("--host");
    args.push_back("127.0.0.1");
    args.push_back("--port");
    args.push_back(std::to_string(port));
    args.push_back("--parallel");
    args.push_back("1");
    args.push_back("--cache-prompt");
    args.push_back("--slots");
    args.push_back("--no-ui");

    if (!params.model.path.empty()) {
        args.push_back("-m");
        args.push_back(params.model.path);
    } else if (!params.model.hf_repo.empty()) {
        args.push_back("-hf");
        args.push_back(params.model.hf_repo);
        if (!params.model.hf_file.empty()) {
            args.push_back("-hff");
            args.push_back(params.model.hf_file);
        }
    } else if (!params.model.url.empty()) {
        args.push_back("-mu");
        args.push_back(params.model.url);
    } else {
        reason = "no model path, URL, or Hugging Face repo was provided";
        return false;
    }

    // Forward every other option the user explicitly passed that llama-server also
    // accepts. This is driven off the shared argument registry rather than a
    // hand-maintained allow-list, so flags added to llama-server in the future are
    // forwarded automatically. We re-parse the (already agent-flag-stripped) argv to
    // learn which options were provided, then mirror the parser's own example filter
    // (arg.cpp: an option applies to an example when in_example(ex) || in_example(COMMON)).
    static const std::set<std::string> managed_flags = {
        // server-management flags we set ourselves above
        "--host", "--port", "-np", "--parallel",
        "--cache-prompt", "--no-cache-prompt",
        "--slots", "--no-slots",
        "--ui", "--no-ui", "--webui", "--no-webui",
        // model selection handled explicitly above (forward the resolved path)
        "-m", "--model", "-hf", "-hfr", "--hf-repo", "-hff", "--hf-file", "-mu", "--model-url",
    };

    std::map<common_arg, std::string> user_args;
    try {
        common_params_to_map(argc, argv, LLAMA_EXAMPLE_CLI, user_args);
    } catch (const std::exception & e) {
        // A two-value or otherwise un-mappable argument means we can't reliably
        // determine what to forward; fall back to the local backend so the user's
        // options are honored in full rather than silently dropped.
        reason = std::string("could not introspect arguments for forwarding (") + e.what() + ")";
        return false;
    }

    for (const auto & entry : user_args) {
        common_arg opt = entry.first; // copy: in_example/is_exclude are non-const
        const std::string & value = entry.second;
        if (opt.args.empty()) {
            continue;
        }
        const bool server_accepts =
            (opt.in_example(LLAMA_EXAMPLE_SERVER) || opt.in_example(LLAMA_EXAMPLE_COMMON)) &&
            !opt.is_exclude(LLAMA_EXAMPLE_SERVER);
        if (!server_accepts) {
            continue;
        }
        bool managed = false;
        for (const char * a : opt.args) {
            if (managed_flags.count(a)) { managed = true; break; }
        }
        for (const char * a : opt.args_neg) {
            if (managed_flags.count(a)) { managed = true; break; }
        }
        if (managed) {
            continue;
        }
        if (opt.value_hint == nullptr && opt.value_hint_2 == nullptr) {
            // boolean flag: common_params_to_map records "1" for the positive form
            // and "0" for the negated form.
            if (value == "1") {
                args.push_back(opt.args[0]);
            } else if (!opt.args_neg.empty()) {
                args.push_back(opt.args_neg[0]);
            }
        } else {
            args.push_back(opt.args[0]);
            args.push_back(value);
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        reason = "fork failed";
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<char *> exec_args;
        exec_args.reserve(args.size() + 1);
        for (auto & arg : args) {
            exec_args.push_back(const_cast<char *>(arg.c_str()));
        }
        exec_args.push_back(nullptr);
        execvp(exec_args[0], exec_args.data());
        _exit(127);
    }

    spawned.pid = pid;
    spawned.url = "http://127.0.0.1:" + std::to_string(port);

    if (!wait_for_spawned_server_ready(spawned, 120000)) {
        spawned.stop();
        reason = "spawned llama-server did not become ready";
        return false;
    }

    return true;
}
#endif
#else
struct spawned_llama_server {
    std::string url;
    void stop() {}
};
#endif

// Result from running a user shell command (! prefix)
struct user_command_result {
    std::string output;
    int exit_code;
};

static user_command_result run_user_command(const std::string & command,
                                            const std::string & working_dir,
                                            std::atomic<bool> & is_interrupted,
                                            std::function<void(std::string_view)> on_output = nullptr) {
    user_command_result result;
    result.exit_code = 0;

    static const size_t MAX_CONTEXT_LENGTH = 100000;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        result.output = "[Failed to create pipe]\n";
        result.exit_code = 1;
        return result;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(STARTUPINFOA)};
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    std::string cmd_line = "cmd /c " + command;

    if (!CreateProcessA(NULL, (LPSTR)cmd_line.c_str(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, working_dir.c_str(), &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        result.output = "[Failed to create process]\n";
        result.exit_code = 1;
        return result;
    }

    CloseHandle(hWritePipe);

    char buffer[4096];
    DWORD bytesRead;

    while (true) {
        if (is_interrupted.load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        DWORD available = 0;
        PeekNamedPipe(hReadPipe, NULL, 0, NULL, &available, NULL);
        if (available == 0) {
            DWORD wait_result = WaitForSingleObject(pi.hProcess, 100);
            if (wait_result == WAIT_OBJECT_0) break;
            continue;
        }

        if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            if (on_output) {
                on_output(std::string_view(buffer, bytesRead));
            } else {
                fwrite(buffer, 1, bytesRead, stdout);
                fflush(stdout);
            }
            result.output.append(buffer, bytesRead);
            if (result.output.size() > MAX_CONTEXT_LENGTH * 2) {
                result.output.erase(0, result.output.size() - MAX_CONTEXT_LENGTH);
            }
        }
    }

    DWORD exitCodeDword;
    GetExitCodeProcess(pi.hProcess, &exitCodeDword);
    result.exit_code = (int)exitCodeDword;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

#else
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
        result.output = "[Failed to create pipe]\n";
        result.exit_code = 1;
        return result;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        result.output = "[Failed to fork process]\n";
        result.exit_code = 1;
        return result;
    }

    if (pid == 0) {
        // Child process
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);

        if (chdir(working_dir.c_str()) != 0) {
            _exit(127);
        }

        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(pipe_fd[1]);

    // Set non-blocking read
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    char buffer[4096];
    bool child_reaped = false;

    while (true) {
        if (is_interrupted.load()) {
            kill(pid, SIGKILL);
            break;
        }

        ssize_t n = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            if (on_output) {
                on_output(std::string_view(buffer, n));
            } else {
                fwrite(buffer, 1, n, stdout);
                fflush(stdout);
            }
            result.output.append(buffer, n);
            if (result.output.size() > MAX_CONTEXT_LENGTH * 2) {
                result.output.erase(0, result.output.size() - MAX_CONTEXT_LENGTH);
            }
        } else if (n == 0) {
            // EOF
            break;
        } else {
            // EAGAIN - no data available
            int status;
            pid_t wp = waitpid(pid, &status, WNOHANG);
            if (wp == pid) {
                // Process ended, read remaining data
                while ((n = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
                    buffer[n] = '\0';
                    if (on_output) {
                        on_output(std::string_view(buffer, n));
                    } else {
                        fwrite(buffer, 1, n, stdout);
                        fflush(stdout);
                    }
                    result.output.append(buffer, n);
                    if (result.output.size() > MAX_CONTEXT_LENGTH * 2) {
                        result.output.erase(0, result.output.size() - MAX_CONTEXT_LENGTH);
                    }
                }
                result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                child_reaped = true;
                break;
            }
            usleep(10000);  // 10ms
        }
    }

    close(pipe_fd[0]);

    // Wait for child if not already done
    if (!child_reaped) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        }
    }
#endif

    // Truncate to max context length (keep tail)
    if (result.output.size() > MAX_CONTEXT_LENGTH) {
        result.output = result.output.substr(result.output.size() - MAX_CONTEXT_LENGTH);
        size_t nl = result.output.find('\n');
        if (nl != std::string::npos && nl < 200) {
            result.output = result.output.substr(nl + 1);
        }
        result.output = "[output truncated]\n" + result.output;
    }

    return result;
}

const char * LLAMA_AGENT_LOGO = R"(
    ____                                                   __
   / / /___ _____ ___  ____ _      ____ _____ ____  ____  / /_
  / / / __ `/ __ `__ \/ __ `/_____/ __ `/ __ `/ _ \/ __ \/ __/
 / / / /_/ / / / / / / /_/ /_____/ /_/ / /_/ /  __/ / / / /_
/_/_/\__,_/_/ /_/ /_/\__,_/      \__,_/\__, /\___/_/ /_/\__/
                                      /____/
)";

static std::atomic<bool> g_is_interrupted = false;
static std::atomic<bool> g_terminate_requested = false;

// Points at main()'s automatic-storage spawned_llama_server (if any) so the
// double-Ctrl-C path can reap the child before std::exit() skips destructors.
static spawned_llama_server * g_spawned_server = nullptr;

static bool should_stop() {
    return g_is_interrupted.load();
}

static bool is_stdin_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(fileno(stdin));
#endif
}

static std::string read_stdin_prompt() {
    std::string result;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!result.empty()) {
            result += "\n";
        }
        result += line;
    }
    return result;
}

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
static void signal_handler(int sig) {
    if (g_is_interrupted.load()) {
        // std::exit() skips automatic-storage destructors, so reap the spawned
        // llama-server here. kill()/waitpid() in stop() are async-signal-safe.
        if (g_spawned_server) {
            g_spawned_server->stop();
        }
        fprintf(stdout, "\033[0m\n");
        fflush(stdout);
        std::exit(130);
    }
    g_is_interrupted.store(true);
    if (sig == SIGTERM) {
        g_terminate_requested.store(true);
    }
}
#endif

int main(int argc, char ** argv) {
    common_params params;

    params.verbosity = LOG_LEVEL_ERROR;

    // Check for custom flags before common_params_parse
    bool yolo_mode = false;
    int max_iterations = 0;  // 0 = unlimited (default)
    bool enable_mcp = true;
    bool enable_skills = true;
    bool enable_agents_md = true;
    bool enable_compaction = true;
    bool enable_session = true;
    bool resume_session = false;
    std::string session_path;  // explicit path, or auto-generated
    std::string backend_mode = "auto";
    std::string server_url;
    std::vector<std::string> extra_skills_paths;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--yolo") {
            yolo_mode = true;
            // Remove from argv
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;  // Re-check this position
        } else if (arg == "--no-mcp") {
            enable_mcp = false;
            // Remove from argv
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (arg == "--no-skills") {
            enable_skills = false;
            // Remove from argv
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;  // Re-check this position
        } else if (arg == "--no-agents-md") {
            enable_agents_md = false;
            // Remove from argv
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (arg == "--no-compaction") {
            enable_compaction = false;
            // Remove from argv
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;  // Re-check this position
        } else if (arg == "--session") {
            if (i + 1 < argc) {
                session_path = argv[i + 1];
                for (int j = i; j < argc - 2; j++) {
                    argv[j] = argv[j + 2];
                }
                argc -= 2;
                i--;
            } else {
                fprintf(stderr, "--session requires a file path\n");
                return 1;
            }
        } else if (arg == "--server-url") {
            if (i + 1 < argc) {
                server_url = argv[i + 1];
                for (int j = i; j < argc - 2; j++) {
                    argv[j] = argv[j + 2];
                }
                argc -= 2;
                i--;
            } else {
                fprintf(stderr, "--server-url requires a URL\n");
                return 1;
            }
        } else if (arg == "--backend") {
            if (i + 1 < argc) {
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
            } else {
                fprintf(stderr, "--backend requires a value\n");
                return 1;
            }
        } else if (arg == "--resume") {
            resume_session = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (arg == "--no-session") {
            enable_session = false;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (arg == "--skills-path") {
            if (i + 1 < argc) {
                extra_skills_paths.push_back(argv[i + 1]);
                // Remove both the flag and its value
                for (int j = i; j < argc - 2; j++) {
                    argv[j] = argv[j + 2];
                }
                argc -= 2;
                i--;  // Re-check this position
            } else {
                fprintf(stderr, "--skills-path requires a value\n");
                return 1;
            }
        } else if (arg == "--max-iterations" || arg == "-mi") {
            if (i + 1 < argc) {
                try {
                    max_iterations = std::stoi(argv[i + 1]);
                    if (max_iterations < 0) max_iterations = 0;  // 0 = unlimited
                } catch (...) {
                    fprintf(stderr, "Invalid --max-iterations value: %s\n", argv[i + 1]);
                    return 1;
                }
                // Remove both the flag and its value
                for (int j = i; j < argc - 2; j++) {
                    argv[j] = argv[j + 2];
                }
                argc -= 2;
                i--;  // Re-check this position
            } else {
                fprintf(stderr, "--max-iterations requires a value\n");
                return 1;
            }
        }
    }

    if (backend_mode != "local") {
        params.server_base = server_url;
    }
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_CLI)) {
        return 1;
    }

    if (params.conversation_mode == COMMON_CONVERSATION_MODE_DISABLED) {
        console::error("--no-conversation is not supported by llama-agent\n");
        return 1;
    }

    common_init();

    // Apply verbosity setting immediately after init to suppress verbose logs
    common_log_set_verbosity_thold(params.verbosity);

    // NOTE: llama_backend_init() / llama_numa_init() are deferred to the local-backend
    // branch below. They dlopen every GGML backend and initialize the GPU runtime
    // (CUDA/Metal/...), and the auto-spawn path fork()s after this point — fork() after
    // GPU-runtime init is unsupported. Initializing here would also be wasted work when
    // the HTTP/auto-spawn backend is used (the spawned llama-server does its own init).

    if (!is_stdin_terminal()) {
        params.simple_io = true;
    }

    console::init(params.simple_io, params.use_color);
    atexit([]() { console::cleanup(); });

    // Register clipboard image paste handler for Ctrl+V
    console::set_paste_image_callback([](std::vector<uint8_t> & bytes, std::string & mime) -> bool {
        auto img = clipboard_read_image();
        if (!img) return false;
        bytes = std::move(img->bytes);
        mime  = std::move(img->mime_type);
        return true;
    });

    console::set_display(DISPLAY_TYPE_RESET);

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset (&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);
#elif defined (_WIN32)
    auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
        return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
    };
    SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

    spawned_llama_server spawned_server;
    g_spawned_server = &spawned_server;
    bool use_http_backend = backend_mode != "local" && !server_url.empty();
    if (backend_mode == "http" && server_url.empty()) {
        console::error("--backend http requires --server-url\n");
        return 1;
    }
    if (backend_mode == "auto" && server_url.empty()) {
#if defined(LLAMA_AGENT_HAS_HTTP_BACKEND) && !defined(_WIN32)
        std::string fallback_reason;
        if (try_auto_spawn_llama_server(argc, argv, params, spawned_server, fallback_reason)) {
            server_url = spawned_server.url;
            use_http_backend = true;
            console::log("Using auto-spawned llama-server at %s\n", server_url.c_str());
        } else if (params.verbosity >= LOG_LEVEL_INFO) {
            console::log("HTTP auto-spawn unavailable: %s; using local backend\n", fallback_reason.c_str());
        }
#endif
    }

    std::unique_ptr<server_context> ctx_server;
    std::unique_ptr<local_inference_backend> local_backend;
#ifdef LLAMA_AGENT_HAS_HTTP_BACKEND
    std::unique_ptr<http_inference_backend> http_backend;
#endif
    inference_backend * inference = nullptr;
    std::thread inference_thread;

    if (use_http_backend) {
#ifndef LLAMA_AGENT_HAS_HTTP_BACKEND
        console::error("HTTP backend is not available in this build\n");
        return 1;
#else
        http_inference_backend_config http_cfg;
        http_cfg.base_url = server_url;
        if (!params.model_alias.empty()) {
            http_cfg.model = *params.model_alias.begin();
        }
        http_backend = std::make_unique<http_inference_backend>(std::move(http_cfg));
        inference = http_backend.get();
#endif
    } else {
        // Local backend: initialize the GGML backends and NUMA now (deferred from
        // startup so the auto-spawn fork() above never runs after GPU-runtime init).
        llama_backend_init();
        llama_numa_init(params.numa);

        ctx_server = std::make_unique<server_context>();

        console::log("\nLoading model... ");
        console::spinner::start();
        if (!ctx_server->load_model(params)) {
            console::spinner::stop();
            console::error("\nFailed to load the model\n");
            return 1;
        }

        console::spinner::stop();
        console::log("\n");

        // Start inference thread
        inference_thread = std::thread([&ctx_server]() {
            ctx_server->start_loop();
        });

        local_backend = std::make_unique<local_inference_backend>(*ctx_server, params);
        inference = local_backend.get();
    }

    inference_backend_meta inf = inference->meta();

    // Get working directory
    std::string working_dir = fs::current_path().string();

#ifndef _WIN32
    // Load MCP servers (Unix only - requires fork/pipe)
    mcp_server_manager mcp_mgr;
    int mcp_tools_count = 0;
    if (enable_mcp) {
        std::string mcp_config = find_mcp_config(working_dir);
        if (!mcp_config.empty()) {
            if (mcp_mgr.load_config(mcp_config)) {
                int started = mcp_mgr.start_servers();
                if (started > 0) {
                    register_mcp_tools(mcp_mgr);
                    mcp_tools_count = (int)mcp_mgr.list_all_tools().size();
                }
            }
        }
    }
#else
    int mcp_tools_count = 0;
#endif

    agent_resource_config resource_cfg;
    resource_cfg.working_dir = working_dir;
    resource_cfg.config_dir = get_config_dir();
    resource_cfg.enable_skills = enable_skills;
    resource_cfg.enable_agents_md = enable_agents_md;
    resource_cfg.extra_skills_paths = extra_skills_paths;
    agent_resource_discovery resources = agent_discover_resources(resource_cfg);

    if (enable_agents_md && resources.agents_md_total_content_size > 50 * 1024) {
        console::log("Warning: AGENTS.md content is large (%zu bytes). "
                    "Consider reducing size for better performance.\n",
                    resources.agents_md_total_content_size);
    }

    // Configure agent
    agent_config config;
    config.working_dir = working_dir;
    config.max_iterations = max_iterations;
    config.tool_timeout_ms = 120000;
    config.verbose = (params.verbosity >= LOG_LEVEL_INFO);
    config.yolo_mode = yolo_mode;
    config.enable_skills = enable_skills;
    config.skills_search_paths = extra_skills_paths;
    config.skills_prompt_section = resources.skills_prompt_section();
    config.enable_agents_md = enable_agents_md;
    config.agents_md_prompt_section = resources.agents_md_prompt_section();
    config.compaction.enabled = enable_compaction;
    if (use_http_backend && inf.is_llama_server && inf.total_slots == 1) {
        config.inference_id_slot = 0;
    }

    // Session persistence
    session_file sf;
    session_file * sf_ptr = nullptr;
    loaded_session loaded;
    const loaded_session * resume_ptr = nullptr;

    if (enable_session && session_path.empty()) {
        // Auto-generate session path based on config dir + working directory
        std::string config_dir = get_config_dir();
        if (!config_dir.empty()) {
            std::string session_dir = session_file::get_session_dir(config_dir, working_dir);
            if (resume_session) {
                session_path = session_file::find_latest_session(session_dir);
                if (session_path.empty()) {
                    console::log("No previous session found, starting new.\n");
                    session_path = session_file::new_session_path(session_dir);
                }
            } else {
                session_path = session_file::new_session_path(session_dir);
            }
        }
    }

    if (!session_path.empty()) {
        if (resume_session || std::filesystem::exists(session_path)) {
            auto maybe = session_file::load(session_path);
            if (maybe) {
                loaded = std::move(*maybe);
                resume_ptr = &loaded;
            }
        }
        if (sf.open(session_path)) {
            sf_ptr = &sf;
            if (resume_ptr) {
                sf.set_message_count(resume_ptr->total_messages_in_file);
            }
        }
    }

    // Create agent loop
    agent_loop agent(*inference, config, g_is_interrupted, sf_ptr, resume_ptr);

    // Display startup info
    console::log("\n");
    console::log("%s\n", LLAMA_AGENT_LOGO);
    console::log("build      : %s\n", inf.build_info.c_str());
    console::log("model      : %s\n", inf.model_name.c_str());
    console::log("backend    : %s\n", use_http_backend ? "http" : "local");
    if (use_http_backend) {
        console::log("server url : %s\n", server_url.c_str());
    }
    console::log("working dir: %s\n", working_dir.c_str());
    if (yolo_mode) {
        console::set_display(DISPLAY_TYPE_ERROR);
        console::log("mode       : YOLO (all permissions auto-approved)\n");
        console::set_display(DISPLAY_TYPE_RESET);
    }
    if (mcp_tools_count > 0) {
        console::log("mcp tools  : %d\n", mcp_tools_count);
    }
    if (resources.skills_count > 0) {
        console::log("skills     : %d\n", resources.skills_count);
    }
    if (resources.agents_md_count > 0) {
        console::log("agents.md  : %d file(s)\n", resources.agents_md_count);
    }
    if (!session_path.empty()) {
        console::log("session    : %s%s\n", session_path.c_str(),
                      resume_ptr ? " (resumed)" : " (new)");
    }
    console::log("\n");

    // Display resumed conversation history
    // Helper: extract text from content that may be a string or array of content blocks.
    auto extract_text = [](const json & msg) -> std::string {
        if (!msg.contains("content")) {
            return "";
        }
        const auto & c = msg["content"];
        if (c.is_string()) {
            return c.get<std::string>();
        }
        if (c.is_array()) {
            std::string text;
            for (const auto & block : c) {
                if (block.contains("type") && block["type"] == "image_url") {
                    text += "[image]\n";
                } else if (block.contains("text") && block["text"].is_string()) {
                    text += block["text"].get<std::string>();
                }
            }
            return text;
        }
        return "";
    };

    // Resolve initial prompt from -p/--prompt flag or stdin
    std::string initial_prompt;
    if (!params.prompt.empty()) {
        initial_prompt = params.prompt;
        params.prompt.clear();  // Only use once
    } else if (!is_stdin_terminal()) {
        initial_prompt = read_stdin_prompt();
        // Trim trailing whitespace
        while (!initial_prompt.empty() && (initial_prompt.back() == '\n' || initial_prompt.back() == '\r')) {
            initial_prompt.pop_back();
        }
        // When reading from stdin pipe, always use single-turn mode
        // (stdin is at EOF, so interactive input would spin forever)
        params.single_turn = true;
    }

    // Non-interactive mode: if we have a prompt and single_turn, skip the help text
    if (initial_prompt.empty() || !params.single_turn) {
        console::log("commands:\n");
        console::log("  /exit       exit the agent\n");
        console::log("  /clear      clear conversation history\n");
        console::log("  /stats      show token usage statistics\n");
        console::log("  /tools      list available tools\n");
        console::log("  /skills     list available skills\n");
        console::log("  /agents     list discovered AGENTS.md files\n");
        console::log("  /compact    manually compact conversation context\n");
        console::log("  !<cmd>      run a shell command (output shared with LLM)\n");
        console::log("  !!<cmd>     run a shell command (output hidden from LLM)\n");
        console::log("  Ctrl+V      paste image from clipboard\n");
        console::log("  ESC/Ctrl+C  abort generation\n");
        console::log("\n");
    }

    const bool use_tui = is_stdin_terminal() && !params.simple_io && !params.single_turn;
    permission_manager_async tui_permissions;
    std::unique_ptr<tui_renderer> tui;

    auto stats_text = [](const session_stats & stats) {
        std::ostringstream ss;
        ss << "\nSession Statistics:\n";
        ss << "  Prompt tokens:  " << stats.total_input << "\n";
        ss << "  Output tokens:  " << stats.total_output << "\n";
        if (stats.total_cached > 0) {
            ss << "  Cached tokens:  " << stats.total_cached << "\n";
        }
        ss << "  Total tokens:   " << (stats.total_input + stats.total_output) << "\n";
        if (stats.total_prompt_ms > 0) {
            ss.setf(std::ios::fixed);
            ss.precision(2);
            ss << "  Prompt time:    " << (stats.total_prompt_ms / 1000.0) << "s\n";
        }
        if (stats.total_predicted_ms > 0) {
            ss.setf(std::ios::fixed);
            ss.precision(2);
            ss << "  Gen time:       " << (stats.total_predicted_ms / 1000.0) << "s\n";
            ss.precision(1);
            double avg_speed = stats.total_output * 1000.0 / stats.total_predicted_ms;
            ss << "  Avg speed:      " << avg_speed << " tok/s\n";
        }
        return ss.str();
    };

    if (use_tui) {
        tui_permissions.set_project_root(working_dir);
        tui_permissions.set_yolo_mode(yolo_mode);

        tui_renderer::config tui_cfg;
        tui_cfg.out = console::output_file();
        tui_cfg.color = params.use_color;
        tui_cfg.multiline_input = params.multiline_input;
        tui_cfg.working_dir = working_dir;
        tui_cfg.session_path = session_path;
        tui_cfg.meta = inf;
        tui_cfg.permissions = &tui_permissions;
        tui_cfg.interrupt = []() { g_is_interrupted.store(true); };
        tui = std::make_unique<tui_renderer>(std::move(tui_cfg));
    }

    auto emit_tui_or_console = [&](const std::string & text,
                                   tui_transcript_style style = tui_transcript_style::NORMAL) {
        if (tui) {
            tui->post_transcript(text, style);
        } else {
            switch (style) {
                case tui_transcript_style::FAILURE:
                    console::error("%s", text.c_str());
                    break;
                case tui_transcript_style::INFO:
                    console::set_display(DISPLAY_TYPE_INFO);
                    console::log("%s", text.c_str());
                    console::set_display(DISPLAY_TYPE_RESET);
                    break;
                default:
                    console::log("%s", text.c_str());
                    break;
            }
        }
    };

    if (resume_ptr && !resume_ptr->messages.empty()) {
        for (const auto & m : resume_ptr->messages) {
            std::string role = m.value("role", "");
            if (role == "user") {
                emit_tui_or_console("\xE2\x80\xBA " + extract_text(m) + "\n", tui_transcript_style::USER_INPUT);
            } else if (role == "assistant") {
                std::string content = extract_text(m);
                if (!content.empty()) {
                    emit_tui_or_console(content + "\n");
                }
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    for (const auto & tc : m["tool_calls"]) {
                        if (tc.contains("function")) {
                            std::string name = tc["function"].value("name", "");
                            emit_tui_or_console("> " + name + "\n", tui_transcript_style::INFO);
                        }
                    }
                }
            } else if (role == "tool") {
                std::string output = extract_text(m);
                if (output.length() > 500) {
                    output = output.substr(0, 500) + "\n... (truncated)";
                }
                emit_tui_or_console(output + "\n");
            }
        }
        emit_tui_or_console("--- session resumed ---\n");
    }

    // Track if we have an initial prompt to process
    bool first_turn = !initial_prompt.empty();

    // Main loop
    while (true) {
        std::string buffer;
        std::vector<std::pair<std::vector<uint8_t>, std::string>> pasted_images;

        if (first_turn) {
            // Use the initial prompt
            buffer = initial_prompt;
            first_turn = false;
            if (tui) {
                tui->post_transcript("\n> " + buffer + "\n", tui_transcript_style::USER_INPUT);
            } else {
                console::set_display(DISPLAY_TYPE_USER_INPUT);
                console::log("\n\xE2\x80\xBA %s\n", buffer.c_str());
                console::set_display(DISPLAY_TYPE_RESET);
            }
        } else {
            if (tui) {
                tui_command command;
                bool received = false;
                while (!received && !g_terminate_requested.load()) {
                    received = tui->wait_for_command(command, 200);
                }
                if (!received || command.eof) {
                    break;
                }
                buffer = std::move(command.text);
                pasted_images = std::move(command.images);
                // The input region clears on submit, so echo the submission into the transcript
                if (!buffer.empty()) {
                    tui->post_transcript("\n\xE2\x80\xBA " + buffer + "\n",
                                         tui_transcript_style::USER_INPUT);
                }
            } else {
                // Interactive input
                console::set_display(DISPLAY_TYPE_USER_INPUT);
                console::log("\n\xE2\x80\xBA ");

                std::string line;
                bool another_line = true;
                do {
                    another_line = console::readline(line, params.multiline_input);
                    buffer += line;
                } while (another_line);

                console::set_display(DISPLAY_TYPE_RESET);

                // Collect clipboard images pasted during readline (via Ctrl+V)
                pasted_images = console::take_pending_images();

                if (should_stop()) {
                    g_is_interrupted.store(false);
                    break;
                }

                // Remove trailing newline
                if (!buffer.empty() && buffer.back() == '\n') {
                    buffer.pop_back();
                }
            }

            if (!tui && !buffer.empty() && buffer.back() == '\n') {
                buffer.pop_back();
            }

            // Skip empty input (unless images were pasted)
            if (buffer.empty() && pasted_images.empty()) {
                continue;
            }

            // Handle ! prefix: run shell command
            if (!buffer.empty() && buffer[0] == '!') {
                bool exclude_from_context = (buffer.size() >= 2 && buffer[1] == '!');
                size_t cmd_start = exclude_from_context ? 2 : 1;
                std::string cmd = buffer.substr(cmd_start);

                // Trim leading whitespace
                size_t first = cmd.find_first_not_of(" \t");
                if (first == std::string::npos) {
                    emit_tui_or_console("Usage: !<command> or !!<command>\n");
                    continue;
                }
                cmd = cmd.substr(first);

                if (tui) {
                    // "!!" commands are hidden from the model - show a distinct prefix
                    if (exclude_from_context) {
                        tui->post_transcript("\n$ " + cmd + "  (hidden from model)\n",
                                             tui_transcript_style::INFO);
                    } else {
                        tui->post_transcript("\n$ " + cmd + "\n", tui_transcript_style::INFO);
                    }
                } else {
                    console::set_display(DISPLAY_TYPE_PROMPT);
                    if (exclude_from_context) {
                        console::log("\n$ %s  (hidden from model)\n", cmd.c_str());
                    } else {
                        console::log("\n$ %s\n", cmd.c_str());
                    }
                    console::set_display(DISPLAY_TYPE_RESET);
                }
                g_is_interrupted.store(false);
                std::string shell_pending;
                std::function<void(std::string_view)> on_output;
                if (tui) {
                    // Post whole lines only: a pipe read can split a line, a UTF-8 sequence, or an escape code
                    on_output = [&](std::string_view chunk) {
                        shell_pending.append(chunk.data(), chunk.size());
                        size_t last_nl = shell_pending.rfind('\n');
                        if (last_nl != std::string::npos) {
                            tui->post_transcript(shell_pending.substr(0, last_nl + 1));
                            shell_pending.erase(0, last_nl + 1);
                        }
                    };
                    // The generating state routes ESC/Ctrl+C to the interrupt flag that stops the command
                    tui->set_generating(true);
                }
                auto cmd_result = run_user_command(cmd, working_dir, g_is_interrupted, on_output);
                if (tui) {
                    tui->set_generating(false);
                    if (!shell_pending.empty()) {
                        tui->post_transcript(shell_pending + "\n");
                    }
                }

                // Ensure output ends with newline for clean display
                if (!tui && !cmd_result.output.empty() && cmd_result.output.back() != '\n') {
                    fwrite("\n", 1, 1, stdout);
                }

                if (cmd_result.exit_code != 0) {
                    if (tui) {
                        tui->post_transcript("[exit code: " + std::to_string(cmd_result.exit_code) + "]\n",
                                             tui_transcript_style::FAILURE);
                    } else {
                        console::set_display(DISPLAY_TYPE_ERROR);
                        console::log("[exit code: %d]\n", cmd_result.exit_code);
                        console::set_display(DISPLAY_TYPE_RESET);
                    }
                }

                if (g_is_interrupted.load()) {
                    emit_tui_or_console("[interrupted]\n");
                    g_is_interrupted.store(false);
                }

                // Inject into LLM context (single ! only)
                if (!exclude_from_context) {
                    std::string context = "[user executed shell command]\n$ " + cmd + "\n" + cmd_result.output;
                    if (cmd_result.exit_code != 0) {
                        context += "[exit code: " + std::to_string(cmd_result.exit_code) + "]\n";
                    }
                    agent.add_context_message("user", context);
                }

                continue;
            }

            // Process commands
            std::string command_buffer = buffer;
            while (!command_buffer.empty() &&
                   (command_buffer.back() == ' ' || command_buffer.back() == '\t')) {
                command_buffer.pop_back();
            }
            if (command_buffer == "/exit" || command_buffer == "/quit") {
                break;
            }
            if (command_buffer == "/clear") {
                agent.clear();
                if (tui) {
                    tui_permissions.clear_session();
                    tui->post_stats(agent.get_stats(), 0);
                    // Separate the old transcript from the cleared context
                    tui->post_transcript(
                        "\n--- conversation cleared ---\n\n",
                        tui_transcript_style::INFO);
                }
                emit_tui_or_console("Conversation cleared.\n", tui_transcript_style::INFO);
                continue;
            }
            if (command_buffer == "/compact") {
                emit_tui_or_console("\nCompacting...\n", tui_transcript_style::INFO);
                g_is_interrupted.store(false);
                if (tui) {
                    tui->set_generating(true);
                }
                bool compacted = agent.compact();
                if (tui) {
                    tui->set_generating(false);
                }
                if (g_is_interrupted.load()) {
                    emit_tui_or_console("[Compaction cancelled]\n");
                    g_is_interrupted.store(false);
                } else if (compacted) {
                    emit_tui_or_console("Context compacted.\n", tui_transcript_style::INFO);
                } else {
                    emit_tui_or_console("Nothing to compact (conversation too short).\n");
                }
                continue;
            }
            if (command_buffer == "/tools") {
                std::ostringstream ss;
                ss << "\nAvailable tools:\n";
                for (const auto * tool : tool_registry::instance().get_all_tools()) {
                    ss << "  " << tool->name << ":\n";
                    ss << "    " << tool->description << "\n";
                }
                emit_tui_or_console(ss.str());
                continue;
            }
            if (command_buffer == "/stats") {
                const auto & stats = agent.get_stats();
                emit_tui_or_console(stats_text(stats));
                continue;
            }
            if (command_buffer == "/skills") {
                const auto & skills = resources.skills.get_skills();
                std::ostringstream ss;
                if (skills.empty()) {
                    ss << "\nNo skills discovered.\n";
                    ss << "Skills are loaded from:\n";
                    ss << "  ./.llama-agent/skills/  (project-local)\n";
                    ss << "  ~/.llama-agent/skills/  (user-global)\n";
                } else {
                    ss << "\nAvailable skills:\n";
                    for (const auto & skill : skills) {
                        ss << "  " << skill.name << ":\n";
                        ss << "    " << skill.description << "\n";
                        ss << "    Path: " << skill.path << "\n";
                    }
                }
                emit_tui_or_console(ss.str());
                continue;
            }
            if (command_buffer == "/agents") {
                const auto & files = resources.agents_md.get_files();
                std::ostringstream ss;
                if (files.empty()) {
                    ss << "\nNo AGENTS.md files discovered.\n";
                    ss << "AGENTS.md files are searched from:\n";
                    ss << "  ./AGENTS.md to git root  (project-specific)\n";
                    ss << "  ~/.llama-agent/AGENTS.md  (global)\n";
                } else {
                    ss << "\nDiscovered AGENTS.md files (closest first):\n";
                    for (const auto & file : files) {
                        ss << "  " << file.relative_path;
                        if (file.depth == 0) {
                            ss << " (highest precedence)";
                        }
                        ss << "\n    " << file.content.size() << " bytes\n";
                    }
                }
                emit_tui_or_console(ss.str());
                continue;
            }
        }

        if (!tui) {
            console::log("\n");
        }

        // Build user content — multimodal if images were pasted, plain string otherwise
        json user_content;
        if (!pasted_images.empty() && (inf.has_vision || !inf.image_support_known)) {
            if (tui) {
                tui->post_transcript("[attached " + std::to_string(pasted_images.size()) +
                                     " image(s)]\n", tui_transcript_style::INFO);
            } else {
                // Show terminal preview of pasted images
                for (const auto & [bytes, mime] : pasted_images) {
                    render_image_to_terminal(bytes.data(), bytes.size(), mime);
                }
            }
            // Strip [image] / [image N] markers that were inserted for display only
            std::string clean_text = buffer;
            for (size_t n = pasted_images.size(); n >= 1; n--) {
                std::string marker = n == 1 ? "[image]" : "[image " + std::to_string(n) + "]";
                size_t pos = clean_text.find(marker);
                if (pos != std::string::npos) {
                    clean_text.erase(pos, marker.size());
                }
            }
            // Trim whitespace left by marker removal
            while (!clean_text.empty() && clean_text.back() == ' ') clean_text.pop_back();

            // Build content block array: text + image_url blocks
            user_content = json::array();
            if (!clean_text.empty()) {
                user_content.push_back({{"type", "text"}, {"text", clean_text}});
            }
            for (const auto & [bytes, mime] : pasted_images) {
                std::string b64 = base64::encode(
                    reinterpret_cast<const char *>(bytes.data()), bytes.size());
                user_content.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", "data:" + mime + ";base64," + b64}}}
                });
            }
        } else {
            if (!pasted_images.empty()) {
                if (tui) {
                    tui->post_transcript("[model lacks vision - " +
                                         std::to_string(pasted_images.size()) +
                                         " image(s) not included]\n",
                                         tui_transcript_style::FAILURE);
                } else {
                    console::set_display(DISPLAY_TYPE_ERROR);
                    console::log("[model lacks vision - %zu image(s) not included]\n",
                                 pasted_images.size());
                    console::set_display(DISPLAY_TYPE_RESET);
                }
            }
            user_content = buffer;
        }

        // Run agent loop
        agent_loop_result result;
        if (tui) {
            g_is_interrupted.store(false);
            tui->set_generating(true);
            result = agent.run_streaming(
                user_content,
                [&](const agent_event & event) {
                    tui->post_agent_event(event);
                },
                should_stop,
                &tui_permissions);
            tui->set_generating(false);
        } else {
            result = agent.run(user_content);
            console::log("\n");
        }

        // Display result
        switch (result.stop_reason) {
            case agent_stop_reason::COMPLETED:
                if (tui) {
                    tui->post_transcript("\n[Completed in " + std::to_string(result.iterations) +
                                         " iteration(s)]\n", tui_transcript_style::INFO);
                } else {
                    console::set_display(DISPLAY_TYPE_INFO);
                    console::log("\n[Completed in %d iteration(s)]\n", result.iterations);
                    console::set_display(DISPLAY_TYPE_RESET);
                }
                break;
            case agent_stop_reason::MAX_ITERATIONS:
                if (tui) {
                    tui->post_transcript("[Stopped: max iterations reached (" +
                                         std::to_string(result.iterations) + ")]\n",
                                         tui_transcript_style::FAILURE);
                } else {
                    console::set_display(DISPLAY_TYPE_ERROR);
                    console::log("[Stopped: max iterations reached (%d)]\n", result.iterations);
                    console::set_display(DISPLAY_TYPE_RESET);
                }
                break;
            case agent_stop_reason::USER_CANCELLED:
                emit_tui_or_console("[Cancelled by user]\n");
                g_is_interrupted.store(false);
                break;
            case agent_stop_reason::AGENT_ERROR:
                emit_tui_or_console("[Error occurred]\n", tui_transcript_style::FAILURE);
                break;
        }

        if (params.single_turn) {
            break;
        }
    }

    if (tui) {
        tui->shutdown();
        tui.reset();
    }

    console::set_display(DISPLAY_TYPE_RESET);
    console::log("\nExiting...\n");

#ifndef _WIN32
    // Shutdown MCP servers
    mcp_mgr.shutdown_all();
#endif

    if (ctx_server) {
        ctx_server->terminate();
    }
    if (inference_thread.joinable()) {
        inference_thread.join();
    }

    common_log_set_verbosity_thold(LOG_LEVEL_INFO);
    if (ctx_server) {
        common_memory_breakdown_print(ctx_server->get_llama_context());
    }

    return 0;
}
