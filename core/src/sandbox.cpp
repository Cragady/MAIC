#include "maic/sandbox.hpp"
#include "maic/paths.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace maic {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

size_t whole_chars(const std::string& s, size_t n) {
    size_t i = n;
    while (i > 0 && n - i < 3 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) --i;
    if (i == 0) return n;
    unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return n - (i - 1) < len ? i - 1 : n;
}

namespace {

constexpr size_t kHeadBytes = kOutputHeadBytes;
constexpr size_t kTailBytes = kOutputTailBytes;
// Streamed output (on_output): a chunk per stream every kChunkEvery or kChunkBytes, whichever comes first; at
// most kQueuedBytes waiting for the consumer; kDrainGrace for it to catch up once the program has ended.
constexpr size_t kChunkBytes = 16 * 1024;
constexpr auto kChunkEvery = std::chrono::milliseconds(100);
constexpr size_t kQueuedBytes = 2 * 1024 * 1024;
constexpr auto kDrainGrace = std::chrono::milliseconds(200);

// The environment a sandboxed program gets, and nothing else: bwrap starts it with --clearenv and sets these
// from MAIC's own environment when they are set, plus every LC_* variable. MAIC sets no variable of its own for
// commands or script tools. Never here: a way out of the sandbox (DBUS_SESSION_BUS_ADDRESS, XDG_RUNTIME_DIR,
// SSH_AUTH_SOCK, GPG_AGENT_INFO, NVIM, DISPLAY, WAYLAND_DISPLAY) or a credential (*_TOKEN, *_KEY, *_SECRET).
// A variable a tool truly needs is added here with the reason (docs/harness.md, Sandboxed commands).
constexpr const char* kPassedEnv[] = {
    "PATH", "HOME", "USER", "LOGNAME", "LANG", "TERM", "TZ", "SHELL",
    // MAIC's own helpers on the default allow list (maic path, maic status, maic sessions) read the session's
    // config and state, not the defaults
    "XDG_CONFIG_HOME", "XDG_STATE_HOME",
};

// Where the system's /run and /nix are: /, or for tests only MAIC_SANDBOX_ROOT under MAIC_TESTING=1, so fake
// NixOS trees in a temporary directory stand in for the real ones.
fs::path system_root() {
    const char* testing = std::getenv("MAIC_TESTING");
    const char* root = std::getenv("MAIC_SANDBOX_ROOT");
    if (testing && std::string_view(testing) == "1" && root && *root) return fs::weakly_canonical(root);
    return "/";
}

// The program trees NixOS keeps under /run, bound back read-only over the empty /run: the system's software,
// its setuid wrappers (inert here: bwrap runs everything with no_new_privs) and the GPU userspace drivers.
constexpr const char* kRunPrograms[] = {"current-system", "booted-system", "wrappers", "opengl-driver", "opengl-driver-32"};

bool inside(const fs::path& p, const fs::path& dir) {
    auto [d, _] = std::mismatch(dir.begin(), dir.end(), p.begin(), p.end());
    return d == dir.end();
}

// Host sockets the sandbox must not reach. Path sockets ignore --unshare-all (only abstract ones live in the
// network namespace), and the read-only bind of / still lets a program connect to one: the session D-Bus in
// $XDG_RUNTIME_DIR can ask systemd to start anything outside the sandbox, the same directory holds nvim's
// socket, Wayland, pipewire and gpg-agent, and /run holds the system bus, docker.sock, libvirt, screen and
// systemd's varlink sockets. So all of /run is an empty tmpfs (removable drives under /run/media are bound back
// read-only), and so are the runtime directory and the directories of the agent sockets the environment names,
// wherever they are. A socket whose directory is $HOME, an ancestor of it or / has /dev/null bound over it instead.
// The workspace is bound in between, writable unless `read_only`.
void bind_workspace_hiding_sockets(std::vector<std::string>& args, const fs::path& workspace, bool read_only) {
    fs::path home = fs::weakly_canonical(std::getenv("HOME")), ws = fs::weakly_canonical(workspace), root = system_root();
    std::vector<fs::path> dirs, files;
    std::error_code ec;
    fs::path sys_run = fs::weakly_canonical(root / "run", ec);
    for (const fs::path& run : {fs::path("/run"), fs::path("/var/run"), sys_run}) {
        fs::path p = fs::canonical(run, ec);
        if (!ec && std::find(dirs.begin(), dirs.end(), p) == dirs.end()) dirs.push_back(p);
    }
    // Whether `p` is already out of sight: inside the workspace only a mask inside it hides anything.
    auto covered = [&](const fs::path& p) {
        auto masks = [&](const fs::path& d) { return inside(p, d) && (!inside(p, ws) || inside(d, ws)); };
        return (inside(p, "/tmp") && !inside(p, ws)) || (std::any_of(dirs.begin(), dirs.end(), masks) && !inside(p, "/run/media"));
    };
    auto hide_dir = [&](const fs::path& p) {
        if (!fs::is_directory(p) || covered(p) || inside(home, p) || p == ws) return false;
        dirs.push_back(p);
        return true;
    };
    // Without a runtime directory the daemon's socket is in <state>/run (docs/daemon.md): out of sight too.
    if (const char* rt = std::getenv("XDG_RUNTIME_DIR"); rt && *rt) hide_dir(fs::weakly_canonical(rt));
    else hide_dir(fs::weakly_canonical(state_dir() / "run", ec));
    for (const char* var : {"SSH_AUTH_SOCK", "GPG_AGENT_INFO", "NVIM"}) {
        const char* v = std::getenv(var);
        if (!v || *v != '/') continue;  // unset, or not a path (NVIM may be host:port, which the network namespace stops)
        std::string given = v;
        if (std::string(var) == "GPG_AGENT_INFO") given = given.substr(0, given.find(':'));  // PATH:PID:1
        fs::path sock = fs::weakly_canonical(given, ec);
        if (ec || !fs::exists(sock) || covered(sock)) continue;
        if (!hide_dir(sock.parent_path())) files.push_back(sock);
    }
    // Before the workspace is bound, so a workspace under one of them is still there; one inside the workspace
    // goes after, or the workspace would cover it again.
    for (const auto& d : dirs) {
        if (inside(d, ws)) continue;
        args.insert(args.end(), {"--tmpfs", d.string()});
        if (d == "/run") args.insert(args.end(), {"--ro-bind-try", "/run/media", "/run/media"});
        if (d == sys_run) {
            // Only these exact paths come back, each followed to its tree; a socket inside one is masked.
            for (const char* name : kRunPrograms) {
                fs::path p = d / name;
                if (!fs::is_directory(p, ec)) continue;
                args.insert(args.end(), {"--ro-bind", fs::canonical(p, ec).string(), p.string()});
                fs::path real = fs::canonical(p, ec);
                for (fs::recursive_directory_iterator it(real, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_socket(ec)) files.push_back(p / it->path().lexically_relative(real));
                }
                ec.clear();
            }
        }
    }
    // The Nix daemon's socket lives outside /run: through it a command could build any derivation, a fixed-output
    // one fetching from the network included. Its directory becomes an empty tmpfs.
    if (fs::path nix = root / "nix" / "var" / "nix" / "daemon-socket"; fs::is_directory(nix, ec) && !inside(fs::weakly_canonical(nix), ws)) {
        args.insert(args.end(), {"--tmpfs", fs::weakly_canonical(nix).string()});
    }
    args.insert(args.end(), {read_only ? "--ro-bind" : "--bind", workspace.string(), workspace.string()});
    for (const auto& d : dirs) {
        if (inside(d, ws)) args.insert(args.end(), {"--tmpfs", d.string()});
    }
    for (const auto& f : files) args.insert(args.end(), {"--ro-bind", "/dev/null", f.string()});
}

// Everything before the program: the mounts, the environment, the namespaces and the working directory.
std::vector<std::string> bwrap_args(const fs::path& workspace, bool read_only, const fs::path& workdir) {
    std::vector<std::string> args = {
        "bwrap",
        "--ro-bind", "/", "/",
        "--dev", "/dev",
        "--proc", "/proc",
        "--tmpfs", "/tmp",
    };
    bind_workspace_hiding_sockets(args, workspace, read_only);
    // Hide secrets behind empty directories. /var/lib/maic stays visible read-only so tools can see the lock.
    fs::path home = std::getenv("HOME");
    for (const char* p : {".ssh", ".gnupg", ".aws", ".kube", ".docker", ".password-store", ".local/share/keyrings", ".ollama"}) {
        if (fs::is_directory(home / p) && !fs::equivalent(home / p, workspace)) {
            args.insert(args.end(), {"--tmpfs", (home / p).string()});
        }
    }
    args.push_back("--clearenv");
    for (const char* name : kPassedEnv) {
        if (const char* v = std::getenv(name)) args.insert(args.end(), {"--setenv", name, v});
    }
    for (char** e = environ; *e; ++e) {
        std::string_view kv = *e;
        size_t eq = kv.find('=');
        if (kv.rfind("LC_", 0) == 0 && eq != std::string_view::npos) {
            args.insert(args.end(), {"--setenv", std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1))});
        }
    }
    args.insert(args.end(), {
        "--unshare-all",       // no network, own PID/IPC/UTS namespaces
        "--die-with-parent",
        "--new-session",       // no TIOCSTI keystroke injection into this terminal
        "--chdir", (workdir.empty() ? workspace : workdir).string(),
        "--",
    });
    return args;
}

// `total` is every byte the program wrote, so the omitted count is right however often absorb cut the middle.
std::string trim_output(std::string out, size_t total) {
    if (total <= kHeadBytes + kTailBytes) {
        return out;
    }
    size_t cut = total - kHeadBytes - kTailBytes;
    return out.substr(0, kHeadBytes) + "\n... [" + std::to_string(cut) + " bytes omitted] ...\n" +
           out.substr(out.size() - kTailBytes);
}

// Keeps memory bounded on runaway output: the head and a rolling tail.
void absorb(std::string& out, size_t& total, const char* buf, size_t n) {
    out.append(buf, n);
    total += n;
    if (out.size() > 4 * (kHeadBytes + kTailBytes)) {
        out.erase(kHeadBytes, out.size() - kHeadBytes - 2 * kTailBytes);
    }
}

// The tee in front of the result: spawn's loop hands it what it read and never waits on `on_output`, which a
// thread of its own calls in order. Past kQueuedBytes waiting, newer chunks are dropped and counted; their
// offsets leave the gap for the consumer to see.
class OutputTee {
public:
    explicit OutputTee(const OnOutput& on_output) : on_output_(on_output), thread_([this] { deliver(); }) {}
    ~OutputTee() {
        if (thread_.joinable()) finish();  // spawn left early by an exception
    }

    void add(OutputStream stream, const char* data, size_t n, Clock::time_point now) {
        Batch& b = batch_[static_cast<int>(stream)];
        if (b.data.empty()) b.since = now;
        b.data.append(data, n);
        while (b.data.size() >= kChunkBytes) send(stream, whole_chars(b.data, kChunkBytes));
    }

    // Sends every batch that has waited kChunkEvery; returns the milliseconds until the next is due, at most `cap`.
    long flush_due(Clock::time_point now, long cap) {
        for (int s = 0; s < 2; ++s) {
            Batch& b = batch_[s];
            if (b.data.empty()) continue;
            auto due = b.since + kChunkEvery;
            if (due <= now) {
                size_t n = whole_chars(b.data, b.data.size());
                if (n) send(static_cast<OutputStream>(s), n);
                if (!b.data.empty()) b.since = now;  // the start of a character: it waits for the rest
            } else cap = std::min<long>(cap, std::chrono::ceil<std::chrono::milliseconds>(due - now).count());
        }
        return cap;
    }

    // The last flush; waits kDrainGrace for the consumer, drops what it has not reached by then, and joins.
    void finish() {
        for (int s = 0; s < 2; ++s) {
            if (!batch_[s].data.empty()) send(static_cast<OutputStream>(s), batch_[s].data.size());
        }
        std::unique_lock lock(mu_);
        closing_ = true;
        cv_.notify_all();
        if (!cv_.wait_for(lock, kDrainGrace, [&] { return chunks_.empty(); })) {
            for (const auto& c : chunks_) {
                ++dropped_chunks;
                dropped_bytes += c.data.size();
            }
            chunks_.clear();
        }
        lock.unlock();
        thread_.join();
    }

    size_t dropped_chunks = 0, dropped_bytes = 0;

private:
    struct Batch {
        std::string data;
        size_t offset = 0;  // where data starts in the stream
        Clock::time_point since;
    };
    struct Chunk {
        OutputStream stream;
        std::string data;
        size_t offset;
    };

    // Moves the batch's first `n` bytes to the queue, or drops them when the consumer is too far behind.
    void send(OutputStream stream, size_t n) {
        Batch& b = batch_[static_cast<int>(stream)];
        Chunk c{stream, b.data.substr(0, n), b.offset};
        b.data.erase(0, n);
        b.offset += n;
        std::lock_guard lock(mu_);
        if (queued_ + n > kQueuedBytes) {
            ++dropped_chunks;
            dropped_bytes += n;
            return;
        }
        queued_ += n;
        chunks_.push_back(std::move(c));
        cv_.notify_all();
    }

    void deliver() {
        std::unique_lock lock(mu_);
        for (;;) {
            cv_.wait(lock, [&] { return !chunks_.empty() || closing_; });
            if (chunks_.empty()) return;
            Chunk c = std::move(chunks_.front());
            chunks_.pop_front();
            lock.unlock();
            on_output_(c.stream, c.data, c.offset);
            lock.lock();
            queued_ -= c.data.size();  // counted until the call returns: a chunk in a slow consumer's hands still waits
            cv_.notify_all();
        }
    }

    const OnOutput& on_output_;
    Batch batch_[2];
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Chunk> chunks_;
    size_t queued_ = 0;
    bool closing_ = false;
    std::thread thread_;  // last: it starts in the constructor and uses everything above
};

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

// Forks bwrap with `args`, feeds `input` (when `feed_input`) to its stdin, reads stdout and, apart when
// `separate_stderr`, stderr, until both close, the deadline passes or the user cancels; the taps get copies.
SandboxResult spawn(std::vector<std::string>& args, const std::string& input, bool feed_input, bool separate_stderr,
                    std::chrono::seconds timeout, const std::atomic<bool>& cancel, const OutputTaps& taps) {
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0 || (feed_input && pipe2(in, O_CLOEXEC) != 0) || (separate_stderr && pipe2(err, O_CLOEXEC) != 0)) {
        throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    }
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0);
        // An ignored SIGPIPE is inherited through exec, and a process with an httplib server (maic-server) ignores
        // it: without this, `seq 1 1000000 | head` would print "write error: Broken pipe" instead of ending quietly.
        signal(SIGPIPE, SIG_DFL);
        if (feed_input) {
            dup2(in[0], STDIN_FILENO);
        } else {
            int null_fd = open("/dev/null", O_RDONLY);
            dup2(null_fd, STDIN_FILENO);
        }
        dup2(out[1], STDOUT_FILENO);
        dup2(separate_stderr ? err[1] : out[1], STDERR_FILENO);
        execvp(argv[0], argv.data());
        dprintf(STDERR_FILENO, "maic: can't run bwrap: %s\n", std::strerror(errno));
        _exit(127);
    }
    close(out[1]);
    if (feed_input) close(in[0]);
    if (separate_stderr) close(err[1]);
    int in_w = feed_input ? in[1] : -1, out_r = out[0], err_r = separate_stderr ? err[0] : -1;
    if (in_w >= 0) fcntl(in_w, F_SETFL, O_NONBLOCK);
    if (in_w >= 0 && input.empty()) {
        close(in_w);
        in_w = -1;
    }

    SandboxResult result;
    std::string stdout_text, stderr_text;
    size_t stdout_total = 0, stderr_total = 0;
    std::optional<OutputTee> tee;
    if (taps.on_output) tee.emplace(taps.on_output);
    size_t sent = 0;
    auto deadline = Clock::now() + timeout;
    char buf[8192];
    while (out_r >= 0 || err_r >= 0) {
        if (cancel.load()) {
            result.cancelled = true;
            break;
        }
        auto now = Clock::now();
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (left.count() <= 0) {
            result.timed_out = true;
            break;
        }
        long wait = std::min<long>(left.count(), 200);
        if (tee) wait = tee->flush_due(now, wait);
        pollfd pfds[3];
        int n_fds = 0;
        int out_i = -1, err_i = -1, in_i = -1;
        if (out_r >= 0) { out_i = n_fds; pfds[n_fds++] = {out_r, POLLIN, 0}; }
        if (err_r >= 0) { err_i = n_fds; pfds[n_fds++] = {err_r, POLLIN, 0}; }
        if (in_w >= 0) { in_i = n_fds; pfds[n_fds++] = {in_w, POLLOUT, 0}; }
        int ready = poll(pfds, static_cast<nfds_t>(n_fds), static_cast<int>(wait));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready <= 0) continue;
        if (out_i >= 0 && pfds[out_i].revents) {
            ssize_t n = read(out_r, buf, sizeof(buf));
            if (n <= 0) {
                close(out_r);
                out_r = -1;
            } else {
                if (taps.on_read) taps.on_read(OutputStream::Stdout, std::string_view(buf, static_cast<size_t>(n)), stdout_total);
                absorb(stdout_text, stdout_total, buf, static_cast<size_t>(n));
                if (tee) tee->add(OutputStream::Stdout, buf, static_cast<size_t>(n), Clock::now());
            }
        }
        if (err_i >= 0 && pfds[err_i].revents) {
            ssize_t n = read(err_r, buf, sizeof(buf));
            if (n <= 0) {
                close(err_r);
                err_r = -1;
            } else {
                if (taps.on_read) taps.on_read(OutputStream::Stderr, std::string_view(buf, static_cast<size_t>(n)), stderr_total);
                absorb(stderr_text, stderr_total, buf, static_cast<size_t>(n));
                if (tee) tee->add(OutputStream::Stderr, buf, static_cast<size_t>(n), Clock::now());
            }
        }
        if (in_i >= 0 && pfds[in_i].revents) {
            ssize_t n = (pfds[in_i].revents & (POLLERR | POLLHUP)) ? -1 : write_quietly(in_w, input.data() + sent, input.size() - sent);
            if (n > 0) sent += static_cast<size_t>(n);
            if (n < 0 || sent == input.size()) {
                close(in_w);  // all sent, or the script closed its stdin: either way nothing more goes in
                in_w = -1;
            }
        }
    }
    if (in_w >= 0) close(in_w);
    if (out_r >= 0) close(out_r);
    if (err_r >= 0) close(err_r);
    if (result.timed_out || result.cancelled) {
        kill(-pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (tee) {
        tee->finish();
        result.dropped_chunks = tee->dropped_chunks;
        result.dropped_bytes = tee->dropped_bytes;
    }

    result.output_bytes = stdout_total;
    result.output = trim_output(std::move(stdout_text), stdout_total);
    result.error = trim_output(std::move(stderr_text), stderr_total);
    return result;
}

}  // namespace

SandboxResult run_sandboxed(const std::string& command, const fs::path& workspace, bool read_only,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel, const fs::path& workdir, const OutputTaps& taps) {
    std::vector<std::string> args = bwrap_args(workspace, read_only, workdir);
    args.insert(args.end(), {"/bin/bash", "-c", command});
    return spawn(args, "", false, false, timeout, cancel, taps);
}

SandboxResult run_sandboxed_argv(const std::vector<std::string>& argv, const std::string& input, const fs::path& workspace,
                                 bool read_only, std::chrono::seconds timeout, const std::atomic<bool>& cancel, const fs::path& workdir,
                                 const OutputTaps& taps) {
    std::vector<std::string> args = bwrap_args(workspace, read_only, workdir);
    args.insert(args.end(), argv.begin(), argv.end());
    return spawn(args, input, true, true, timeout, cancel, taps);
}

}  // namespace maic
