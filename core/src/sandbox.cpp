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

std::vector<std::string> bwrap_args(const std::string& command, const fs::path& workspace, bool read_only, const fs::path& workdir) {
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
        "--", "/bin/bash", "-c", command,
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

}  // namespace

SandboxResult run_sandboxed(const std::string& command, const fs::path& workspace, bool read_only,
                            std::chrono::seconds timeout, const std::atomic<bool>& cancel, const fs::path& workdir) {
    std::vector<std::string> args = bwrap_args(command, workspace, read_only, workdir);
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
    }
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDONLY);
        dup2(null_fd, STDIN_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        execvp(argv[0], argv.data());
        dprintf(STDERR_FILENO, "maic: can't run bwrap: %s\n", std::strerror(errno));
        _exit(127);
    }
    close(pipefd[1]);

    SandboxResult result;
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[8192];
    for (;;) {
        if (cancel.load()) {
            result.cancelled = true;
            break;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            result.timed_out = true;
            break;
        }
        pollfd pfd{pipefd[0], POLLIN, 0};
        int ready = poll(&pfd, 1, static_cast<int>(std::min<long>(left.count(), 200)));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) {
                break;  // EOF: the command finished
            }
            // Keep memory bounded on runaway output: hold the head and a rolling tail.
            out.append(buf, static_cast<size_t>(n));
            if (out.size() > 4 * (kHeadBytes + kTailBytes)) {
                out.erase(kHeadBytes, out.size() - kHeadBytes - 2 * kTailBytes);
            }
        }
    }
    close(pipefd[0]);
    if (result.timed_out || result.cancelled) {
        kill(-pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

    result.output = trim_output(std::move(out));
    return result;
}

}  // namespace maic
