#pragma once

#include "maic/llm.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/theme.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace maic {

// One line of `maic status` / `:status`, with what the user could do about it.
struct ServiceReport {
    std::string name;
    std::string state;    // running, stopped, foreign, starting, failed (it exited without `maic down`)
    std::string runtime;  // host (a process MAIC started), docker (a container it started)
    std::string where;    // "pid 1234 · http://127.0.0.1:8081", "container maic-comfyui · ...", or the port for a foreign process
    std::string detail;   // what it holds, when it answers: a llama server's resident model, ComfyUI's VRAM and queue
    std::string log;      // path of the log MAIC keeps for it
    std::vector<std::string> actions;  // quick actions, as commands the user can run
    bool gpu = false;     // the definition says it uses the card (needs_gpu, or a docker service run with --gpus all)
    std::string who;      // "pid 1234" or "container maic-comfyui" while it runs; "" otherwise
    std::string url;      // where it listens, "" for a service without a port
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

// Colour for `maic status`: each state and the GPU tag take the style of a role of the settings' theme
// (harness_armed, harness_tripped, notice, status_dim, error, focus), at the terminal's colour depth.
struct StatusPaint {
    const Settings& settings;
    ColorDepth depth;
};

// Plain-text rendering shared by the CLI command and the TUI; `paint` adds colour (the CLI, on a terminal).
std::string format_status(const StatusReport& report, const StatusPaint* paint = nullptr);

// `maic status --text-base`: one record per line, fields separated by a tab, in a fixed order, "-" for an empty
// field, no colour and no prose:
//   harness  armed|tripped
//   service  NAME  STATE  RUNTIME  gpu|cpu  WHO  URL  DETAIL
//   lazy-lock  TEXT        (only when there is something to say)
std::string format_status_records(const StatusReport& report);

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

// The llama servers: `llamacpp`, `llamacpp-2` and `llamacpp-fim` (services/llamacpp*.json), one resident model each.
bool is_llama_server(const std::string& service_name);
// The code completion server (services/llamacpp-fim.json, port 8084): a router over <models_dir>/fim that serves
// /infill to llama.vim, with no chat provider.
bool is_fim_server(const std::string& service_name);

// Before starting `def`: when it needs the GPU, every llama server unloads what it holds and a running ComfyUI
// (when `def` is not ComfyUI) is asked to unload its models; a running whisper, which cannot unload without
// stopping, is named. A llama server itself loads nothing at start, so starting one never evicts the other.
// Returns a notice ("" when nothing had to happen).
std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services);

// Who holds the card, without nvidia-smi where possible: each llama server's resident models and ComfyUI's
// own VRAM figures.
struct GpuReport {
    struct Server {
        std::string name;                 // llamacpp, llamacpp-2, llamacpp-fim
        bool running = false;
        std::vector<std::string> models;  // resident ("" entries never); the completion server's "current" as the folder it links
        int context = 0;                  // the completion server's --ctx-size; the others take theirs from settings
        std::string linked;               // the completion server's linked coder ("" when none is linked)
    };
    std::vector<Server> servers;   // every llama server, in service order
    bool comfyui_running = false;
    long comfyui_vram_used = -1;   // bytes, from ComfyUI's /system_stats; -1 when unknown
    long comfyui_vram_total = -1;
    long card_total = -1;          // bytes: ComfyUI's figure, else nvidia-smi's, else -1
    // The whisper server (services/whisper.json), when there is one: it loads its model at start and keeps it.
    bool has_whisper = false;
    bool whisper_running = false;
    std::string whisper_model;     // the file <models_dir>/whisper/current.bin points at; "" when none is linked
    long whisper_bytes = -1;       // its size on disk
    std::string text() const;      // a few lines for a person
    // `maic gpu --text-base`: one tab-separated record per line, "-" for an empty field:
    //   server  NAME  running|stopped  MODELS (comma separated)  CONTEXT  LINKED
    //   comfyui  running|stopped  VRAM_USED_BYTES  VRAM_TOTAL_BYTES   (-1 when unknown)
    //   whisper  running|stopped  MODEL  BYTES   (only when there is a whisper service)
    //   card  TOTAL_BYTES
    std::string records() const;
};
GpuReport gpu_report(const std::vector<ServiceDef>& services);
// Frees what can be freed without stopping anything: a llama server unloads its models, ComfyUI unloads its
// models and releases cached memory (its /free route); whisper cannot, and says so. `what` is "all", a llama
// server's name, "whisper" or "comfyui".
std::string gpu_free(const std::vector<ServiceDef>& services, const std::string& what = "all");

// The completion server runs with --no-models-autoload: llama.vim asks at every pause in typing, so a coder
// unloaded for ComfyUI or whisper would otherwise come straight back behind their backs. MAIC loads its model
// "current" itself instead: after maic up, on maic gpu load, after a relink, and when the last service that
// unloaded it (free_gpu_for keeps the list under the state directory) stops through MAIC.
// `load_fim` asks the router to load "current" and waits up to `timeout` until it is loaded; on failure it
// throws with the reason and the card's budget sentence.
std::string load_fim(const ServiceDef& def, const std::vector<ServiceDef>& services, std::chrono::seconds timeout = std::chrono::seconds(120));
// `maic gpu load NAME`: only llamacpp-fim waits to be asked; anything else is an error naming it.
std::string gpu_load(const std::vector<ServiceDef>& services, const std::string& what);
// After fim's current.gguf was relinked: a running completion server that held a coder unloads it and loads the
// new link. "" when it is not running; a notice when it held nothing (the link waits for maic gpu load).
std::string reload_fim(const std::vector<ServiceDef>& services);
// After `def` stopped through MAIC: when it was the last service that had unloaded the coder, load it again.
// Stopping llamacpp-fim itself forgets the list. Returns a notice, "" when nothing happened.
std::string restore_gpu_after(const ServiceDef& def, const std::vector<ServiceDef>& services);

// One model as a server holds it, for the budget sentence.
struct ModelPlan {
    std::string id;   // the router id (file stem or folder name)
    int context = 0;  // tokens
    std::string label;      // set with `bytes` for a model that is not a GGUF under the root (whisper's)
    long bytes = -1;        // its footprint as given, instead of one computed from `id` and `context`
};
// What a plan takes on the card: the GGUF's size on disk (an mmproj beside it counts) plus a rough KV cache
// estimate from the context and the parameter count in the name (65 MB per 1k tokens for a 4B, 130 for a
// 9B). -1 when no such model is under `models_root`.
long model_footprint(const ModelPlan& plan, const std::filesystem::path& models_root);
// The arithmetic under it, for weights already summed (the catalog's vram figures use it too).
long estimate_footprint(long weight_bytes, const std::string& id, int context);
// One plain sentence: "4B at 16k (3.6 GB est.) + 4B at 8k (3.1 GB est.) = 6.7 GB of 8.0 GB: fits with ComfyUI
// stopped". `card_total` and `comfyui_used` in bytes, -1 when unknown. "" when no plan has a file.
std::string budget_sentence(const std::vector<ModelPlan>& plans, const std::filesystem::path& models_root, long card_total, bool comfyui_running, long comfyui_used);
// The sentence for this machine: each llama server's resident model, else the one settings would send it
// (`model` and `context` for llamacpp; `reviewer_model` on llamacpp-2, else the same model, with `context_2`),
// the whisper server's model when one is linked (its file plus an estimate for its buffers), and the completion
// server's model while it is loaded.
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

// The context window of a llama server ("llamacpp" or "llamacpp-2"): exports ${MAIC_CONTEXT} or ${MAIC_CONTEXT_2}
// for its service file and sizes the matching provider's readout.
void set_context(std::vector<Provider>& providers, int tokens, const std::string& service = "llamacpp");

// When `query` names a preset: sets settings.model, thinking, the provider's context_window and, for a local
// llama.cpp model, settings.context. The reviewer follows from the preset in the agent (reviewer_pick); a
// reviewer_model in settings stays the user's pin. Returns the preset's name, "" when none matched.
std::string apply_preset(Settings& settings, const std::string& query);

// The `:model` listing of presets: one line each with tier, limited, the subagent pick and the reviewer.
std::string preset_lines(const Settings& settings);

// When the llama server `service` is running with another command than its file now gives (a new context
// size), restarts it. Returns a notice, "" when nothing had to happen.
std::string restart_llamacpp_if_changed(const std::string& service = "llamacpp");

}  // namespace maic
