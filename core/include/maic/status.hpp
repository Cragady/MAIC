#pragma once

#include "maic/llm.hpp"
#include "maic/service.hpp"

#include <string>
#include <vector>

namespace maic {

// One line of `maic status` / `:status`, with what the user could do about it.
struct ServiceReport {
    std::string name;
    std::string state;    // running, stopped, foreign, starting
    std::string runtime;  // host (a process MAIC started), docker (planned)
    std::string where;    // "pid 1234 · http://127.0.0.1:11434", or the port for a foreign process
    std::string log;      // path of the log MAIC keeps for it
    std::vector<std::string> actions;  // quick actions, as commands the user can run
};

std::vector<ServiceReport> service_reports(const std::vector<ServiceDef>& services);

struct StatusReport {
    bool tripped = false;
    std::string tripwire;  // contents of the lock file when tripped
    std::vector<ServiceReport> services;
    std::vector<std::string> actions;  // harness-level quick actions
};

StatusReport status_report(const std::vector<ServiceDef>& services);

// Plain-text rendering shared by the CLI command and the TUI.
std::string format_status(const StatusReport& report);

// What a service still needs before it can start, with what to do about it: a mounted drive, or for a
// vendored model link, `maic vendor use`. Empty when it can start.
std::string missing_requirement(const ServiceDef& def);

// Why a local provider could not be reached and what to do, from the state of the service on its port
// (the one MAIC runs for it). Empty when no service listens there: a remote provider, or one MAIC does not run.
std::string unreachable_hint(const Provider& provider, const std::vector<ServiceDef>& services);

// The models llama-server (router mode, at `base_url` such as http://127.0.0.1:8081) has resident, by id.
// Empty when it is not running or serves a single model. `unload_resident` asks it to unload each of them
// and returns what it unloaded; the server stays up and reloads on the next request.
std::vector<std::string> resident_models(const std::string& base_url);
std::vector<std::string> unload_resident(const std::string& base_url);

// Before starting `def`: when it needs the GPU and llama-server holds a model, unload it. Returns a notice
// ("" when nothing had to happen).
std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services);

// Who holds the card, without nvidia-smi: llama-server's resident models and ComfyUI's own VRAM figures.
struct GpuReport {
    std::vector<std::string> llamacpp_models;  // resident in llama-server ("" entries never)
    bool llamacpp_running = false;
    bool comfyui_running = false;
    long comfyui_vram_used = -1;   // bytes, from ComfyUI's /system_stats; -1 when unknown
    long comfyui_vram_total = -1;
    std::string text() const;      // a few lines for a person
};
GpuReport gpu_report(const std::vector<ServiceDef>& services);
// Frees what can be freed without stopping anything: llama-server unloads its models, ComfyUI unloads its
// models and releases cached memory (its /free route). `what` is "all", "llamacpp" or "comfyui".
std::string gpu_free(const std::vector<ServiceDef>& services, const std::string& what = "all");

// Why a service died, from the tail of its log: a CUDA out of memory, a missing module, a port in use, and
// what to do about it. "" when nothing recognisable is there.
std::string explain_exit(const ServiceDef& def, const std::vector<ServiceDef>& services);

}  // namespace maic
