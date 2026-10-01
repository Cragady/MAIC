#include "maic/sandbox.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace maic {

namespace fs = std::filesystem;

namespace {

constexpr size_t kHeadBytes = 24 * 1024;
constexpr size_t kTailBytes = 8 * 1024;

// Everything before the program: the mounts, the namespaces and the working directory.
std::vector<std::string> bwrap_args(const fs::path& workspace, bool read_only, const fs::path& workdir) {
    std::vector<std::string> args = {
        "bwrap",
        "--ro-bind", "/", "/",
        "--dev", "/dev",
        "--proc", "/proc",
        "--tmpfs", "/tmp",
        read_only ? "--ro-bind" : "--bind", workspace.string(), workspace.string(),
    };
    // Hide secrets behind empty directories. /var/lib/maic stays visible read-only so tools can see the lock.
    fs::path home = std::getenv("HOME");
    for (const char* p : {".ssh", ".gnupg", ".aws", ".kube", ".docker", ".password-store", ".local/share/keyrings", ".ollama"}) {
        if (fs::is_directory(home / p) && !fs::equivalent(home / p, workspace)) {
            args.insert(args.end(), {"--tmpfs", (home / p).string()});
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

std::string trim_output(std::string out) {
    if (out.size() <= kHeadBytes + kTailBytes) {
        return out;
    }
    size_t cut = out.size() - kHeadBytes - kTailBytes;
    return out.substr(0, kHeadBytes) + "\n... [" + std::to_string(cut) + " bytes omitted] ...\n" +
           out.substr(out.size() - kTailBytes);
}

// Keeps memory bounded on runaway output: the head and a rolling tail.
void absorb(std::string& out, const char* buf, size_t n) {
    out.append(buf, n);
    if (out.size() > 4 * (kHeadBytes + kTailBytes)) {
        out.erase(kHeadBytes, out.size() - kHeadBytes - 2 * kTailBytes);
    }
}

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
// `separate_stderr`, stderr, until both close, the deadline passes or the user cancels.
SandboxResult spawn(std::vector<std::string>& args, const std::string& input, bool feed_input, bool separate_stderr,
                    std::chrono::seconds timeout, const std::atomic<bool>& cancel) {
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
    size_t sent = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[8192];
    while (out_r >= 0 || err_r >= 0) {
        if (cancel.load()) {
            result.cancelled = true;
            break;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            result.timed_out = true;
            break;
        }
        pollfd pfds[3];
        int n_fds = 0;
        int out_i = -1, err_i = -1, in_i = -1;
        if (out_r >= 0) { out_i = n_fds; pfds[n_fds++] = {out_r, POLLIN, 0}; }
        if (err_r >= 0) { err_i = n_fds; pfds[n_fds++] = {err_r, POLLIN, 0}; }
        if (in_w >= 0) { in_i = n_fds; pfds[n_fds++] = {in_w, POLLOUT, 0}; }
        int ready = poll(pfds, static_cast<nfds_t>(n_fds), static_cast<int>(std::min<long>(left.count(), 200)));
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
                absorb(stdout_text, buf, static_cast<size_t>(n));
            }
        }
        if (err_i >= 0 && pfds[err_i].revents) {
            ssize_t n = read(err_r, buf, sizeof(buf));
            if (n <= 0) {
                close(err_r);
                err_r = -1;
            } else {
                absorb(stderr_text, buf, static_cast<size_t>(n));
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

    result.output = trim_output(std::move(stdout_text));
    result.error = trim_output(std::move(stderr_text));
    return result;
}

}  // namespace

SandboxResult run_sandboxed(const std::string& command, const fs::path& workspace, bool read_only,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel, const fs::path& workdir) {
    std::vector<std::string> args = bwrap_args(workspace, read_only, workdir);
    args.insert(args.end(), {"/bin/bash", "-c", command});
    return spawn(args, "", false, false, timeout, cancel);
}

SandboxResult run_sandboxed_argv(const std::vector<std::string>& argv, const std::string& input, const fs::path& workspace,
                                 bool read_only, std::chrono::seconds timeout, const std::atomic<bool>& cancel, const fs::path& workdir) {
    std::vector<std::string> args = bwrap_args(workspace, read_only, workdir);
    args.insert(args.end(), argv.begin(), argv.end());
    return spawn(args, input, true, true, timeout, cancel);
}

}  // namespace maic
