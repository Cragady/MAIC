#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace maid {

// A place where a service leaves things on disk (outputs, logs, histories), so MAID can show and clean it.
struct ArtifactDef {
    std::string name;
    std::string description;
    std::filesystem::path path;
};

// A long-running local process MAID starts and stops, loaded from services/<name>.json.
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
    std::string runtime = "host";  // "host": a process MAID starts; "docker": a container (image, volumes, gpu below), loopback only
    bool needs_gpu = false;        // it uses the GPU (maid status tags it); starting a non-llama one first frees the GPU: each llama server's resident model is unloaded
    std::string ready_pattern;     // POSIX extended regex the output since start must match, besides the open port, to count as ready
    std::string image;             // docker: the image `docker run` starts; MAID never pulls it
    std::map<std::string, std::string> volumes;  // docker: host path -> container path; hosts only under the state, models or vendor trees
    bool gpu = false;              // docker: --gpus all
};

// A file that doesn't load (bad JSON, an unset variable, a refused volume) is skipped, so one broken service never
// stops the others: its problem goes into `problems`, or to stderr once per process when none is given. A missing
// directory is reported the same way and loads nothing; this never throws.
std::vector<ServiceDef> load_services(const std::filesystem::path& dir, std::vector<std::string>* problems = nullptr);

enum class ServiceState {
    Stopped,
    Running,   // started by MAID and still the same process
    Foreign,   // port in use by something MAID did not start
};

struct ServiceStatus {
    ServiceState state = ServiceState::Stopped;
    pid_t pid = 0;           // host: the process
    std::string container;   // docker: maid-<name>
    bool port_open = false;
    bool crashed = false;    // stopped, but the record of a start is still there: it exited without `maid down`
    std::string who() const;  // "pid 1234" or "container maid-comfyui"
};

ServiceStatus service_status(const ServiceDef& def);

// Launches the service detached from the terminal (host) or as `docker run --rm -d --name maid-<name>
// -p 127.0.0.1:<port>:<port> ...` (docker) and waits for its port and, when set, its ready_pattern.
// Returns false if it is still starting when ready_timeout runs out.
bool start_service(const ServiceDef& def);
// What start_service runs: the command itself, or the docker run argv.
std::vector<std::string> launch_argv(const ServiceDef& def);
// The command a running service was started with (recorded at start), "" when unknown or not running.
std::string recorded_command(const ServiceDef& def);
// True when the service is running and `def` (as loaded now) would start it with a different command.
bool command_changed(const ServiceDef& def);

// Stops a service MAID started: SIGTERM to its process group, SIGKILL after the timeout (host); `docker stop`
// (docker). Never signals a process whose identity doesn't match the PID file, nor a container MAID did not record.
void stop_service(const ServiceDef& def, std::chrono::seconds timeout = std::chrono::seconds{15});

// The log MAID keeps: the process's output, or for a container MAID's own notes (start headers, docker errors).
std::filesystem::path service_log_path(const ServiceDef& def);
// The last `lines` lines of the service's output: the log file, or `docker logs` while the container runs.
std::string log_tail(const ServiceDef& def, size_t lines);

}  // namespace maid
