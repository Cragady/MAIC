#include "daemon.hpp"

#include "audit_trail.hpp"
#include "tui.hpp"
#include "maid/agent.hpp"
#include "maid/audit_trail.hpp"
#include "maid/engine.hpp"
#include "maid/paths.hpp"
#include "maid/session.hpp"
#include "maid/settings.hpp"
#include "maid/tripwire.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <list>
#include <optional>
#include <sstream>

// The daemon (docs/design/engine-protocol.md, section 8 and build step 13; docs/daemon.md): one engine behind a
// Unix socket that owns sessions, so a session outlives the window it started in. The TUI and maid.nvim attach to
// it when it answers and run their own engine when it does not. MAID keeps its PID with the process's start time,
// as it keeps its services' (MAID owns the PID, not systemd), and a lock file makes it the only one.

namespace maid {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// The runtime directory the socket, the PID and the lock live in, made 0700 and refused when it is not this
// user's own directory.
fs::path daemon_dir() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    return rt && *rt ? fs::path(rt) / "maid" : state_dir() / "run";
}

void secure_dir(const fs::path& dir) {
    fs::create_directories(dir);
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        throw std::runtime_error(dir.string() + " is not a directory of yours; the daemon will not listen there");
    }
    if (chmod(dir.c_str(), 0700) != 0) throw std::runtime_error("cannot make " + dir.string() + " private: " + std::strerror(errno));
}

fs::path pid_file() { return daemon_dir() / "engine.pid"; }
fs::path log_file() { return state_dir() / "engine" / "daemon.log"; }

// /proc/<pid>/stat field 22: with the PID, a process's identity (PIDs are reused).
unsigned long long start_time(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return 0;
    size_t close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream rest(line.substr(close + 2));
    std::string f;
    for (int field = 3; rest >> f; ++field) {
        if (field == 3 && f == "Z") return 0;  // exited, waiting for its parent
        if (field == 22) return std::stoull(f);
    }
    return 0;
}

// The daemon's PID when the file names a process that is still the one that wrote it, else 0.
pid_t running_pid() {
    std::ifstream in(pid_file());
    pid_t pid = 0;
    unsigned long long started = 0;
    if (!(in >> pid >> started) || pid <= 0) return 0;
    return start_time(pid) == started ? pid : 0;
}

sockaddr_un socket_address(const fs::path& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.string().size() >= sizeof addr.sun_path) throw std::runtime_error("the socket path is too long: " + path.string());
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    return addr;
}

uid_t peer_uid(int fd) {
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return static_cast<uid_t>(-1);
    return cred.uid;
}

int signal_fds[2] = {-1, -1};

void on_signal(int) {
    char c = 0;
    (void)!write(signal_fds[1], &c, 1);
}

json request(const std::string& method, json params = json::object()) {
    static int next = 0;
    return {{"jsonrpc", "2.0"}, {"id", "daemon-" + std::to_string(++next)}, {"method", method}, {"params", std::move(params)}};
}

// `maid daemon run`: the engine on the socket, in the foreground, until SIGTERM, SIGINT or SIGHUP.
int run_daemon() {
    fs::path dir = daemon_dir();
    secure_dir(dir);
    int lock = open((dir / "engine.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "maid daemon: already running" << (running_pid() ? " (pid " + std::to_string(running_pid()) + ")" : "") << "\n";
        return 1;
    }
    // Holding the lock, no other daemon owns the socket: one left there was a daemon's that did not end cleanly.
    fs::path sock = dir / "engine.sock";
    std::error_code ec;
    fs::remove(sock, ec);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr = socket_address(sock);
    mode_t old_mask = umask(077);
    int bound = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    umask(old_mask);
    if (listener < 0 || bound != 0 || chmod(sock.c_str(), 0600) != 0 || listen(listener, 16) != 0) {
        std::cerr << "maid daemon: cannot listen on " << sock.string() << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    {
        fs::path tmp = pid_file();
        tmp += ".tmp";
        std::ofstream(tmp, std::ios::trunc) << getpid() << ' ' << start_time(getpid()) << '\n';
        fs::rename(tmp, pid_file());
    }

    // A daemon is no nvim's child: its sessions get no host connection (open question 8).
    unsetenv("NVIM");
    fs::path home = std::getenv("HOME") ? fs::path(std::getenv("HOME")) : fs::current_path();
    fs::current_path(home, ec);
    Settings settings = tui_settings(TuiOptions{}, home);
    for (const auto& w : settings.warnings) std::cerr << "※ " << w << "\n";
    set_tripwire_scope(settings.tripwire, runtime_sessions_dir() / ("daemon-" + std::to_string(getpid()) + ".tripped"));
    audit_gate(settings);

    EngineOptions eo;
    eo.settings = settings;
    eo.tier = settings.protocol_tier;
    eo.kind = "daemon";
    eo.titles = true;
    eo.index_file = state_dir() / "engine" / "index.json";
    eo.keeps_sessions = true;
    // Each session reads the settings files of the directory it works in, as a start there would.
    eo.settings_for = [](const fs::path& ws) { return tui_settings(TuiOptions{}, ws); };
    eo.settings_at = eo.settings_for;
    eo.setup = [](Agent& agent, const Settings& st) {
        configure_agent(agent, st);
        if (st.tripwire == "isolated") agent.set_confined(true);
        agent.reload_instructions();
    };
    Engine engine(std::move(eo));

    if (pipe2(signal_fds, O_CLOEXEC | O_NONBLOCK) != 0) return 1;
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    std::cerr << "maid daemon " MAID_VERSION ": listening on " << sock.string() << " (pid " << getpid() << ")\n" << std::flush;

    const char* record_dir = std::getenv("MAID_PROTOCOL_RECORD");
    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    std::list<Connection> connections;
    for (;;) {
        pollfd fds[2] = {{listener, POLLIN, 0}, {signal_fds[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        // Only this user: the directory is 0700 and the socket 0600, and the peer's credentials are checked too.
        if (peer_uid(fd) != getuid()) {
            close(fd);
            continue;
        }
        for (auto it = connections.begin(); it != connections.end();) {
            if (!*it->done) {
                ++it;
                continue;
            }
            it->thread.join();
            it = connections.erase(it);
        }
        Connection& c = connections.emplace_back();
        c.thread = std::thread([&engine, fd, done = c.done, record_dir] {
            std::string client = engine.connect(Origin::Local, "socket", "socket");
            fs::path record;
            if (record_dir && *record_dir) record = fs::path(record_dir) / ("daemon-" + std::to_string(getpid()) + "-" + client + ".jsonl");
            serve_lines(engine, client, fd, fd, -1, record, [&] { engine.leave(client); });
            engine.disconnect(client);
            close(fd);
            *done = true;
        });
    }

    // Stopped: no new connections; the sessions are parked (their turns interrupted, each resumes where it stopped)
    // and every connection ends with what was queued for it.
    std::cerr << "maid daemon: stopping\n" << std::flush;
    close(listener);
    fs::remove(sock, ec);
    engine.shutdown();
    for (auto& c : connections) c.thread.join();
    fs::remove(pid_file(), ec);
    std::cerr << "maid daemon: stopped\n" << std::flush;
    close(lock);
    return 0;
}

// `maid daemon start`: `maid daemon run` detached, its output in <state>/engine/daemon.log; returns once it answers.
int start_daemon() {
    if (int fd = daemon_connect(); fd >= 0) {
        close(fd);
        std::cout << "maid daemon: already running (pid " << running_pid() << ")\n";
        return 0;
    }
    std::string exe = self_exe();
    if (exe.empty()) throw std::runtime_error("cannot find this maid binary to start the daemon");
    fs::create_directories(log_file().parent_path());
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDONLY);
        int log = open(log_file().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (null >= 0) dup2(null, STDIN_FILENO);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        execl(exe.c_str(), "maid", "daemon", "run", static_cast<char*>(nullptr));
        _exit(127);
    }
    for (auto until = std::chrono::steady_clock::now() + 30s; std::chrono::steady_clock::now() < until; std::this_thread::sleep_for(50ms)) {
        if (int fd = daemon_connect(); fd >= 0) {
            close(fd);
            std::cout << "maid daemon: running (pid " << pid << "), socket " << daemon_socket().string() << "\n"
                      << "the TUI and maid.nvim open their sessions there now (daemon = \"off\" in settings keeps them in their own process)\n";
            return 0;
        }
        if (waitpid(pid, nullptr, WNOHANG) == pid) break;
    }
    std::cerr << "maid daemon: did not start; the end of " << log_file().string() << ":\n";
    std::ifstream in(log_file());
    std::deque<std::string> tail;
    for (std::string l; std::getline(in, l);) {
        tail.push_back(l);
        if (tail.size() > 15) tail.pop_front();
    }
    for (const auto& l : tail) std::cerr << "  " << l << "\n";
    return 1;
}

// The daemon's sessions with a turn under way (or waiting for a person), from its index.
std::vector<json> working(DaemonClient& d) {
    std::vector<json> out;
    json r = d.call(request("maid.index.get"));
    for (const auto& e : r.value("result", json::object()).value("entries", json::array())) {
        if (e.value("state", "") != "parked" && e.value("activity", "idle") != "idle") out.push_back(e);
    }
    return out;
}

// `maid daemon stop`: asks first when a turn is running, since stopping interrupts it (it resumes where it stopped).
int stop_daemon(bool yes) {
    pid_t pid = running_pid();
    if (auto d = DaemonClient::connect()) {
        d->call(request("maid.hello", {{"protocol", 1}, {"client", {{"name", "maid daemon stop"}, {"version", MAID_VERSION}}}}));
        auto busy = working(*d);
        d->close();
        if (!busy.empty() && !yes) {
            std::cerr << busy.size() << (busy.size() == 1 ? " session is" : " sessions are") << " working in the daemon:\n";
            for (const auto& e : busy) {
                std::string title = e.value("title", "");
                std::cerr << "  " << (title.empty() ? e.value("id", "") : title) << "  (" << e.value("activity", "") << ")\n";
            }
            std::cerr << "stopping interrupts them; each is parked and resumes where it stopped. ";
            if (!isatty(STDIN_FILENO)) {
                std::cerr << "maid daemon stop --yes stops it anyway\n";
                return 1;
            }
            std::cerr << "Stop the daemon? [y/N] " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y" && answer != "yes") {
                std::cerr << "left running\n";
                return 1;
            }
        }
    }
    if (!pid) {
        std::cout << "maid daemon: not running\n";
        return 0;
    }
    kill(pid, SIGTERM);
    for (auto until = std::chrono::steady_clock::now() + 60s; std::chrono::steady_clock::now() < until; std::this_thread::sleep_for(50ms)) {
        if (!running_pid()) {
            std::cout << "maid daemon: stopped; its sessions are parked (maid -r, or :switch in MAID, resumes one)\n";
            return 0;
        }
    }
    std::cerr << "maid daemon: pid " << pid << " did not stop within a minute\n";
    return 1;
}

std::string ago(const std::string& when) {
    return when.empty() ? "" : "  " + when;
}

// `maid daemon status`: 0 when it runs, 3 when it does not (systemctl's convention).
int status_daemon(bool as_json, bool text_base) {
    auto d = DaemonClient::connect();
    if (!d) {
        bool left = fs::exists(daemon_socket());
        if (as_json) {
            std::cout << json{{"running", false}, {"socket", daemon_socket().string()}, {"stale_socket", left}}.dump(2) << "\n";
        } else if (text_base) {
            std::cout << "daemon\tstopped\t-\t" << daemon_socket().string() << "\n";
        } else {
            std::cout << "maid daemon: not running" << (left ? " (a socket was left at " + daemon_socket().string() + "; maid daemon start clears it)" : "")
                      << "\nmaid daemon start runs it\n";
        }
        return 3;
    }
    json hello = d->call(request("maid.hello", {{"protocol", 1}, {"client", {{"name", "maid daemon status"}, {"version", MAID_VERSION}}}})).value("result", json::object());
    json st = d->call(request("maid.engine.status")).value("result", json::object());
    json entries = d->call(request("maid.index.get")).value("result", json::object()).value("entries", json::array());
    d->close();
    pid_t pid = running_pid();
    if (as_json) {
        std::cout << json{{"running", true}, {"pid", pid}, {"socket", daemon_socket().string()}, {"engine", hello.value("engine", json::object())},
                          {"tier", st.value("tier", "")}, {"sessions", entries}}.dump(2) << "\n";
        return 0;
    }
    if (text_base) {
        std::cout << "daemon\trunning\t" << pid << "\t" << daemon_socket().string() << "\n";
        for (const auto& e : entries) {
            std::string state = e.value("state", "");
            std::string doing = state == "parked" ? "parked" : e.value("activity", "idle") != "idle" ? e.value("activity", "") : e.value("unseen", false) ? "finished" : "idle";
            std::string workspace = e.value("workspace", "");
            std::cout << "session\t" << doing << "\t" << (state == "live" ? "focus" : "-") << "\t" << e.value("id", "") << "\t" << (workspace.empty() ? "-" : workspace) << "\n";
        }
        return 0;
    }
    std::cout << "maid daemon: running (pid " << pid << ", maid " << hello.value("engine", json::object()).value("version", "?") << "), socket " << daemon_socket().string()
              << "\nlog: " << log_file().string() << "\n";
    if (entries.empty()) {
        std::cout << "no sessions\n";
        return 0;
    }
    std::cout << "sessions:\n";
    for (const auto& e : entries) {
        std::string state = e.value("state", "");
        std::string doing = state == "parked" ? "parked" : e.value("activity", "idle") != "idle" ? e.value("activity", "") : e.value("unseen", false) ? "finished" : "idle";
        std::string title = e.value("title", "");
        std::cout << "  " << doing << (state == "live" ? " (in focus)" : "") << "  " << (title.empty() ? e.value("id", "") : title) << "  " << e.value("workspace", "")
                  << ago(e.value("last_activity", "")) << "\n";
    }
    return 0;
}

// `maid daemon unit [install|remove]`: the systemd user unit from contrib/systemd/, printed, written or taken away.
// Installing writes it only; enabling it (start at login) is a command the user runs.
int unit_daemon(const std::string& what) {
    fs::path templ = root_dir() / "contrib" / "systemd" / "maid-daemon.service";
    std::ifstream in(templ);
    if (!in) throw std::runtime_error("no " + templ.string());
    std::string maid = find_on_path("maid");
    if (maid.empty()) maid = self_exe();
    std::string text = render_unit(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "", maid, "");
    fs::path target = systemd_user_dir() / "maid-daemon.service";
    std::string systemctl = find_on_path("systemctl");
    if (what.empty()) {
        std::cout << text;
        return 0;
    }
    if (what == "install") {
        fs::create_directories(target.parent_path());
        std::ofstream(target, std::ios::trunc) << text;
        if (!systemctl.empty()) run({systemctl, "--user", "daemon-reload"}, nullptr);
        std::cout << "wrote " << target.string() << " (maid daemon run, from " << maid << ")\n"
                  << "start it now and at each login with: systemctl --user enable --now maid-daemon.service\n";
        return 0;
    }
    if (what == "remove") {
        if (!fs::exists(target)) {
            std::cout << "no " << target.string() << "\n";
            return 0;
        }
        if (!systemctl.empty()) run({systemctl, "--user", "disable", "--now", "maid-daemon.service"}, nullptr);
        fs::remove(target);
        if (!systemctl.empty()) run({systemctl, "--user", "daemon-reload"}, nullptr);
        std::cout << "removed " << target.string() << "; a daemon it was running is stopped and its sessions parked\n";
        return 0;
    }
    throw std::runtime_error("maid daemon unit [install|remove]");
}

}  // namespace

fs::path daemon_socket() {
    return daemon_dir() / "engine.sock";
}

int daemon_connect() {
    fs::path sock = daemon_socket();
    if (!fs::exists(sock)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr = socket_address(sock);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || peer_uid(fd) != getuid()) {
        close(fd);
        return -1;
    }
    return fd;
}

std::string not_for_daemon(const TuiOptions& o, bool model_and_mode) {
    if (!model_and_mode && o.model) return "--model";
    if (!model_and_mode && o.mode) return "--mode";
    if (!o.context.empty()) return "--context";
    if (o.system) return "--system";
    if (o.prefill) return "--prefix";
    if (o.ctx) return "--ctx";
    if (o.ctx2) return "--ctx2";
    if (!o.rules.empty()) return "--rule";
    if (o.load_instructions) return "--no-instructions";
    if (!o.bans.empty() || !o.ban_patterns.empty()) return "--ban";
    if (!o.sampling.empty()) return "--sampling";
    if (o.harness) return "--harness";
    if (o.accept_dumb_auto) return "--accept-dumb-auto";
    if (o.record) return *o.record ? "--record" : "--no-record";
    if (o.fork_at) return "--fork-at";
    if (o.resume && !o.append) return "--no-append";
    if (o.trust) return "--trust";
    return "";
}

// ---------- the client side ----------

std::unique_ptr<DaemonClient> DaemonClient::connect(std::function<void()> wake) {
    int fd = daemon_connect();
    if (fd < 0) return nullptr;
    return std::unique_ptr<DaemonClient>(new DaemonClient(fd, std::move(wake)));
}

DaemonClient::DaemonClient(int fd, std::function<void()> wake) : fd_(fd), wake_(std::move(wake)) {
    reader_ = std::thread([this] { read_loop(); });
}

DaemonClient::~DaemonClient() {
    close();
}

void DaemonClient::read_loop() {
    std::string buf;
    char chunk[65536];
    for (;;) {
        ssize_t n = read(fd_, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t start = 0;
        bool unasked = false;
        for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1) {
            json msg = json::parse(std::string_view(buf.data() + start, nl - start), nullptr, false);
            if (!msg.is_object()) continue;
            std::lock_guard lock(mu_);
            if (msg.contains("method") || !msg.contains("id") || msg["id"].is_null()) {
                queue_.push_back(std::move(msg));
                unasked = true;
            } else {
                std::string id = msg["id"].dump();  // before the move: json's operator= takes its argument first
                answers_[id] = std::move(msg);
            }
        }
        buf.erase(0, start);
        cv_.notify_all();
        if (unasked && wake_) wake_();
    }
    gone_ = true;
    cv_.notify_all();
    if (wake_) wake_();
}

json DaemonClient::call(const json& message) {
    std::string id = message.value("id", json()).dump();
    std::string line = message.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    {
        std::lock_guard lock(write_mu_);
        for (size_t at = 0; !gone_ && at < line.size();) {
            ssize_t n = send(fd_, line.data() + at, line.size() - at, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += static_cast<size_t>(n);
        }
    }
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return answers_.count(id) || gone_; });
    if (auto it = answers_.find(id); it != answers_.end()) {
        json reply = std::move(it->second);
        answers_.erase(it);
        return reply;
    }
    return {{"jsonrpc", "2.0"}, {"id", message.value("id", json())}, {"error", {{"code", -32000}, {"message", "the daemon has gone (maid daemon status)"}}}};
}

std::vector<json> DaemonClient::take() {
    std::lock_guard lock(mu_);
    std::vector<json> out(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    queue_.clear();
    return out;
}

void DaemonClient::close() {
    if (fd_ < 0) return;
    shutdown(fd_, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    ::close(fd_);
    fd_ = -1;
}

int bridge_to_daemon(int sock, int in, int out) {
    // Two directions, each until its side closes. Stdin closing ends the connection (the daemon lets go of the
    // session in this client's focus); the daemon going away ends this process, so its interface sees the exit.
    int gone[2];
    if (pipe2(gone, O_CLOEXEC) != 0) return 1;
    std::thread down([&] {
        char buf[65536];
        for (ssize_t n; (n = read(sock, buf, sizeof buf)) != 0;) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) break;
            bool ok = true;
            for (ssize_t at = 0; ok && at < n;) {
                ssize_t w = write(out, buf + at, static_cast<size_t>(n - at));
                if (w < 0 && errno == EINTR) continue;
                ok = w > 0;
                at += ok ? w : 0;
            }
            if (!ok) break;
        }
        char c = 0;
        (void)!write(gone[1], &c, 1);
    });
    char buf[65536];
    for (;;) {
        pollfd fds[2] = {{in, POLLIN, 0}, {gone[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || send(sock, buf, static_cast<size_t>(n), MSG_NOSIGNAL) != n) break;
    }
    shutdown(sock, SHUT_RDWR);
    down.join();
    ::close(gone[0]);
    ::close(gone[1]);
    ::close(sock);
    return 0;
}

int cmd_daemon(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "status" : args[0];
    auto has = [&](const char* flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
    if (sub == "run") return run_daemon();
    if (sub == "start") return start_daemon();
    if (sub == "stop") return stop_daemon(has("--yes") || has("-y"));
    if (sub == "status") return status_daemon(has("--json"), has("--text-base"));
    if (sub == "unit") return unit_daemon(args.size() > 1 ? args[1] : "");
    throw std::runtime_error("maid daemon start | stop [--yes] | status [--json|--text-base] | run | unit [install|remove]");
}

// ---------- maid liaison ----------

// Another agent's client of the daemon (docs/daemon.md, "The liaison"): ordinary client input on a session the
// daemon holds, never a focus of its own, so a window attached to the session keeps it and leaving changes nothing.

namespace {

const char* kLiaisonUsage =
    "maid liaison send ID (TEXT | --file FILE) [--out FILE] [--timeout SECONDS] | approve ID APPROVAL yes|no | status ID";

std::unique_ptr<DaemonClient> liaison_connect(std::function<void()> wake = {}) {
    auto d = DaemonClient::connect(std::move(wake));
    if (!d) return nullptr;
    d->call(request("maid.hello", {{"protocol", 1},
                                   {"client", {{"name", "liaison"}, {"version", MAID_VERSION}}},
                                   {"exclude", {"response.output_text.delta", "response.reasoning_text.delta"}}}));
    return d;
}

int no_daemon() {
    std::cerr << "maid liaison: no daemon is running (maid daemon start runs one)\n";
    return 3;
}

// The daemon's index entry for `id`, or a unique prefix of one; null when there is none.
json liaison_entry(DaemonClient& d, const std::string& id) {
    json found;
    int matches = 0;
    for (const auto& e : d.call(request("maid.index.get")).value("result", json::object()).value("entries", json::array())) {
        std::string eid = e.value("id", "");
        if (eid == id) return e;
        if (eid.rfind(id, 0) == 0) {
            found = e;
            ++matches;
        }
    }
    return matches == 1 ? found : json();
}

std::string one_line(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::string error_of(const json& reply) {
    return reply.value("error", json::object()).value("message", "the daemon refused it");
}

int liaison_send(const std::vector<std::string>& args) {
    std::string id, text, out;
    std::optional<std::string> file;
    long timeout = 600;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if ((a == "--file" || a == "--out" || a == "--timeout") && i + 1 >= args.size()) throw std::runtime_error(a + " takes a value");
        if (a == "--file") {
            file = args[++i];
        } else if (a == "--out") {
            out = args[++i];
        } else if (a == "--timeout") {
            timeout = std::stol(args[++i]);
        } else if (id.empty()) {
            id = a;
        } else if (text.empty()) {
            text = a;
        } else {
            throw std::runtime_error(kLiaisonUsage);
        }
    }
    if (id.empty() || text.empty() == !file) throw std::runtime_error(kLiaisonUsage);
    if (file) {
        std::ifstream in(*file == "-" ? "/dev/stdin" : *file);
        if (!in) throw std::runtime_error("cannot read " + *file);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.empty()) throw std::runtime_error("the input is empty");

    std::mutex mu;
    std::condition_variable cv;
    bool woke = false;
    auto d = liaison_connect([&] {
        std::lock_guard lock(mu);
        woke = true;
        cv.notify_all();
    });
    if (!d) return no_daemon();
    // Loaded (a parked one is resumed) without moving any client's focus, then followed from here on.
    json res = d->call(request("maid.session.resume", {{"session", id}, {"focus", false}}));
    if (!res.contains("result")) {
        std::cerr << "maid liaison: " << error_of(res) << "\n";
        return 1;
    }
    std::string sid = res["result"].value("id", id);
    json att = d->call(request("maid.session.attach", {{"session", sid}, {"exchanges", 0}}));
    if (!att.contains("result")) {
        std::cerr << "maid liaison: " << error_of(att) << "\n";
        return 1;
    }
    json created = d->call(request("response.create", {{"conversation", sid}, {"input", text}}));
    if (!created.contains("result")) {
        std::cerr << "maid liaison: " << error_of(created) << "\n";
        return 2;
    }
    long turn = created["result"].value("maid", json::object()).value("turn", 0L);

    // A turn can span several responses (steers make successors): the reply is the last assistant message of the
    // turn, and it ends on a terminal event marked final. An approval counts only once this turn has started.
    bool ours = false;
    std::string reply;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        for (const auto& m : d->take()) {
            if (m.value("method", "") != "maid.event") continue;
            const json& e = m["params"];
            if (e.value("stream_id", "") != sid) continue;
            std::string type = e.value("type", "");
            json resp = e.value("response", json::object());
            json rmaid = resp.value("maid", json::object());
            bool this_turn = rmaid.value("turn", -1L) == turn;
            if (type == "response.created" && this_turn) ours = true;
            if (type == "maid.approval.requested" && ours) {
                std::cerr << "approval\t" << e.value("id", "") << "\t" << e.value("tool", "") << "\t" << one_line(e.value("summary", "")) << "\n";
                return 4;
            }
            bool terminal = type == "response.completed" || type == "response.failed" || type == "response.incomplete" || type == "maid.response.cancelled";
            if (!terminal || !this_turn) continue;
            for (const auto& item : resp.value("output", json::array())) {
                if (item.value("type", "") != "message" || item.value("role", "") != "assistant") continue;
                if (item.contains("maid") && item["maid"].value("status", "") == "discarded") continue;
                reply.clear();
                for (const auto& part : item.value("content", json::array())) reply += part.value("text", "");
            }
            if (!rmaid.value("final", false)) continue;
            if (type != "response.completed") {
                json err = resp.value("error", json());
                std::string why = err.is_object() ? err.value("message", "") : "";
                std::cerr << "maid liaison: the response " << resp.value("status", "ended") << (why.empty() ? "" : ": " + why) << "\n";
                return 2;
            }
            if (out.empty()) {
                std::cout << reply << (reply.empty() || reply.back() != '\n' ? "\n" : "") << std::flush;
                return 0;
            }
            fs::path partial = out + ".partial";
            {
                std::ofstream f(partial, std::ios::binary | std::ios::trunc);
                f << reply;
                if (!f.flush()) throw std::runtime_error("cannot write " + partial.string());
            }
            fs::rename(partial, out);
            return 0;
        }
        if (d->gone()) {
            std::cerr << "maid liaison: the daemon has gone (maid daemon status)\n";
            return 3;
        }
        std::unique_lock lock(mu);
        if (!cv.wait_until(lock, until, [&] { return woke; })) {
            std::cerr << "maid liaison: no reply within " << timeout << " s; the turn keeps running (maid liaison status " << sid << ")\n";
            return 5;
        }
        woke = false;
    }
}

int liaison_approve(const std::vector<std::string>& args) {
    if (args.size() != 4) throw std::runtime_error(kLiaisonUsage);
    const std::string& choice = args[3];
    if (choice == "always") throw std::runtime_error("the liaison answers yes or no: always is for a person at their own window");
    if (choice != "yes" && choice != "no") throw std::runtime_error(kLiaisonUsage);
    auto d = liaison_connect();
    if (!d) return no_daemon();
    json e = liaison_entry(*d, args[1]);
    if (e.is_null()) {
        std::cerr << "maid liaison: the daemon holds no session " << args[1] << "\n";
        return 1;
    }
    json r = d->call(request("maid.approval.answer", {{"session", e["id"]}, {"approval", args[2]}, {"choice", choice}}));
    if (!r.contains("result")) {
        std::cerr << "maid liaison: " << error_of(r) << "\n";
        return 1;
    }
    std::cout << "answered " << choice << "\n";
    return 0;
}

int liaison_status(const std::vector<std::string>& args) {
    if (args.size() != 2) throw std::runtime_error(kLiaisonUsage);
    auto d = liaison_connect();
    if (!d) return no_daemon();
    json e = liaison_entry(*d, args[1]);
    if (e.is_null()) {
        std::cerr << "maid liaison: the daemon holds no session " << args[1] << "\n";
        return 1;
    }
    json waiting = e.value("waiting", json());
    std::string state = e.value("state", "") == "parked"                                   ? "parked"
                        : e.value("activity", "idle") == "idle"                            ? "idle"
                        : waiting.is_object() && waiting.value("kind", "") == "steer" ? "paused"
                                                                                       : "working";
    std::string approvals;
    if (state != "parked") {
        json snap = d->call(request("maid.session.attach", {{"session", e["id"]}, {"exchanges", 0}})).value("result", json::object());
        for (const auto& a : snap.value("pending", json::array())) approvals += (approvals.empty() ? "" : ",") + a.value("id", "");
    }
    std::cout << state << "\t" << e.value("model", "") << "\tqueued=" << e.value("queued", 0) << "\tapprovals=" << (approvals.empty() ? "-" : approvals) << "\n";
    return 0;
}

}  // namespace

int cmd_liaison(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "" : args[0];
    if (sub == "send") return liaison_send(args);
    if (sub == "approve") return liaison_approve(args);
    if (sub == "status") return liaison_status(args);
    throw std::runtime_error(kLiaisonUsage);
}

}  // namespace maid
