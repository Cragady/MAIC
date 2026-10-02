// The `cli` provider kind: an agent CLI run headless (docs/settings.md, Providers). Claude Code speaks stream-json
// both ways; the protocol is the one diction's ClaudeScribe used (diction/scribe.py): one user line in, lines out
// until a `result` line, whose `is_error` marks a failure. Without tool schemas it is a text-only model (level 1).
// With them it runs the agent loop (level 2): its own tools stay off and MAIC's are served to it over MCP, from a
// unix socket in the runtime directory, and each call it makes comes back to the caller as the reply's tool calls,
// so MAIC's harness judges and runs every one exactly as for its own model.
#include "llm_http.hpp"

#include "maic/paths.hpp"
#include "maic/session.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <thread>
#include <vector>

extern char** environ;

namespace maic::detail {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// A write to a child that has already gone raises SIGPIPE, which would end MAIC: block it on this thread for the
// write and swallow the one it may have left pending.
ssize_t write_quietly(int fd, const char* data, size_t n) {
    sigset_t pipe_only, before;
    sigemptyset(&pipe_only);
    sigaddset(&pipe_only, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_only, &before);
    ssize_t written = write(fd, data, n);
    int saved = errno;
    if (written < 0 && saved == EPIPE) {
        timespec none{0, 0};
        sigtimedwait(&pipe_only, nullptr, &none);
    }
    pthread_sigmask(SIG_SETMASK, &before, nullptr);
    errno = saved;
    return written;
}

// Writes all of `data`, or returns false.
bool write_all(int fd, const std::string& data) {
    for (size_t sent = 0; sent < data.size();) {
        ssize_t n = write_quietly(fd, data.data() + sent, data.size() - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && errno == EAGAIN) {
            pollfd w{fd, POLLOUT, 0};
            poll(&w, 1, 100);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// The MCP versions this server speaks, newest first (docs/standards.md, MCP).
const std::vector<std::string> kMcpVersions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

// MAIC's tools served to one CLI over MCP: newline-delimited JSON-RPC on a unix socket the CLI reaches through
// `maic mcp-bridge`. A thread answers the handshake and tools/list itself; each tools/call waits in `calls` until
// the agent loop has judged and run it, and take() hands it to the reply.
struct McpServer {
    struct Call {
        json id;
        std::string name;
        json arguments;
    };
    fs::path path;
    json tools;  // MCP's Tool objects
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Call> calls;
    std::mutex write_mu;
    int listen_fd = -1, conn = -1;
    std::atomic<bool> stopping{false};
    std::thread thread;

    explicit McpServer(json tool_list) : tools(std::move(tool_list)) {
        static std::atomic<int> counter{0};
        fs::path dir = runtime_sessions_dir().parent_path() / "mcp";
        fs::create_directories(dir);
        chmod(dir.c_str(), 0700);
        std::random_device rd;
        path = dir / (std::to_string(getpid()) + "-" + std::to_string(++counter) + "-" + std::to_string(rd() % 1000000) + ".sock");
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.string().size() >= sizeof(addr.sun_path)) throw std::runtime_error("the MCP socket path is too long: " + path.string());
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        fs::remove(path);
        if (listen_fd < 0 || bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || chmod(path.c_str(), 0600) != 0 || listen(listen_fd, 1) != 0) {
            std::string why = std::strerror(errno);
            if (listen_fd >= 0) close(listen_fd);
            throw std::runtime_error("can't serve MAIC's tools on " + path.string() + ": " + why);
        }
        thread = std::thread([this] { serve(); });
    }
    ~McpServer() {
        stopping = true;
        cv.notify_all();
        if (thread.joinable()) thread.join();
        if (conn >= 0) close(conn);
        close(listen_fd);
        std::error_code ec;
        fs::remove(path, ec);
    }

    void send(const json& j) {
        std::lock_guard lock(write_mu);
        if (conn >= 0) write_all(conn, dump(j) + "\n");
    }
    void answer(const json& id, const json& result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }
    void refuse(const json& id, int code, const std::string& message) {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
    }

    void handle(const json& j) {
        if (!j.is_object() || !j.contains("method")) return;  // a response, or not JSON-RPC: nothing asked of us
        std::string method = j.value("method", "");
        json params = j.value("params", json::object());
        if (!j.contains("id")) {
            if (method == "notifications/cancelled" && params.contains("requestId")) {
                // The CLI gave up on a call: it will not read the answer, so the call no longer waits.
                std::lock_guard lock(mu);
                std::erase_if(calls, [&](const Call& c) { return c.id == params["requestId"]; });
            }
            return;  // notifications/initialized and the rest need no answer
        }
        if (method == "initialize") {
            std::string asked = params.value("protocolVersion", "");
            bool known = std::find(kMcpVersions.begin(), kMcpVersions.end(), asked) != kMcpVersions.end();
            answer(j["id"], {{"protocolVersion", known ? asked : kMcpVersions.front()},
                             {"capabilities", {{"tools", {{"listChanged", false}}}}},
                             {"serverInfo", {{"name", "maic"}, {"version", "1"}}}});
        } else if (method == "ping") {
            answer(j["id"], json::object());
        } else if (method == "tools/list") {
            answer(j["id"], {{"tools", tools}});
        } else if (method == "tools/call") {
            {
                std::lock_guard lock(mu);
                calls.push_back({j["id"], params.value("name", ""), params.value("arguments", json::object())});
            }
            cv.notify_all();
        } else {
            refuse(j["id"], -32601, "Method not found: " + method);
        }
    }

    void serve() {
        LineSplitter lines;
        char buf[16384];
        while (!stopping.load()) {
            pollfd fds[2] = {{listen_fd, POLLIN, 0}, {conn, POLLIN, 0}};
            if (poll(fds, conn >= 0 ? 2 : 1, 100) <= 0) continue;
            if (fds[0].revents & POLLIN) {
                int c = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (c >= 0) {
                    // One CLI at a time: a new connection is the same CLI's bridge again.
                    std::lock_guard lock(write_mu);
                    if (conn >= 0) close(conn);
                    conn = c;
                    lines = LineSplitter();
                }
                continue;
            }
            if (conn < 0 || !fds[1].revents) continue;
            ssize_t n = read(conn, buf, sizeof(buf));
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                std::lock_guard lock(write_mu);
                close(conn);
                conn = -1;
                continue;
            }
            if (n > 0) lines.feed(std::string_view(buf, static_cast<size_t>(n)), [&](const std::string& l) { handle(json::parse(l, nullptr, false)); });
        }
    }

    // The next tools/call for `name` (the one with these arguments first), once the CLI has sent it. Throws when
    // `gone` says the CLI ended, on cancel, or at the deadline.
    Call take(const std::string& name, const json& arguments, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel,
              const std::function<bool()>& gone, const std::string& who) {
        std::unique_lock lock(mu);
        for (;;) {
            auto match = std::find_if(calls.begin(), calls.end(), [&](const Call& c) { return c.name == name && c.arguments == arguments; });
            if (match == calls.end()) match = std::find_if(calls.begin(), calls.end(), [&](const Call& c) { return c.name == name; });
            if (match != calls.end()) {
                Call c = *match;
                calls.erase(match);
                return c;
            }
            if (cancel.load()) throw Cancelled();
            if (gone()) throw std::runtime_error(who + ": the process ended while MAIC ran its tool calls");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(who + ": it never asked for the " + name + " call it announced");
            cv.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
};

// One running CLI. The system prompt is fixed when it starts, so each purpose (the reviewer, titles, summaries)
// gets its own, as diction kept one per mode; it serves `max_requests` requests and is then replaced, which
// bounds the conversation it carries. An agent (level 2) carries one conversation: `seen` is what of it the CLI
// has had, a hash per message, and `pending` the calls it waits on.
struct CliProcess {
    std::mutex mu;  // one request at a time
    pid_t pid = -1;
    int in = -1, out = -1;
    int requests = 0;
    LineSplitter lines;
    std::deque<std::string> backlog;  // lines read past the end of the last reply
    bool agent = false;
    std::string provider;
    std::chrono::steady_clock::time_point used;
    std::unique_ptr<McpServer> mcp;
    std::vector<size_t> seen;
    std::vector<ToolCall> pending;

    bool alive() {
        if (pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0) return true;
        if (pid > 0) pid = -1;  // reaped
        close_pipes();
        return false;
    }
    void close_pipes() {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        in = out = -1;
        requests = 0;
        lines = LineSplitter();
        backlog.clear();
        mcp.reset();
        seen.clear();
        pending.clear();
    }
    // Without its stdin it exits by itself; what is still there after `grace` is terminated.
    void stop(std::chrono::milliseconds grace) {
        if (in >= 0) close(in);
        in = -1;
        auto deadline = std::chrono::steady_clock::now() + grace;
        while (pid > 0 && waitpid(pid, nullptr, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(-pid, SIGKILL);
                waitpid(pid, nullptr, 0);
                break;
            }
            usleep(20000);
        }
        pid = -1;
        close_pipes();
    }
};

// Every process MAIC started, reaped when MAIC exits.
struct Registry {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<CliProcess>> procs;
    ~Registry() {
        for (auto& [key, p] : procs) {
            if (p->in >= 0) close(p->in);
            p->in = -1;
        }
        for (auto& [key, p] : procs) p->stop(std::chrono::milliseconds(1500));
    }
};

Registry& registry() {
    static Registry r;
    return r;
}

std::string who(const Provider& provider, const std::string& model) {
    return provider.name + "/" + model;
}

[[noreturn]] void not_found(const Provider& provider, const std::string& model, const std::string& command) {
    static const std::map<std::string, std::pair<std::string, std::string>> presets = {
        {"haiku", {"claude-haiku-cli", "haiku-4.5"}}, {"sonnet", {"claude-sonnet-cli", "sonnet-5"}}, {"opus", {"", "opus-5.5"}}};
    auto it = presets.find(model);
    std::string preset = it != presets.end() && !it->second.first.empty() ? " (the " + it->second.first + " preset)" : "";
    std::string api = it != presets.end() ? "the API preset " + it->second.second : "anthropic/" + model + " on the API";
    throw std::runtime_error(who(provider, model) + preset + ": `" + command + "` is not on PATH. Install Claude Code and log in, set providers." +
                             provider.name + ".options.command, or use " + api + " instead (ANTHROPIC_API_KEY, billed by the API).");
}

void spawn(CliProcess& p, const Provider& provider, const std::string& model, const std::string& system, const McpServer* mcp = nullptr) {
    std::string command = provider.options.value("command", "claude");
    // Headless and stream-json both ways, with every built-in tool off, so MAIC's harness stays the one judge of
    // what runs. Its MCP servers are only MAIC's (strict, so the user's own are not loaded): none for text only,
    // or MAIC's tools for an agent, pre-approved so the CLI neither asks about nor classifies them. The dontAsk
    // permission mode refuses anything else. Nothing is kept on disk. No settings files either (the user's
    // CLAUDE.md, plugins and hooks among them) unless `setting_sources` names some.
    std::string sources = provider.options.value("setting_sources", "");
    for (size_t at = 0; !sources.empty() && at <= sources.size();) {
        size_t comma = std::min(sources.find(',', at), sources.size());
        std::string s = sources.substr(at, comma - at);
        if (s != "user" && s != "project" && s != "local") {
            throw std::runtime_error("providers." + provider.name + ".options.setting_sources: '" + s + "' is not user, project or local");
        }
        at = comma + 1;
    }
    json servers = json::object();
    if (mcp) {
        servers["maic"] = {{"type", "stdio"}, {"command", fs::read_symlink("/proc/self/exe").string()}, {"args", {"mcp-bridge", mcp->path.string()}}};
    }
    std::vector<std::string> args = {command, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
                                     "--include-partial-messages", "--tools", "", "--strict-mcp-config", "--mcp-config", dump({{"mcpServers", servers}}),
                                     "--permission-mode", "dontAsk", "--setting-sources", sources, "--no-session-persistence",
                                     "--system-prompt", system, "--model", model};
    if (mcp) args.insert(args.end(), {"--allowedTools", "mcp__maic"});
    for (const auto& a : provider.options.value("args", json::array())) {
        std::string s = a.get<std::string>();
        for (const char* ours : {"--tools", "--mcp-config", "--strict-mcp-config", "--permission-", "--allowedTools", "--allowed-tools", "--dangerously-", "--allow-dangerously-", "--setting-sources"}) {
            if (s.rfind(ours, 0) == 0) throw std::runtime_error("providers." + provider.name + ".options.args may not contain " + s + ": a cli provider runs text only, with its tools off");
        }
        args.push_back(s);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // The user's own login and plan: an API key in the environment would make the CLI bill the API instead. No other
    // provider's key (DEEPSEEK_API_KEY, any configured api_key_env) is its business either.
    std::vector<char*> envp;
    for (char** e = environ; *e; ++e) {
        std::string_view name(*e, std::strcspn(*e, "="));
        if (!is_key_env(name) && name != "ANTHROPIC_AUTH_TOKEN" && name != "MCP_TOOL_TIMEOUT") envp.push_back(*e);
    }
    // A call waits on MAIC's harness, a person's approval among it: the CLI must not give up on it first.
    std::string tool_timeout = "MCP_TOOL_TIMEOUT=" + std::to_string(provider.options.value("tool_timeout", 86400) * 1000LL);
    if (mcp) envp.push_back(tool_timeout.data());
    envp.push_back(nullptr);

    // A directory of its own, so no project's CLAUDE.md is read; stderr goes to <state>/logs/<provider>.log.
    fs::path cwd = state_dir() / "cli" / provider.name, log = state_dir() / "logs" / (provider.name + ".log");
    fs::create_directories(cwd);
    fs::create_directories(log.parent_path());
    int log_fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    int in[2], out[2];
    if (log_fd < 0 || pipe2(in, O_CLOEXEC) != 0) throw std::runtime_error(who(provider, model) + ": " + std::strerror(errno));
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]), close(in[1]), close(log_fd);
        throw std::runtime_error(who(provider, model) + ": " + std::strerror(errno));
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, log_fd, 2);
    posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    // Its own process group: Ctrl-C in MAIC's terminal must not end it mid-reply.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in[0]);
    close(out[1]);
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    if (rc != 0) {
        close(in[1]), close(out[0]), close(log_fd);
        if (rc == ENOENT) not_found(provider, model, command);
        throw std::runtime_error(who(provider, model) + ": can't run " + command + ": " + std::strerror(rc));
    }
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%F %T", std::localtime(&now));
    std::string line = std::string("[") + stamp + "] maic: started " + command + " --model " + model + (mcp ? " as an agent" : "") + " (pid " + std::to_string(pid) + ")\n";
    (void)!write(log_fd, line.data(), line.size());
    close(log_fd);
    p.pid = pid;
    p.in = in[1];
    p.out = out[0];
    p.requests = 0;
}

// A failed result: the CLI's text, with a status that lets is_usage_limit and chat()'s retries judge it.
[[noreturn]] void result_error(const Provider& provider, const std::string& model, const json& j) {
    std::string msg = j.contains("result") && j["result"].is_string() ? j["result"].get<std::string>() : "";
    if (msg.empty() && j.contains("errors") && j["errors"].is_array()) {
        for (const auto& e : j["errors"]) {
            if (e.is_string()) msg += (msg.empty() ? "" : "; ") + e.get<std::string>();
        }
    }
    std::string subtype = j.value("subtype", "");
    if (msg.empty()) msg = subtype.empty() ? "unknown error" : subtype;
    std::string lower = msg;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int status = 0;
    std::smatch m;
    static const std::regex api_status("API Error: ([0-9]{3})");
    if (std::regex_search(msg, m, api_status)) status = std::stoi(m[1].str());
    else if (lower.find("limit") != std::string::npos || lower.find("usage-credits") != std::string::npos || lower.find("quota") != std::string::npos) status = 429;
    throw ApiError(status, who(provider, model) + ": " + msg, 0, subtype);
}

// Each turn labelled, an agent's calls and their results written out too.
std::string labelled(const std::vector<Message>& rest) {
    std::string out;
    for (const auto& m : rest) {
        std::string label = m.role == "user" ? "User" : m.role == "assistant" ? "Assistant" : m.role == "system" ? "System note" : "Tool result (" + m.tool_name + ")";
        out += "[" + label + "]: " + m.content;
        for (const auto& c : m.tool_calls) out += "\n(called " + c.name + " " + dump(c.arguments) + ")";
        out += "\n\n";
    }
    return out;
}

// The conversation as one user turn: a lone user message as it is, anything longer labelled, for a CLI that
// takes over a conversation midway too.
std::string prompt_of(const Provider& provider, const std::vector<Message>& rest) {
    for (const auto& m : rest) {
        if (!m.images.empty()) throw std::runtime_error(provider.name + " takes text only; a picture needs another provider");
    }
    if (rest.size() == 1 && rest[0].role == "user") return rest[0].content;
    return labelled(rest);
}

std::string user_line(const std::string& text) {
    return dump({{"type", "user"}, {"message", {{"role", "user"}, {"content", {{{"type", "text"}, {"text", text}}}}}}}) + "\n";
}

// Writes `line` (if any) and hands each line the CLI prints to `on_line` until it returns true; false when the
// process ended first. Lines read past that one wait in the backlog for the next exchange. Cancel and the
// deadline stop the process and throw.
bool exchange(CliProcess& p, const std::string& line, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel,
              const std::string& too_slow, const std::function<bool(const json&)>& on_line) {
    auto take = [&](const std::string& l) {
        json j = json::parse(l, nullptr, false);
        return j.is_object() && on_line(j);
    };
    while (!p.backlog.empty()) {
        std::string l = std::move(p.backlog.front());
        p.backlog.pop_front();
        if (take(l)) return true;
    }
    size_t sent = 0;
    char buf[16384];
    for (;;) {
        if (cancel.load()) {
            p.stop(std::chrono::milliseconds(0));
            throw Cancelled();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            p.stop(std::chrono::milliseconds(0));
            throw std::runtime_error(too_slow);
        }
        pollfd fds[2] = {{p.out, POLLIN, 0}, {p.in, POLLOUT, 0}};
        nfds_t n_fds = sent < line.size() ? 2 : 1;
        if (poll(fds, n_fds, 100) <= 0) continue;
        if (n_fds == 2 && fds[1].revents) {
            ssize_t n = write_quietly(p.in, line.data() + sent, line.size() - sent);
            if (n > 0) sent += static_cast<size_t>(n);
            else if (errno != EAGAIN && errno != EINTR) return false;
        }
        if (!fds[0].revents) continue;
        ssize_t n = read(p.out, buf, sizeof(buf));
        if (n <= 0) return false;
        std::vector<std::string> got;
        p.lines.feed(std::string_view(buf, static_cast<size_t>(n)), [&](const std::string& l) { got.push_back(l); });
        for (size_t i = 0; i < got.size(); ++i) {
            if (!take(got[i])) continue;
            p.backlog.insert(p.backlog.end(), got.begin() + static_cast<long>(i) + 1, got.end());
            return true;
        }
    }
}

std::string log_path(const Provider& provider) {
    return (state_dir() / "logs" / (provider.name + ".log")).string();
}

void read_usage(const json& result, Usage& usage) {
    json u = result.value("usage", json::object());
    usage.input = u.value("input_tokens", 0) + u.value("cache_creation_input_tokens", 0) + u.value("cache_read_input_tokens", 0);
    usage.output = u.value("output_tokens", 0);
    json models = result.value("modelUsage", json::object());
    for (const auto& [name, m] : models.items()) usage.context = std::max(usage.context, m.value("contextWindow", 0));
}

// MCP describes a tool as {name, description, inputSchema}; the caller gives OpenAI's function format.
json mcp_tools(const json& tools) {
    json out = json::array();
    for (const auto& t : tools) {
        json f = t.value("function", t);
        json schema = f.value("parameters", json::object());
        if (!schema.is_object() || schema.empty()) schema = {{"type", "object"}};
        out.push_back({{"name", f.value("name", "")}, {"description", f.value("description", "")}, {"inputSchema", schema}});
    }
    return out;
}

constexpr std::string_view kToolPrefix = "mcp__maic__";

size_t fingerprint(const Message& m) {
    return std::hash<std::string>{}(dump(message_to_json(m)));
}

// The process that carries this conversation. Keyed by everything fixed when the CLI starts (the system prompt
// and the tools among it) and by the conversation's first message, so a subagent's conversation has its own.
// At most `max_agents` of them stay; past that the one idle longest is stopped.
std::shared_ptr<CliProcess> agent_process(const Provider& provider, const std::string& key) {
    std::lock_guard lock(registry().mu);
    auto& slot = registry().procs[key];
    if (!slot) {
        slot = std::make_shared<CliProcess>();
        slot->agent = true;
        slot->provider = provider.name;
    }
    slot->used = std::chrono::steady_clock::now();
    std::shared_ptr<CliProcess> mine = slot;
    for (size_t limit = static_cast<size_t>(std::max(1, provider.options.value("max_agents", 8)));;) {
        std::vector<std::map<std::string, std::shared_ptr<CliProcess>>::iterator> agents;
        for (auto it = registry().procs.begin(); it != registry().procs.end(); ++it) {
            if (it->second->agent && it->second->provider == provider.name) agents.push_back(it);
        }
        if (agents.size() <= limit) break;
        std::sort(agents.begin(), agents.end(), [](const auto& a, const auto& b) { return a->second->used < b->second->used; });
        bool stopped = false;
        for (auto it : agents) {
            if (it->second == mine) continue;
            std::unique_lock busy(it->second->mu, std::try_to_lock);
            if (!busy.owns_lock()) continue;
            it->second->stop(std::chrono::milliseconds(0));
            busy.unlock();
            registry().procs.erase(it);
            stopped = true;
            break;
        }
        if (!stopped) break;
    }
    return mine;
}

// Level 2: the CLI runs the loop on MAIC's tools. Each reply ends at the CLI's next batch of calls, which the
// caller judges and runs; the next request brings their results, answered over MCP, and the CLI continues.
Message chat_cli_agent(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages, const json& tools,
                       const TextSink& on_text, const std::atomic<bool>& cancel) {
    std::string system;
    size_t first = 0;
    for (; first < messages.size() && messages[first].role == "system"; ++first) system += (system.empty() ? "" : "\n\n") + messages[first].content;
    if (first == messages.size()) throw std::runtime_error(provider.name + ": nothing to send");
    json served = mcp_tools(tools);
    std::string key = std::string("agent") + '\0' + provider.name + '\0' + options.model + '\0' + system + '\0' + provider.options.value("command", "claude") + '\0' +
                      provider.options.value("args", json::array()).dump() + '\0' + served.dump() + '\0' + dump(message_to_json(messages[first]));
    std::shared_ptr<CliProcess> proc = agent_process(provider, key);
    std::lock_guard busy(proc->mu);
    CliProcess& p = *proc;
    std::string tag = who(provider, options.model);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(provider.options.value("timeout", 300));

    std::vector<size_t> hashes;
    for (size_t i = first; i < messages.size(); ++i) hashes.push_back(fingerprint(messages[i]));
    bool continues = p.alive() && p.seen.size() <= hashes.size() && std::equal(p.seen.begin(), p.seen.end(), hashes.begin());
    std::vector<Message> tail(messages.begin() + static_cast<long>(first + (continues ? p.seen.size() : 0)), messages.end());
    // The calls it waits on are answered from the results that follow them, in order. Anything after the results
    // (a note from the harness, a message the user sent mid-turn) rides on the last one, so it arrives at the
    // CLI's next step, as it would for MAIC's own model.
    std::vector<Message> results;
    if (continues && !p.pending.empty()) {
        bool answered = tail.size() >= p.pending.size();
        for (size_t i = 0; answered && i < p.pending.size(); ++i) answered = tail[i].role == "tool" && tail[i].tool_call_id == p.pending[i].id;
        for (size_t i = p.pending.size(); answered && i < tail.size(); ++i) answered = tail[i].role != "tool";
        if (answered) {
            results.assign(tail.begin(), tail.begin() + static_cast<long>(p.pending.size()));
            std::vector<Message> after(tail.begin() + static_cast<long>(p.pending.size()), tail.end());
            if (!after.empty()) results.back().content += "\n\n" + labelled(after);
        }
        continues = answered;
    }
    if (continues && p.pending.empty() && tail.empty()) continues = false;
    std::string line;
    if (!continues) {
        // A new conversation, or one the CLI has not followed (compacted, undone, resumed, another process):
        // a new process takes it over, given the whole conversation at once.
        if (p.pid > 0) p.stop(std::chrono::milliseconds(0));
        p.mcp = std::make_unique<McpServer>(served);
        spawn(p, provider, options.model, system, p.mcp.get());
        tail.assign(messages.begin() + static_cast<long>(first), messages.end());
        line = user_line(prompt_of(provider, tail));
    } else if (results.empty()) {
        line = user_line(prompt_of(provider, tail));
    } else {
        auto gone = [&] { return p.pid <= 0 || waitpid(p.pid, nullptr, WNOHANG) != 0; };
        try {
            for (size_t i = 0; i < p.pending.size(); ++i) {
                McpServer::Call call = p.mcp->take(p.pending[i].name, p.pending[i].arguments, deadline, cancel, gone, tag);
                p.mcp->answer(call.id, {{"content", {{{"type", "text"}, {"text", results[i].content}}}}, {"isError", results[i].is_error}});
            }
        } catch (...) {
            p.stop(std::chrono::milliseconds(0));
            throw;
        }
        p.pending.clear();
    }

    struct Block {
        std::string id, name, input;
        bool tool = false;
    };
    std::map<int, Block> blocks;
    std::vector<ToolCall> calls;
    std::string text;
    Usage usage;
    json result;
    bool finished = exchange(p, line, deadline, cancel, tag + ": no reply in " + std::to_string(provider.options.value("timeout", 300)) + " s; the process was stopped",
                             [&](const json& j) {
        std::string type = j.value("type", "");
        if (type == "result") {
            result = j;
            return true;
        }
        // Its whole-message lines repeat what the stream events said; only those are read.
        if (type != "stream_event" || !j.contains("event")) return false;
        const json& ev = j["event"];
        std::string et = ev.value("type", "");
        int index = ev.value("index", 0);
        if (et == "message_start") {
            json u = ev.value("message", json::object()).value("usage", json::object());
            usage.input = u.value("input_tokens", 0) + u.value("cache_creation_input_tokens", 0) + u.value("cache_read_input_tokens", 0);
            blocks.clear();
        } else if (et == "content_block_start") {
            json b = ev.value("content_block", json::object());
            if (b.value("type", "") == "tool_use") blocks[index] = {b.value("id", ""), b.value("name", ""), "", true};
        } else if (et == "content_block_delta") {
            json d = ev.value("delta", json::object());
            std::string dt = d.value("type", "");
            if (dt == "text_delta") {
                text += d.value("text", "");
                on_text(d.value("text", ""), false);
            } else if (dt == "thinking_delta") {
                on_text(d.value("thinking", ""), true);
            } else if (dt == "input_json_delta") {
                blocks[index].input += d.value("partial_json", "");
            }
        } else if (et == "content_block_stop" && blocks.count(index) && blocks[index].tool) {
            const Block& b = blocks[index];
            json args = b.input.empty() ? json::object() : json::parse(b.input, nullptr, false);
            if (!args.is_object()) args = json::object();
            calls.push_back({b.id, b.name, args});
        } else if (et == "message_delta") {
            usage.output = ev.value("usage", json::object()).value("output_tokens", usage.output);
        } else if (et == "message_stop") {
            return !calls.empty();  // the CLI now runs these calls: they are the caller's to judge
        }
        return false;
    });
    if (!finished) {
        p.stop(std::chrono::milliseconds(0));
        throw std::runtime_error(tag + ": the process ended without a reply (its stderr is in " + log_path(provider) + ")");
    }
    if (!result.is_null() && result.value("is_error", false)) {
        p.stop(std::chrono::milliseconds(0));  // the conversation is not where the caller will pick it up
        result_error(provider, options.model, result);
    }
    Message reply{"assistant", text};
    if (!result.is_null()) {
        if (text.empty() && result.contains("result") && result["result"].is_string()) {
            reply.content = result["result"].get<std::string>();
            on_text(reply.content, false);
        }
        read_usage(result, reply.usage);
    } else {
        reply.usage = usage;
        for (auto& c : calls) {
            if (c.name.rfind(kToolPrefix, 0) != 0) {
                p.stop(std::chrono::milliseconds(0));
                throw std::runtime_error(tag + ": it called " + c.name + ", which is not one of MAIC's tools");
            }
            c.name.erase(0, kToolPrefix.size());
        }
        reply.tool_calls = calls;
        p.pending = calls;
    }
    p.seen = hashes;
    p.seen.push_back(fingerprint(reply));
    return reply;
}

}  // namespace

Message chat_cli(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages, const nlohmann::json& tools,
                 const TextSink& on_text, const std::atomic<bool>& cancel) {
    if (!tools.empty()) return chat_cli_agent(provider, options, messages, tools, on_text, cancel);
    std::string system;
    size_t first = 0;
    for (; first < messages.size() && messages[first].role == "system"; ++first) system += (system.empty() ? "" : "\n\n") + messages[first].content;
    std::string line = user_line(prompt_of(provider, {messages.begin() + static_cast<long>(first), messages.end()}));

    std::string key = provider.name + '\0' + options.model + '\0' + system + '\0' + provider.options.value("command", "claude") + '\0' +
                      provider.options.value("args", json::array()).dump();
    std::shared_ptr<CliProcess> proc;
    {
        std::lock_guard lock(registry().mu);
        auto& slot = registry().procs[key];
        if (!slot) slot = std::make_shared<CliProcess>();
        proc = slot;
    }
    std::lock_guard busy(proc->mu);
    CliProcess& p = *proc;
    int timeout_s = provider.options.value("timeout", 300);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    std::string text;
    bool streamed = false;
    json result;
    for (bool again = false;; again = true) {
        if (p.alive() && p.requests >= provider.options.value("max_requests", 20)) p.stop(std::chrono::milliseconds(0));  // idle, nothing to lose
        bool fresh = !p.alive();
        if (fresh) spawn(p, provider, options.model, system);
        bool heard = false;
        bool finished = exchange(p, line, deadline, cancel,
                                 who(provider, options.model) + ": no reply in " + std::to_string(timeout_s) + " s; the process was stopped and the next request starts a new one",
                                 [&](const json& j) {
            heard = true;
            std::string type = j.value("type", "");
            if (type == "stream_event" && j.contains("event") && j["event"].value("type", "") == "content_block_delta") {
                json d = j["event"].value("delta", json::object());
                if (d.value("type", "") == "text_delta") {
                    streamed = true;
                    text += d.value("text", "");
                    on_text(d.value("text", ""), false);
                } else if (d.value("type", "") == "thinking_delta") {
                    on_text(d.value("thinking", ""), true);
                }
            } else if (type == "assistant" && !streamed && j.contains("message")) {
                // A CLI that sends no partial messages: the whole reply at once.
                for (const auto& b : j["message"].value("content", json::array())) {
                    if (b.value("type", "") != "text") continue;
                    text += b.value("text", "");
                    on_text(b.value("text", ""), false);
                }
            } else if (type == "result") {
                result = j;
                return true;
            }
            return false;
        });
        if (finished) break;
        p.stop(std::chrono::milliseconds(0));
        // A process that served earlier requests and has died since: once more on a new one, as diction did.
        if (fresh || heard || again) {
            throw std::runtime_error(who(provider, options.model) + ": the process ended without a reply (its stderr is in " + log_path(provider) + ")");
        }
    }
    ++p.requests;
    if (result.value("is_error", false)) result_error(provider, options.model, result);
    Message reply{"assistant", result.contains("result") && result["result"].is_string() ? result["result"].get<std::string>() : text};
    if (text.empty() && !reply.content.empty()) on_text(reply.content, false);
    read_usage(result, reply.usage);
    return reply;
}

}  // namespace maic::detail

namespace maic {

int run_mcp_bridge(const std::string& socket_path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) return 2;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string why = std::strerror(errno);
        std::string msg = "maic mcp-bridge: can't reach " + socket_path + ": " + why + "\n";
        (void)!write(2, msg.data(), msg.size());
        return 1;
    }
    char buf[16384];
    for (;;) {
        pollfd fds[2] = {{0, POLLIN, 0}, {s, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        for (int i = 0; i < 2; ++i) {
            if (!fds[i].revents) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n <= 0) return 0;  // either side closed: the CLI or MAIC is done with it
            if (!detail::write_all(i == 0 ? s : 1, std::string(buf, static_cast<size_t>(n)))) return 0;
        }
    }
}

}  // namespace maic
