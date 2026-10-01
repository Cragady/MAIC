#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace maic {

// A place where a service leaves things on disk (outputs, logs, histories), so MAIC can show and clean it.
struct ArtifactDef {
    std::string name;
    std::string description;
    std::filesystem::path path;
};

// A long-running local process MAIC starts and stops, loaded from services/<name>.json.
struct ServiceDef {
    std::string name;
    std::string description;
    std::vector<std::string> command;
    std::map<std::string, std::string> env;
    std::filesystem::path cwd;
    int port = 0;
    std::vector<std::filesystem::path> requires_paths;
    std::chrono::seconds ready_timeout{30};
    std::vector<ArtifactDef> artifacts;
    std::string runtime = "host";  // "host": a process MAIC starts; "docker": a container (image, volumes, gpu below), loopback only
    bool needs_gpu = false;        // starting it first frees the GPU: each llama server's resident model is unloaded
    std::string ready_pattern;     // POSIX extended regex the output since start must match, besides the open port, to count as ready
    std::string image;             // docker: the image `docker run` starts; MAIC never pulls it
    std::map<std::string, std::string> volumes;  // docker: host path -> container path; hosts only under the state, models or vendor trees
    bool gpu = false;              // docker: --gpus all
};

std::vector<ServiceDef> load_services(const std::filesystem::path& dir);

enum class ServiceState {
    Stopped,
    Running,   // started by MAIC and still the same process
    Foreign,   // port in use by something MAIC did not start
};

struct ServiceStatus {
    ServiceState state = ServiceState::Stopped;
    pid_t pid = 0;           // host: the process
    std::string container;   // docker: maic-<name>
    bool port_open = false;
    std::string who() const;  // "pid 1234" or "container maic-comfyui"
};

ServiceStatus service_status(const ServiceDef& def);

// Launches the service detached from the terminal (host) or as `docker run --rm -d --name maic-<name>
// -p 127.0.0.1:<port>:<port> ...` (docker) and waits for its port and, when set, its ready_pattern.
// Returns false if it is still starting when ready_timeout runs out.
bool start_service(const ServiceDef& def);
// What start_service runs: the command itself, or the docker run argv.
std::vector<std::string> launch_argv(const ServiceDef& def);
// The command a running service was started with (recorded at start), "" when unknown or not running.
std::string recorded_command(const ServiceDef& def);
// True when the service is running and `def` (as loaded now) would start it with a different command.
bool command_changed(const ServiceDef& def);

// Stops a service MAIC started: SIGTERM to its process group, SIGKILL after the timeout (host); `docker stop`
// (docker). Never signals a process whose identity doesn't match the PID file, nor a container MAIC did not record.
void stop_service(const ServiceDef& def, std::chrono::seconds timeout = std::chrono::seconds{15});

// The log MAIC keeps: the process's output, or for a container MAIC's own notes (start headers, docker errors).
std::filesystem::path service_log_path(const ServiceDef& def);
// The last `lines` lines of the service's output: the log file, or `docker logs` while the container runs.
std::string log_tail(const ServiceDef& def, size_t lines);

}  // namespace maic
