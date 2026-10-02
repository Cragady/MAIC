#include "audit_trail.hpp"
#include "daemon.hpp"
#include "nvim_host.hpp"
#include "tui.hpp"
#include "maic/agent.hpp"
#include "maic/engine.hpp"
#include "maic/lua.hpp"
#include "maic/protocol.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// `maic --rpc` (docs/design/engine-protocol.md, section 1 and build step 8): one engine, its parent the one local
// client, JSON-RPC 2.0 one message per line on stdin and stdout. Each request runs on its own thread, so a long one
// (`!cmd`, a `:` command that restarts a server) never holds up a cancel; a writer thread sends what the engine
// queues. Diagnostics go to stderr, and so does anything else in the process that prints: the protocol has its
// own copies of fd 0 and 1.

namespace maic {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxLine = 1 << 20;  // a message is at most 1 MiB (section 1)
constexpr int kInFlight = 64;         // requests a client may have running; the next line is read when one ends

int signal_fds[2] = {-1, -1};  // `maic --rpc`: SIGINT, SIGTERM or SIGHUP ends the read loop

void on_signal(int) {
    char c = 0;
    (void)!write(signal_fds[1], &c, 1);
}

// The connection's output. One message per line, whole, in the order the lock is taken.
class Out {
public:
    Out(int fd, std::string client, const std::filesystem::path& record) : fd_(fd), client_(std::move(client)) {
        if (!record.empty()) recorder_ = std::make_unique<protocol::Recorder>(record);
    }

    std::mutex mu;  // held while writing; a subscribing request holds it from its call to its answer

    void record_in(const json& msg) {
        std::lock_guard lock(mu);
        record("in", msg);
    }

    // Under mu. False once the reader has gone.
    bool write_locked(const json& msg, bool recorded = true) {
        if (recorded) record("out", msg);
        if (broken_) return false;
        std::string line = msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
        for (size_t at = 0; at < line.size();) {
            ssize_t n = ::send(fd_, line.data() + at, line.size() - at, MSG_NOSIGNAL);
            if (n < 0 && errno == ENOTSOCK) n = ::write(fd_, line.data() + at, line.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                broken_ = true;
                return false;
            }
            at += static_cast<size_t>(n);
        }
        return true;
    }

    bool write(const json& msg, bool recorded = true) {
        std::lock_guard lock(mu);
        return write_locked(msg, recorded);
    }

private:
    void record(const char* dir, const json& msg) {
        if (!recorder_) return;
        if (auto v = recorder_->add(dir, client_, msg)) fprintf(stderr, "maic: protocol: %s\n", protocol::describe(*v).c_str());
    }

    int fd_;
    std::string client_;
    bool broken_ = false;
    std::unique_ptr<protocol::Recorder> recorder_;
};

// An answer to a line that never became a request: no id to answer, so it goes out unrecorded.
json fault(int code, const std::string& data_code, const std::string& message) {
    json data = {{"type", "invalid_request_error"}, {"code", data_code.empty() ? json() : json(data_code)}, {"message", message}, {"param", nullptr}};
    return {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", code}, {"message", message}, {"data", data}}}};
}

// Calls whose answer must reach the client before the replay they queue: the checker, and any client, joins a
// session's stream at the answer.
bool subscribes(const json& msg) {
    std::string method = msg.is_object() ? msg.value("method", "") : "";
    return method == "maic.session.attach" || method == "maic.session.subscribe";
}

}  // namespace

// Each request runs on its own thread, so a long one (`!cmd`, a `:` command that restarts a server) never holds up
// a cancel; a writer thread sends what the engine queues.
bool serve_lines(Engine& engine, const std::string& client, int in, int out_fd, int stop_fd, const std::filesystem::path& record,
                 const std::function<void()>& ended) {
    Out out(out_fd, client, record);
    int wake[2];
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) return true;
    auto wake_reader = [&] {
        char c = 0;
        (void)!write(wake[1], &c, 1);
    };

    std::atomic<bool> faulted{false};  // the connection ended for a fault rather than at EOF or a signal
    std::atomic<bool> stop{false};  // the read loop has ended: the writer sends what is queued, then ends
    std::thread writer([&] {
        for (;;) {
            std::vector<json> msgs;
            try {
                msgs = engine.take(client, std::chrono::milliseconds(200));
            } catch (const std::exception&) {
                return;  // disconnected
            }
            std::lock_guard lock(out.mu);
            for (const auto& m : msgs) {
                if (!out.write_locked(m)) {
                    faulted = true;
                    wake_reader();
                    return;
                }
            }
            if (!msgs.empty()) continue;
            if (std::string why = engine.closed(client); !why.empty()) {
                if (why != "maic_shutdown" && !stop) faulted = true;  // maic_too_slow, maic_too_large
                wake_reader();
                return;
            }
            if (stop) return;  // the reader has ended and what was queued has gone out
        }
    });

    std::mutex flight_mu;
    std::condition_variable flight_cv;
    int in_flight = 0;
    auto run = [&](json msg) {
        out.record_in(msg);
        json reply;
        if (subscribes(msg)) {
            std::lock_guard lock(out.mu);
            reply = engine.call(client, msg);
            if (!reply.is_null()) out.write_locked(reply);
        } else {
            reply = engine.call(client, msg);
            if (!reply.is_null()) out.write(reply);
        }
        if (!engine.closed(client).empty()) wake_reader();
    };

    std::string buf;
    char chunk[65536];
    bool reading = true;
    while (reading) {
        pollfd fds[3] = {{in, POLLIN, 0}, {wake[0], POLLIN, 0}, {stop_fd, POLLIN, 0}};
        if (poll(fds, stop_fd >= 0 ? 3 : 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents || (stop_fd >= 0 && fds[2].revents)) break;
        ssize_t n = read(in, chunk, sizeof chunk);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t start = 0;
        for (size_t nl; reading && (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view line(buf.data() + start, nl - start);
            if (line.find_first_not_of(" \t\r") == std::string_view::npos) continue;
            if (line.size() > kMaxLine) {
                reading = false;
                break;
            }
            json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded()) {
                out.write(fault(-32700, "", "not JSON: each line is one JSON-RPC 2.0 message"), false);
                continue;
            }
            if (msg.is_object() && msg.value("method", "") == "maic.hello") {
                run(std::move(msg));  // in line, so whatever the client sends after it finds the hello done
                continue;
            }
            {
                std::unique_lock lock(flight_mu);
                flight_cv.wait(lock, [&] { return in_flight < kInFlight; });
                ++in_flight;
            }
            std::thread([&, msg = std::move(msg)]() mutable {
                run(std::move(msg));
                std::lock_guard lock(flight_mu);
                --in_flight;
                flight_cv.notify_all();
            }).detach();
        }
        buf.erase(0, start);
        if (reading && buf.size() > kMaxLine) reading = false;
        if (!reading) {
            faulted = true;
            out.write(fault(-32000, "maic_too_large", "a message is at most 1 MiB"), false);
        }
    }

    ended();
    {
        std::unique_lock lock(flight_mu);
        flight_cv.wait(lock, [&] { return in_flight == 0; });
    }
    stop = true;
    writer.join();
    close(wake[0]);
    close(wake[1]);
    return faulted;
}

int run_rpc(const TuiOptions& options) {
    // The protocol's own descriptors; fd 0 becomes /dev/null and fd 1 stderr, so a stray print or a child can
    // neither read a request nor corrupt the stream.
    int in = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    int out_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (in < 0 || out_fd < 0) {
        perror("maic --rpc");
        return 1;
    }
    if (int null = open("/dev/null", O_RDONLY | O_CLOEXEC); null >= 0) {
        dup2(null, STDIN_FILENO);
        close(null);
    }
    dup2(STDERR_FILENO, STDOUT_FILENO);

    // As the TUI starts, without its questions: trust is never asked here (stdin is the protocol).
    const char* bare_env = std::getenv("MAIC_BARE");
    bool bare = options.bare || (bare_env && std::string(bare_env) == "1");
    std::string host_refused;
    std::shared_ptr<HostNvim> host = bare ? nullptr : HostNvim::from_env(host_refused);
    set_lua_nvim_host(host);
    std::filesystem::path ws = std::filesystem::current_path();
    for (const auto& n : trust_notices(ws)) fprintf(stderr, "※ %s\n", n.c_str());
    for (const auto& n : settle_trust(ws)) fprintf(stderr, "※ %s\n", n.c_str());
    Settings settings = tui_settings(options, ws);
    for (const auto& w : settings.warnings) fprintf(stderr, "※ %s\n", w.c_str());
    // A running daemon takes this interface as one of its clients (its sessions outlive the interface), unless a flag
    // asks for what only an engine of this process gives.
    if (settings.daemon == "attach" && not_for_daemon(options, false).empty()) {
        if (int sock = daemon_connect(); sock >= 0) {
            set_lua_nvim_host(nullptr);
            int rc = bridge_to_daemon(sock, in, out_fd);
            close(in);
            close(out_fd);
            return rc;
        }
    }
    if (!host_refused.empty()) fprintf(stderr, "※ %s\n", host_refused.c_str());
    if (settings.bare) {
        set_lua_nvim_host(nullptr);
        host.reset();
    }
    if (settings.harness != "smart" && settings.harness != "dumb") {
        fprintf(stderr, "maic: --harness must be smart or dumb\n");
        return 2;
    }
    if (settings.tripwire == "isolated" && !settings.allow_isolated) {
        fprintf(stderr, "maic: tripwire = \"isolated\" (opting out of the machine lock) is not allowed: set allow_isolated = true in settings to permit it\n");
        return 2;
    }
    if (!parse_mode(settings.mode)) {
        fprintf(stderr, "maic: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    set_tripwire_scope(settings.tripwire, runtime_sessions_dir() / ("rpc-" + std::to_string(getpid()) + ".tripped"));
    audit_gate(settings);
    if (options.ctx) {
        if (std::string r = restart_llamacpp_if_changed(); !r.empty()) fprintf(stderr, "※ %s\n", r.c_str());
    }
    if (options.ctx2) {
        if (std::string r = restart_llamacpp_if_changed("llamacpp-2"); !r.empty()) fprintf(stderr, "※ %s\n", r.c_str());
    }

    EngineOptions eo;
    eo.settings = settings;
    eo.tier = settings.protocol_tier;
    eo.mode_asked = options.mode.has_value();
    eo.kind = "rpc";
    eo.titles = true;
    eo.settings_at = [&options](const std::filesystem::path& dir) { return tui_settings(options, dir); };
    eo.setup = [host](Agent& agent, const Settings& st) {
        configure_agent(agent, st);
        if (st.tripwire == "isolated") agent.set_confined(true);
        agent.reload_instructions();
        agent.set_nvim_host(host);
    };
    Engine engine(std::move(eo));
    std::string client = engine.connect(Origin::Local, "stdio", "stdio");

    if (pipe2(signal_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        perror("maic --rpc");
        return 1;
    }
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    // MAIC_PROTOCOL_RECORD=DIR keeps the exchange for `maic protocol check`, as the TUI and maic -p do.
    std::filesystem::path record;
    if (const char* dir = std::getenv("MAIC_PROTOCOL_RECORD"); dir && *dir) record = std::filesystem::path(dir) / ("rpc-" + std::to_string(getpid()) + ".jsonl");
    // Stdin closed, a signal, or the connection ended: running turns are interrupted and the sessions parked, as
    // quitting the TUI does, then the last events go out.
    bool faulted = serve_lines(engine, client, in, out_fd, signal_fds[0], record, [&] { engine.shutdown(); });
    set_lua_nvim_host(nullptr);
    close(in);
    close(out_fd);
    return faulted ? 1 : 0;
}

}  // namespace maic
