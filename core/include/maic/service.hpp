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
    std::string runtime = "host";  // "host": a process MAIC starts. "docker" is reserved for a container runtime.
    bool needs_gpu = false;        // starting it first frees the GPU: llama-server's resident model is unloaded
};

std::vector<ServiceDef> load_services(const std::filesystem::path& dir);

enum class ServiceState {
    Stopped,
    Running,   // started by MAIC and still the same process
    Foreign,   // port in use by something MAIC did not start
};

struct ServiceStatus {
    ServiceState state = ServiceState::Stopped;
    pid_t pid = 0;
    bool port_open = false;
};

ServiceStatus service_status(const ServiceDef& def);

// Launches the service detached from the terminal and waits for its port.
// Returns false if it is still starting when ready_timeout runs out.
bool start_service(const ServiceDef& def);
// The command a running service was started with (recorded at start), "" when unknown or not running.
std::string recorded_command(const ServiceDef& def);
// True when the service is running and `def` (as loaded now) would start it with a different command.
bool command_changed(const ServiceDef& def);

// Stops a service MAIC started: SIGTERM to its process group, SIGKILL after the timeout.
// Never signals a process whose identity doesn't match the PID file.
void stop_service(const ServiceDef& def, std::chrono::seconds timeout = std::chrono::seconds{15});

std::filesystem::path service_log_path(const ServiceDef& def);

}  // namespace maic
