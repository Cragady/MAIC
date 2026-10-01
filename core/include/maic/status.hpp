#pragma once

#include "maic/llm.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"

#include <string>
#include <vector>

namespace maic {

// One line of `maic status` / `:status`, with what the user could do about it.
struct ServiceReport {
    std::string name;
    std::string state;    // running, stopped, foreign, starting
    std::string runtime;  // host (a process MAIC started), docker (a container it started)
    std::string where;    // "pid 1234 · http://127.0.0.1:8081", "container maic-comfyui · ...", or the port for a foreign process
    std::string detail;   // what it holds, when it answers: a llama server's resident model, ComfyUI's VRAM and queue
    std::string log;      // path of the log MAIC keeps for it
    std::vector<std::string> actions;  // quick actions, as commands the user can run
};

std::vector<ServiceReport> service_reports(const std::vector<ServiceDef>& services);

// Health beyond the open port, for a service that answers: "model: Qwen3.5-4B-Q4_K_M" or "no model resident" for a
// llama server; "VRAM 3.0 GB used of 8.0 GB; queue idle" (or "queue: 1 running, 2 pending") for ComfyUI. "" for
// the rest, or when the port is closed.
std::string service_detail(const ServiceDef& def, const ServiceStatus& status);

struct StatusReport {
    bool tripped = false;
    std::string tripwire;  // contents of the lock file when tripped
    std::vector<ServiceReport> services;
    std::vector<std::string> actions;  // harness-level quick actions
    std::string lazy_lock;  // nvim's lazy-lock.json (lazy_lock_summary), filled by the caller; "" prints nothing
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

// The llama servers: `llamacpp` and `llamacpp-2` (services/llamacpp*.json), one resident model each.
bool is_llama_server(const std::string& service_name);

// Before starting `def`: when it needs the GPU and a llama server holds a model, unload it. A llama server
// itself loads nothing at start, so starting one never evicts the other. Returns a notice ("" when nothing
// had to happen).
std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services);

// Who holds the card, without nvidia-smi where possible: each llama server's resident models and ComfyUI's
// own VRAM figures.
struct GpuReport {
    struct Server {
        std::string name;                 // llamacpp, llamacpp-2
        bool running = false;
        std::vector<std::string> models;  // resident ("" entries never)
    };
    std::vector<Server> servers;   // every llama server, in service order
    bool comfyui_running = false;
    long comfyui_vram_used = -1;   // bytes, from ComfyUI's /system_stats; -1 when unknown
    long comfyui_vram_total = -1;
    long card_total = -1;          // bytes: ComfyUI's figure, else nvidia-smi's, else -1
    std::string text() const;      // a few lines for a person
};
GpuReport gpu_report(const std::vector<ServiceDef>& services);
// Frees what can be freed without stopping anything: a llama server unloads its models, ComfyUI unloads its
// models and releases cached memory (its /free route). `what` is "all", a llama server's name or "comfyui".
std::string gpu_free(const std::vector<ServiceDef>& services, const std::string& what = "all");

// One model as a server holds it, for the budget sentence.
struct ModelPlan {
    std::string id;   // the router id (file stem or folder name)
    int context = 0;  // tokens
};
// What a plan takes on the card: the GGUF's size on disk (an mmproj beside it counts) plus a rough KV cache
// estimate from the context and the parameter count in the name (65 MB per 1k tokens for a 4B, 130 for a
// 9B). -1 when no such model is under `models_root`.
long model_footprint(const ModelPlan& plan, const std::filesystem::path& models_root);
// One plain sentence: "4B at 16k (3.6 GB est.) + 4B at 8k (3.1 GB est.) = 6.7 GB of 8.0 GB: fits with ComfyUI
// stopped". `card_total` and `comfyui_used` in bytes, -1 when unknown. "" when no plan has a file.
std::string budget_sentence(const std::vector<ModelPlan>& plans, const std::filesystem::path& models_root, long card_total, bool comfyui_running, long comfyui_used);
// The sentence for this machine: each llama server's resident model, else the one settings would send it
// (`model` and `context` for llamacpp; `reviewer_model` on llamacpp-2, else the same model, with `context_2`).
std::string gpu_budget(const GpuReport& report, const Settings& settings, long card_total_fallback = -1);

// Why a service died, from the tail of its log: a CUDA out of memory, a missing module, a port in use, and
// what to do about it. "" when nothing recognisable is there.
std::string explain_exit(const ServiceDef& def, const std::vector<ServiceDef>& services);

// The CUDA version the vendored ComfyUI's torch was built for ("13.0"; "" when the venv or torch is not there)
// and the one the driver supports, from nvidia-smi ("" without a driver).
std::string comfyui_torch_cuda();
std::string driver_cuda();
// One sentence on whether the two agree: the driver must support at least torch's version. "" when either is unknown.
std::string cuda_agreement(const std::string& torch_cuda, const std::string& driver_cuda);

}  // namespace maic
