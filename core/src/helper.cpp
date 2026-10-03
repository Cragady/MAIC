#include "maid/helper.hpp"

#include "maid/llm.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

extern char** environ;

namespace maid {

std::vector<std::string> keyless_environ() {
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        if (!is_key_env(std::string_view(*e, std::strcspn(*e, "=")))) env.push_back(*e);
    }
    return env;
}

int run_helper(const std::string& command, std::string* out, const std::string* input) {
    int in[2] = {-1, -1}, from[2] = {-1, -1};
    if ((input && pipe2(in, O_CLOEXEC) != 0) || (out && pipe2(from, O_CLOEXEC) != 0)) return 127;
    std::vector<std::string> env = keyless_environ();
    std::vector<char*> envp;
    for (auto& kv : env) envp.push_back(kv.data());
    envp.push_back(nullptr);
    std::string sh = "/bin/sh", flag = "-c", cmd = command;
    char* argv[] = {sh.data(), flag.data(), cmd.data(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (input) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (input) close(in[0]);
    if (out) close(from[1]);
    if (rc != 0) {
        if (input) close(in[1]);
        if (out) close(from[0]);
        return 127;
    }
    // The input goes in beside the reading, so a helper that answers before it has read everything cannot stall both.
    std::thread writer;
    if (input) {
        writer = std::thread([&] {
            // A helper that exits without reading gives EPIPE here, not a SIGPIPE that ends MAID.
            sigset_t pipe_sig;
            sigemptyset(&pipe_sig);
            sigaddset(&pipe_sig, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipe_sig, nullptr);
            for (size_t done = 0; done < input->size();) {
                ssize_t n = write(in[1], input->data() + done, input->size() - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                done += static_cast<size_t>(n);
            }
            close(in[1]);
        });
    }
    if (out) {
        char buf[4096];
        for (ssize_t n; (n = read(from[0], buf, sizeof buf)) != 0;) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) break;
            out->append(buf, static_cast<size_t>(n));
        }
        close(from[0]);
    }
    if (writer.joinable()) writer.join();
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

}  // namespace maid
