#include "maic/service.hpp"

#include "maic/llm.hpp"
#include "maic/paths.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

extern char** environ;

namespace maic {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Identity of a process: PID alone is not enough because Linux reuses PIDs.
struct ProcessId {
    pid_t pid = 0;
    unsigned long long start_time = 0;  // clock ticks since boot, /proc/<pid>/stat field 22
};

struct ProcStat {
    char state = '?';
    pid_t pgrp = 0;
    unsigned long long start_time = 0;
};

std::optional<ProcStat> read_proc_stat(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!in || !std::getline(in, line)) {
        return std::nullopt;
    }
    // The command name (field 2) is in parentheses and may contain spaces.
    size_t close = line.rfind(')');
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::istringstream rest(line.substr(close + 2));
    std::vector<std::string> fields;  // fields[0] is field 3 (state)
    for (std::string f; rest >> f;) {
        fields.push_back(f);
    }
    if (fields.size() < 20) {
        return std::nullopt;
    }
    return ProcStat{fields[0][0], static_cast<pid_t>(std::stol(fields[2])), std::stoull(fields[19])};
}

bool is_alive(const ProcessId& id) {
    auto stat = read_proc_stat(id.pid);
    // A zombie has exited; it only waits for its parent to collect it.
    return stat && stat->start_time == id.start_time && stat->state != 'Z';
}

fs::path pid_path(const ServiceDef& def) {
    return state_dir() / "run" / (def.name + ".pid");
}

std::optional<ProcessId> read_pid_file(const ServiceDef& def) {
    std::ifstream in(pid_path(def));
    ProcessId id;
    if (!(in >> id.pid >> id.start_time)) {
        return std::nullopt;
    }
    return id;
}

void write_pid_file(const ServiceDef& def, const ProcessId& id) {
    fs::path path = pid_path(def);
    fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << id.pid << ' ' << id.start_time << '\n';
        if (!out) {
            throw std::runtime_error("could not write " + tmp.string());
        }
    }
    fs::rename(tmp, path);
}

std::string container_name(const ServiceDef& def) {
    return "maic-" + def.name;
}

// The docker equivalent of the pid file: its presence says MAIC started the container.
fs::path container_path(const ServiceDef& def) {
    return state_dir() / "run" / (def.name + ".container");
}

std::vector<char*> c_strings(std::vector<std::string>& strings) {
    std::vector<char*> out;
    for (auto& s : strings) {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

// Runs a program with stdout and stderr captured; the exit code, 127 when it could not be run.
int run_capture(std::vector<std::string> args, std::string& out) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return 127;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 127;
    }
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDONLY);
        dup2(null_fd, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        std::vector<char*> argv = c_strings(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
}

bool container_running(const ServiceDef& def) {
    std::string out;
    return run_capture({"docker", "inspect", "-f", "{{.State.Running}}", container_name(def)}, out) == 0 && out.rfind("true", 0) == 0;
}

std::vector<std::string> docker_run_argv(const ServiceDef& def) {
    std::vector<std::string> a = {"docker", "run", "--rm", "-d", "--name", container_name(def)};
    if (def.port) {
        std::string p = std::to_string(def.port);
        a.insert(a.end(), {"-p", "127.0.0.1:" + p + ":" + p});  // loopback only, like every host service
    }
    for (const auto& [host, inside] : def.volumes) a.insert(a.end(), {"-v", host + ":" + inside});
    for (const auto& [k, v] : def.env) a.insert(a.end(), {"-e", k + "=" + v});
    if (def.gpu) a.insert(a.end(), {"--gpus", "all"});
    a.push_back(def.image);
    a.insert(a.end(), def.command.begin(), def.command.end());
    return a;
}

// POSIX extended regex over the text; a pattern that does not compile matches nothing (load_services refused it).
bool matches(const std::string& pattern, const std::string& text) {
    regex_t re;
    if (regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB | REG_NEWLINE) != 0) return false;
    bool hit = regexec(&re, text.c_str(), 0, nullptr, 0) == 0;
    regfree(&re);
    return hit;
}

std::string regex_error(const std::string& pattern) {
    regex_t re;
    int rc = regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB | REG_NEWLINE);
    if (rc == 0) {
        regfree(&re);
        return "";
    }
    char buf[256];
    regerror(rc, &re, buf, sizeof buf);
    return buf;
}

// The service's output since `since` (a byte offset into the log file; a container's whole log).
std::string output_since(const ServiceDef& def, std::streamoff since) {
    if (def.runtime == "docker") {
        std::string out;
        run_capture({"docker", "logs", container_name(def)}, out);
        return out;
    }
    std::ifstream in(service_log_path(def), std::ios::binary);
    in.seekg(since);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool port_open(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool open = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return open;
}

// The service's environment: ours without model keys, with the definition's variables on top.
std::vector<std::string> build_env(const ServiceDef& def) {
    std::map<std::string, std::string> merged;
    for (char** e = environ; *e; ++e) {
        std::string_view kv(*e);
        size_t eq = kv.find('=');
        if (eq != std::string_view::npos && !is_key_env(kv.substr(0, eq))) {
            merged[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
        }
    }
    for (const auto& [k, v] : def.env) {
        merged[k] = v;
    }
    std::vector<std::string> out;
    for (const auto& [k, v] : merged) {
        out.push_back(k + "=" + v);
    }
    return out;
}

std::string timestamp() {
    std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return buf;
}

}  // namespace

std::vector<ServiceDef> load_services(const fs::path& dir) {
    std::vector<ServiceDef> out;
    if (!fs::is_directory(dir)) {
        throw std::runtime_error("no services directory at " + dir.string());
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(in);
            ServiceDef def;
            def.name = j.at("name").get<std::string>();
            def.description = j.value("description", "");
            for (const auto& arg : j.value("command", nlohmann::json::array())) {
                def.command.push_back(expand_vars(arg.get<std::string>()));
            }
            nlohmann::json env = j.value("env", nlohmann::json::object());
            for (const auto& [k, v] : env.items()) {
                def.env[k] = expand_vars(v.get<std::string>());
            }
            if (j.contains("cwd")) {
                def.cwd = expand_vars(j["cwd"].get<std::string>());
            }
            def.port = j.value("port", 0);
            for (const auto& p : j.value("requires", nlohmann::json::array())) {
                def.requires_paths.emplace_back(expand_vars(p.get<std::string>()));
            }
            def.ready_timeout = std::chrono::seconds(j.value("ready_timeout", 30));
            def.runtime = j.value("runtime", "host");
            def.needs_gpu = j.value("needs_gpu", false);
            def.ready_pattern = j.value("ready_pattern", "");
            if (std::string err = regex_error(def.ready_pattern); !err.empty()) {
                throw std::runtime_error("ready_pattern: " + err);
            }
            if (def.runtime == "docker") {
                def.image = j.value("image", "");
                if (def.image.empty()) throw std::runtime_error("a docker service needs an image");
                def.gpu = j.value("gpu", false);
                // A container sees only MAIC's own trees: state, models, vendor. Anything else stays outside.
                std::vector<fs::path> roots = {state_dir(), expand_vars("${MAIC_MODELS}"), expand_vars("${MAIC_VENDOR}")};
                nlohmann::json volumes = j.value("volumes", nlohmann::json::object());
                for (const auto& [host, inside] : volumes.items()) {
                    fs::path h = fs::path(expand_vars(host)).lexically_normal();
                    bool allowed = false;
                    for (const auto& root : roots) {
                        auto rel = h.lexically_relative(root.lexically_normal());
                        if (!rel.empty() && *rel.begin() != "..") allowed = true;
                    }
                    if (!allowed) throw std::runtime_error("volume " + h.string() + " is outside ${MAIC_STATE}, ${MAIC_MODELS} and ${MAIC_VENDOR}");
                    def.volumes[h.string()] = inside.get<std::string>();
                }
            } else if (def.runtime != "host") {
                throw std::runtime_error("runtime '" + def.runtime + "' is unknown (host or docker)");
            }
            for (const auto& a : j.value("artifacts", nlohmann::json::array())) {
                def.artifacts.push_back({a.at("name").get<std::string>(), a.value("description", ""),
                                         expand_vars(a.at("path").get<std::string>())});
            }
            if (def.command.empty() && def.runtime == "host") {
                throw std::runtime_error("command is empty");
            }
            out.push_back(std::move(def));
        } catch (const std::exception& e) {
            throw std::runtime_error(entry.path().string() + ": " + e.what());
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

std::string recorded_command(const ServiceDef& def) {
    if (service_status(def).state != ServiceState::Running) return "";
    std::ifstream in(state_dir() / "run" / (def.name + ".cmd"));
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return out;
}

std::vector<std::string> launch_argv(const ServiceDef& def) {
    return def.runtime == "docker" ? docker_run_argv(def) : def.command;
}

bool command_changed(const ServiceDef& def) {
    std::string now;
    for (const auto& a : launch_argv(def)) now += a + '\n';
    std::string was = recorded_command(def);
    return !was.empty() && was != now;
}

fs::path service_log_path(const ServiceDef& def) {
    return state_dir() / "logs" / (def.name + ".log");
}

std::string log_tail(const ServiceDef& def, size_t lines) {
    std::error_code ec;
    if (def.runtime == "docker" && fs::exists(container_path(def), ec) && container_running(def)) {
        std::string out;
        run_capture({"docker", "logs", "--tail", std::to_string(lines), container_name(def)}, out);
        return out;
    }
    std::ifstream in(service_log_path(def));
    std::deque<std::string> tail;
    for (std::string line; std::getline(in, line);) {
        tail.push_back(std::move(line));
        if (tail.size() > lines) tail.pop_front();
    }
    std::string out;
    for (const auto& l : tail) out += l + "\n";
    return out;
}

std::string ServiceStatus::who() const {
    return container.empty() ? "pid " + std::to_string(pid) : "container " + container;
}

ServiceStatus service_status(const ServiceDef& def) {
    ServiceStatus status;
    status.port_open = def.port && port_open(def.port);
    if (def.runtime == "docker") {
        std::error_code ec;
        if (fs::exists(container_path(def), ec) && container_running(def)) {
            status.state = ServiceState::Running;
            status.container = container_name(def);
        } else if (status.port_open) {
            status.state = ServiceState::Foreign;
        } else {
            status.crashed = fs::exists(container_path(def), ec);
        }
        return status;
    }
    auto id = read_pid_file(def);
    if (id && is_alive(*id)) {
        status.state = ServiceState::Running;
        status.pid = id->pid;
    } else if (status.port_open) {
        status.state = ServiceState::Foreign;
    } else {
        status.crashed = id.has_value();
    }
    return status;
}

bool start_service(const ServiceDef& def) {
    ServiceStatus status = service_status(def);
    if (status.state == ServiceState::Running) {
        throw std::runtime_error(def.name + " is already running (" + status.who() + ")");
    }
    if (status.state == ServiceState::Foreign) {
        throw std::runtime_error("port " + std::to_string(def.port) + " is already in use by a process MAIC did not start");
    }
    for (const auto& path : def.requires_paths) {
        if (!fs::exists(path)) {
            throw std::runtime_error(def.name + " needs " + path.string() + " (is the drive mounted?)");
        }
    }
    fs::remove(pid_path(def));
    fs::remove(container_path(def));

    fs::path log = service_log_path(def);
    fs::create_directories(log.parent_path());
    int log_fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) {
        throw std::runtime_error("could not open " + log.string() + ": " + std::strerror(errno));
    }
    std::string header = "\n=== maic: starting " + def.name + " at " + timestamp() + " ===\n";
    (void)!write(log_fd, header.data(), header.size());
    std::streamoff since = static_cast<std::streamoff>(lseek(log_fd, 0, SEEK_CUR));  // the service's own output starts here
    fs::create_directories(state_dir() / "run");
    {
        std::ofstream cmd(state_dir() / "run" / (def.name + ".cmd"), std::ios::trunc);
        for (const auto& a : launch_argv(def)) cmd << a << '\n';
    }
    auto ready = [&] { return (!def.port || port_open(def.port)) && (def.ready_pattern.empty() || matches(def.ready_pattern, output_since(def, since))); };

    if (def.runtime == "docker") {
        std::string out;
        int rc = run_capture(docker_run_argv(def), out);
        (void)!write(log_fd, out.data(), out.size());
        close(log_fd);
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (rc != 0) throw std::runtime_error(def.name + ": docker run failed (" + (out.empty() ? "exit " + std::to_string(rc) : out) + "); see " + log.string());
        fs::create_directories(container_path(def).parent_path());
        std::ofstream(container_path(def), std::ios::trunc) << container_name(def) << '\n';
        auto deadline = std::chrono::steady_clock::now() + def.ready_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!container_running(def)) {
                fs::remove(container_path(def));
                throw std::runtime_error(def.name + " exited (the container is gone with --rm); see " + log.string());
            }
            if (ready()) return true;
            std::this_thread::sleep_for(500ms);
        }
        return false;
    }

    // Everything the child needs is built before fork so the child only makes syscalls.
    std::vector<std::string> args = def.command;
    std::vector<std::string> env = build_env(def);
    std::vector<char*> argv = c_strings(args);
    std::vector<char*> envp = c_strings(env);
    std::string cwd = def.cwd.string();

    pid_t pid = fork();
    if (pid < 0) {
        close(log_fd);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        setsid();  // own session and process group, detached from the terminal
        int null_fd = open("/dev/null", O_RDONLY);
        dup2(null_fd, STDIN_FILENO);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            dprintf(STDERR_FILENO, "maic: chdir %s: %s\n", cwd.c_str(), std::strerror(errno));
            _exit(127);
        }
        execvpe(argv[0], argv.data(), envp.data());
        dprintf(STDERR_FILENO, "maic: exec %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    close(log_fd);

    auto stat = read_proc_stat(pid);
    if (!stat) {
        throw std::runtime_error(def.name + " exited immediately; see " + log.string());
    }
    write_pid_file(def, {pid, stat->start_time});

    auto deadline = std::chrono::steady_clock::now() + def.ready_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int wstatus = 0;
        if (waitpid(pid, &wstatus, WNOHANG) == pid) {
            fs::remove(pid_path(def));
            std::string how = WIFEXITED(wstatus) ? "exited with code " + std::to_string(WEXITSTATUS(wstatus))
                                                 : "was killed by signal " + std::to_string(WTERMSIG(wstatus));
            throw std::runtime_error(def.name + " " + how + "; see " + log.string());
        }
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

void stop_service(const ServiceDef& def, std::chrono::seconds timeout) {
    if (def.runtime == "docker") {
        std::error_code ec;
        if (!fs::exists(container_path(def), ec) || !container_running(def)) {
            fs::remove(container_path(def), ec);
            if (def.port && port_open(def.port)) {
                throw std::runtime_error(def.name + " was not started by MAIC; refusing to stop it");
            }
            throw std::runtime_error(def.name + " is not running");
        }
        std::string out;
        int rc = run_capture({"docker", "stop", "-t", std::to_string(timeout.count()), container_name(def)}, out);
        if (rc != 0) throw std::runtime_error("docker stop " + container_name(def) + " failed: " + out);
        fs::remove(container_path(def), ec);
        return;
    }
    auto id = read_pid_file(def);
    if (!id || !is_alive(*id)) {
        fs::remove(pid_path(def));
        if (def.port && port_open(def.port)) {
            throw std::runtime_error(def.name + " was not started by MAIC; refusing to stop it");
        }
        throw std::runtime_error(def.name + " is not running");
    }

    // Signal the whole group only if it is still the group start_service created.
    auto stat = read_proc_stat(id->pid);
    pid_t target = (stat && stat->pgrp == id->pid) ? -id->pid : id->pid;

    kill(target, SIGTERM);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (is_alive(*id) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(200ms);
    }
    if (is_alive(*id)) {
        kill(target, SIGKILL);
        while (is_alive(*id)) {
            std::this_thread::sleep_for(100ms);
        }
    }
    fs::remove(pid_path(def));
}

}  // namespace maic
