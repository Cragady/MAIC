#include "nvim_run.hpp"

#include "maic/llm.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdexcept>
#include <thread>

extern char** environ;

namespace maic {

NvimRun run_nvim_child(const std::vector<std::string>& args, const std::vector<std::string>& drop, const std::vector<std::string>& add,
                       std::chrono::seconds timeout, const std::atomic<bool>* cancel) {
    // The environment is built before fork: the child of a threaded process may only exec.
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        std::string name = kv.substr(0, kv.find('='));
        bool dropped = is_key_env(name);
        for (const auto& d : drop) dropped = dropped || name == d || (d.back() == '_' && name.rfind(d, 0) == 0);
        if (!dropped) env.push_back(kv);
    }
    env.insert(env.end(), add.begin(), add.end());
    std::vector<char*> envp;
    for (auto& kv : env) envp.push_back(kv.data());
    envp.push_back(nullptr);
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (auto& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);
    NvimRun r;
    bool done = false;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done) {
        done = waitpid(pid, &r.status, WNOHANG) == pid;
        if (done) break;
        if (cancel && cancel->load()) r.cancelled = true;
        else if (std::chrono::steady_clock::now() >= deadline) r.timed_out = true;
        if (r.cancelled || r.timed_out) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-pid, SIGKILL);  // whatever it left behind, and nvim itself when it ran out of time
    if (!done) waitpid(pid, &r.status, 0);
    return r;
}

}  // namespace maic
