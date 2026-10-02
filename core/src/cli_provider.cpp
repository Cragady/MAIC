// The `cli` provider kind: an agent CLI run headless as a text-only model (docs/settings.md, Providers). Claude
// Code speaks stream-json both ways; the protocol is the one diction's ClaudeScribe used (diction/scribe.py):
// one user line in, lines out until a `result` line, whose `is_error` marks a failure.
#include "llm_http.hpp"

#include "maic/paths.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
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

// One running CLI. The system prompt is fixed when it starts, so each purpose (the reviewer, titles, summaries)
// gets its own, as diction kept one per mode; it serves `max_requests` requests and is then replaced, which
// bounds the conversation it carries.
struct CliProcess {
    std::mutex mu;  // one request at a time
    pid_t pid = -1;
    int in = -1, out = -1;
    int requests = 0;

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

void spawn(CliProcess& p, const Provider& provider, const std::string& model, const std::string& system) {
    std::string command = provider.options.value("command", "claude");
    // Headless and stream-json both ways. Text only, so MAIC's harness stays the one judge of what runs: every
    // built-in tool off, no MCP server (an empty config, and strict so the user's own are not loaded), and the
    // dontAsk permission mode, which refuses anything not pre-approved. Nothing is kept on disk.
    std::vector<std::string> args = {command, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
                                     "--include-partial-messages", "--tools", "", "--strict-mcp-config", "--mcp-config", R"({"mcpServers":{}})",
                                     "--permission-mode", "dontAsk", "--no-session-persistence", "--system-prompt", system, "--model", model};
    for (const auto& a : provider.options.value("args", json::array())) {
        std::string s = a.get<std::string>();
        for (const char* ours : {"--tools", "--mcp-config", "--strict-mcp-config", "--permission-", "--allowedTools", "--allowed-tools", "--dangerously-", "--allow-dangerously-"}) {
            if (s.rfind(ours, 0) == 0) throw std::runtime_error("providers." + provider.name + ".options.args may not contain " + s + ": a cli provider runs text only, with its tools off");
        }
        args.push_back(s);
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    // The user's own login and plan: an API key in the environment would make the CLI bill the API instead.
    std::vector<char*> envp;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "ANTHROPIC_API_KEY=", 18) != 0 && std::strncmp(*e, "ANTHROPIC_AUTH_TOKEN=", 21) != 0) envp.push_back(*e);
    }
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
    std::string line = std::string("[") + stamp + "] maic: started " + command + " --model " + model + " (pid " + std::to_string(pid) + ")\n";
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

// The conversation as one user turn: a lone user message as it is, anything longer with each turn labelled.
std::string prompt_of(const Provider& provider, const std::vector<Message>& rest) {
    for (const auto& m : rest) {
        if (!m.images.empty()) throw std::runtime_error(provider.name + " takes text only; a picture needs another provider");
    }
    if (rest.size() == 1 && rest[0].role == "user") return rest[0].content;
    std::string out;
    for (const auto& m : rest) {
        std::string label = m.role == "user" ? "User" : m.role == "assistant" ? "Assistant" : m.role == "system" ? "System note" : "Tool result";
        out += "[" + label + "]: " + m.content + "\n\n";
    }
    return out;
}

}  // namespace

Message chat_cli(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages, const nlohmann::json& tools,
                 const TextSink& on_text, const std::atomic<bool>& cancel) {
    if (!tools.empty()) {
        throw std::runtime_error(provider.name + " is a text-only provider here; use it for small_model, the reviewer, titles or compaction; agent use arrives with level 2");
    }
    std::string system;
    size_t first = 0;
    for (; first < messages.size() && messages[first].role == "system"; ++first) system += (system.empty() ? "" : "\n\n") + messages[first].content;
    std::string prompt = prompt_of(provider, {messages.begin() + static_cast<long>(first), messages.end()});
    std::string line = dump({{"type", "user"}, {"message", {{"role", "user"}, {"content", {{{"type", "text"}, {"text", prompt}}}}}}}) + "\n";

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
    for (bool again = false; result.is_null(); again = true) {
        if (p.alive() && p.requests >= provider.options.value("max_requests", 20)) p.stop(std::chrono::milliseconds(0));  // idle, nothing to lose
        bool fresh = !p.alive();
        if (fresh) spawn(p, provider, options.model, system);
        size_t sent = 0;
        bool heard = false, ended = false;
        LineSplitter lines;
        char buf[16384];
        while (result.is_null() && !ended) {
            if (cancel.load()) {
                p.stop(std::chrono::milliseconds(0));
                throw Cancelled();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                p.stop(std::chrono::milliseconds(0));
                throw std::runtime_error(who(provider, options.model) + ": no reply in " + std::to_string(timeout_s) + " s; the process was stopped and the next request starts a new one");
            }
            pollfd fds[2] = {{p.out, POLLIN, 0}, {p.in, POLLOUT, 0}};
            nfds_t n_fds = sent < line.size() ? 2 : 1;
            if (poll(fds, n_fds, 100) <= 0) continue;
            if (n_fds == 2 && fds[1].revents) {
                ssize_t n = write_quietly(p.in, line.data() + sent, line.size() - sent);
                if (n > 0) sent += static_cast<size_t>(n);
                else if (errno != EAGAIN && errno != EINTR) ended = true;
            }
            if (ended || !fds[0].revents) continue;
            ssize_t n = read(p.out, buf, sizeof(buf));
            if (n <= 0) {
                ended = true;
                continue;
            }
            lines.feed(std::string_view(buf, static_cast<size_t>(n)), [&](const std::string& l) {
                json j = json::parse(l, nullptr, false);
                if (!j.is_object() || !result.is_null()) return;
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
                }
            });
        }
        if (!ended) break;
        p.stop(std::chrono::milliseconds(0));
        // A process that served earlier requests and has died since: once more on a new one, as diction did.
        if (fresh || heard || again) {
            throw std::runtime_error(who(provider, options.model) + ": the process ended without a reply (its stderr is in " +
                                     (state_dir() / "logs" / (provider.name + ".log")).string() + ")");
        }
    }
    ++p.requests;
    if (result.value("is_error", false)) result_error(provider, options.model, result);
    Message reply{"assistant", result.contains("result") && result["result"].is_string() ? result["result"].get<std::string>() : text};
    if (text.empty() && !reply.content.empty()) on_text(reply.content, false);
    json u = result.value("usage", json::object());
    reply.usage.input = u.value("input_tokens", 0) + u.value("cache_creation_input_tokens", 0) + u.value("cache_read_input_tokens", 0);
    reply.usage.output = u.value("output_tokens", 0);
    json models = result.value("modelUsage", json::object());
    for (const auto& [name, m] : models.items()) reply.usage.context = std::max(reply.usage.context, m.value("contextWindow", 0));
    return reply;
}

}  // namespace maic::detail
