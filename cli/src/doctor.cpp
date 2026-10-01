// `maic doctor`: what this machine has, what MAIC needs, and a recommended local setup.
#include "doctor.hpp"

#include "maic/instructions.hpp"
#include "maic/lazy_lock.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/vendor.hpp"
#include "maic/session.hpp"

#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace maic {

namespace fs = std::filesystem;

namespace {

std::string run(const char* command) {
    FILE* p = popen(command, "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

bool has_program(const std::string& name) {
    return !run(("command -v " + name + " 2>/dev/null").c_str()).empty();
}

long meminfo_kb(const char* key) {
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind(key, 0) == 0) {
            std::istringstream s(line.substr(std::string(key).size()));
            long v = 0;
            s >> v;
            return v;
        }
    }
    return 0;
}

std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    }
    return "unknown";
}

double free_gb(const fs::path& p) {
    struct statvfs st{};
    if (statvfs(p.c_str(), &st) != 0) return -1;
    return static_cast<double>(st.f_bavail) * st.f_frsize / 1e9;
}

bool has_model(const std::vector<std::string>& models, const std::string& name) {
    for (const auto& m : models) {
        if (m == name || m.rfind(name + "-", 0) == 0) return true;
    }
    return false;
}

}  // namespace

Gpu detect_gpu() {
    Gpu g;
    std::string smi = run("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null | head -1");
    if (!smi.empty() && smi.find(',') != std::string::npos) {
        g.name = smi.substr(0, smi.find(','));
        g.vram_mb = std::atoi(smi.substr(smi.find(',') + 1).c_str());
        return g;
    }
    // nvidia-smi is missing or broken (a driver/library mismatch until the next reboot is common): the kernel
    // driver still names the card.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc/driver/nvidia/gpus", ec)) {
        std::ifstream in(e.path() / "information");
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("Model:", 0) == 0) {
                g.name = line.substr(line.find_first_not_of(" \t", 6));
                g.note = "nvidia-smi did not answer (driver/library mismatch? a reboot usually fixes it)";
                // Known sizes for common cards, so the recommendation still works.
                for (const auto& [needle, mb] : std::vector<std::pair<const char*, int>>{
                         {"2080 Ti", 11264}, {"2080", 8192}, {"2070", 8192}, {"2060", 6144}, {"3060", 12288}, {"3070", 8192}, {"3080", 10240},
                         {"3090", 24576}, {"4060", 8192}, {"4070", 12288}, {"4080", 16384}, {"4090", 24576}, {"5070", 12288}, {"5080", 16384}, {"5090", 32768}}) {
                    if (g.name.find(needle) != std::string::npos) {
                        g.vram_mb = mb;
                        g.note += "; VRAM assumed from the model name";
                        break;
                    }
                }
                return g;
            }
        }
    }
    std::string lspci = run("lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1");
    if (!lspci.empty()) {
        g.name = lspci.substr(lspci.find(": ") + 2);
        g.note = "no NVIDIA driver information; VRAM unknown";
    }
    return g;
}

std::vector<Check> prerequisites() {
    std::vector<Check> out;
    auto program = [&](const std::string& name, const std::string& why, bool optional = false) {
        bool ok = has_program(name);
        out.push_back({name + " (" + why + ")", ok, ok ? "" : optional ? "not found; optional" : "install " + name, optional});
    };
    program("python3", "ComfyUI, the helpers beside maic");
    program("git", "maic vendor add fetches submodules");
    program("uv", "ComfyUI's own Python; https://docs.astral.sh/uv/");
    program("curl", "maic vendor model downloads");
    bool bwrap = has_program("bwrap");
    out.push_back({"bubblewrap (the command sandbox)", bwrap, bwrap ? "" : "install bubblewrap; run_shell cannot work without it"});
    bool trip = fs::exists("/usr/local/sbin/maic-lock");
    out.push_back({"tripwire installed", trip, trip ? "" : "sudo ./harness/install-tripwire.sh"});
    program("docker", "services with \"runtime\": \"docker\"", true);
    program("nvidia-smi", "the GPU and its driver", true);
    return out;
}

Recommendation recommend(const Gpu& gpu, long ram_gb) {
    int vram_gb = gpu.vram_mb / 1024;
    if (vram_gb >= 24) return {"Qwen3.5-9B", "Qwen3.5-27B", "24 GB or more fits a 27B at 4-bit with room for context"};
    if (vram_gb >= 16) return {"Qwen3.5-9B", "Qwen3.5-14B", "16 GB fits a 14B at 4-bit"};
    if (vram_gb >= 12) return {"Qwen3.5-4B", "Qwen3.5-14B", "12 GB fits a 14B at 4-bit, tightly"};
    if (vram_gb >= 8) return {"Qwen3.5-4B", "Qwen3.5-9B", "8 GB: the 4B fits entirely on the GPU, the 9B mostly (measured 81 and 21 tokens/s on an RTX 2080)"};
    if (vram_gb > 0) return {"Qwen3.5-2B", "Qwen3.5-4B", "under 8 GB: keep models small so they stay on the GPU"};
    if (ram_gb >= 32) return {"Qwen3.5-4B", "Qwen3.5-9B", "no usable GPU found; a 4B on CPU is workable, a 9B is slow"};
    return {"Qwen3.5-2B", "Qwen3.5-4B", "no usable GPU and limited RAM"};
}

int run_doctor() {
    std::cout << "MAIC doctor\n\n";

    // ---- machine
    long ram_gb = meminfo_kb("MemTotal:") / 1024 / 1024;
    long avail_gb = meminfo_kb("MemAvailable:") / 1024 / 1024;
    std::cout << "machine\n";
    std::cout << "  cpu:   " << cpu_model() << " (" << sysconf(_SC_NPROCESSORS_ONLN) << " threads)\n";
    std::cout << "  ram:   " << ram_gb << " GB (" << avail_gb << " GB available now)\n";
    Gpu gpu = detect_gpu();
    if (gpu.name.empty()) std::cout << "  gpu:   none detected\n";
    else std::cout << "  gpu:   " << gpu.name << (gpu.vram_mb ? " " + std::to_string(gpu.vram_mb / 1024) + " GB VRAM" : " (VRAM unknown)") << "\n";
    if (!gpu.note.empty()) std::cout << "         " << gpu.note << "\n";
    std::cout << "  disk:  " << static_cast<int>(free_gb(std::getenv("HOME"))) << " GB free in $HOME";
    Settings settings = load_settings();
    std::cout << ", " << static_cast<int>(free_gb(state_dir().parent_path().parent_path())) << " GB free where sessions live\n\n";

    // ---- tools MAIC relies on
    std::cout << "tools\n";
    auto line = [&](const std::string& what, bool ok, const std::string& detail) {
        std::cout << "  " << (ok ? "ok  " : "--  ") << what << (detail.empty() ? "" : ": " + detail) << "\n";
    };
    for (const auto& c : prerequisites()) line(c.what, c.ok, c.detail);
    for (const auto& e : load_vendor_manifest()) {
        auto st = vendor_status(e);
        line("vendored " + e.name + " (" + e.ref + ")", st.installed, st.installed ? st.target : st.note);
        if (e.name == "llamacpp" && st.installed) line("llama.cpp model", !st.model.empty(), st.model.empty() ? "maic vendor use llamacpp PATH" : st.model);
    }
    // The venv's torch against the driver: a cu130 wheel on a driver below 580 fails at the first CUDA call.
    if (std::string torch = comfyui_torch_cuda(); !torch.empty()) {
        std::string driver = driver_cuda();
        std::string verdict = cuda_agreement(torch, driver);
        line("ComfyUI torch vs driver", !driver.empty() && verdict.find("they agree") != std::string::npos, driver.empty() ? "torch is built for CUDA " + torch + "; nvidia-smi did not answer" : verdict);
    }
    std::vector<ServiceDef> services;
    try {
        services = load_services(root_dir() / "services");
    } catch (const std::exception&) {
    }
    for (const auto& s : services) {
        if (!is_llama_server(s.name)) continue;
        bool up = service_status(s).state != ServiceState::Stopped;
        line(s.name + " running", up, up ? "" : "maic up " + s.name + (s.name == "llamacpp" ? "" : " (the side server: a second resident model for the deep pass and the reviewer)"));
    }
    bool clip = has_program("wl-copy") || has_program("xclip") || has_program("xsel");
    line("clipboard tool (wl-copy / xclip / xsel)", clip, clip ? "" : "yanks still reach the terminal through OSC 52");
    line("nvim (for :e)", has_program("nvim"), "");
    LazyLockState lock = lazy_lock_state(lazy_lock_path(settings.lazy_lock));
    if (std::string s = lazy_lock_summary(lock); !s.empty()) line("nvim lazy-lock.json", lock.kind == LazyLockState::Kind::InSync, s);
    std::cout << "\n";

    // ---- models and the recommendation
    std::vector<std::string> models = llamacpp_model_ids();
    std::cout << "models under " << llamacpp_models_root().string() << " (llama.cpp serves them by file name): ";
    if (models.empty()) std::cout << "none\n";
    else {
        for (size_t i = 0; i < models.size(); ++i) std::cout << (i ? ", " : "") << models[i];
        std::cout << "\n";
    }
    std::cout << "\nrecommendation\n";
    auto [quick, deep, why] = recommend(gpu, ram_gb);
    std::cout << "  " << why << "\n";
    auto lc = find_vendor("llamacpp");
    VendorStatus lcs = lc ? vendor_status(*lc) : VendorStatus{};
    std::cout << "  llama.cpp (default, llamacpp/current):  ";
    if (!lcs.installed) std::cout << "not built  ->  maic vendor add llamacpp\n";
    else if (lcs.model.empty()) std::cout << "no GGUF linked yet  ->  maic vendor use llamacpp PATH, or maic vendor model llamacpp URL SHA256 (docs/llamacpp.md)\n";
    else std::cout << lcs.model << "  ->  maic up llamacpp\n";
    // Q4_K_M of these from unsloth/<name>-GGUF on Hugging Face; the file's stem is the model id.
    std::cout << "  quick model (" << quick << " at Q4_K_M):  " << (has_model(models, quick) ? "installed" : "maic vendor model llamacpp URL SHA256") << "\n";
    std::cout << "  deep model (" << deep << " at Q4_K_M):   " << (has_model(models, deep) ? "installed" : "maic vendor model llamacpp URL SHA256") << "\n";
    if (settings.model != "llamacpp/current") std::cout << "  your settings choose \"" << settings.model << "\"; the default is llamacpp/current\n";
    // Two servers, two resident models: whether the pair fits the card, from the GGUF sizes and the contexts.
    if (std::string fit = gpu_budget(gpu_report(services), settings, gpu.vram_mb > 0 ? static_cast<long>(gpu.vram_mb) * 1024 * 1024 : -1); !fit.empty()) {
        std::cout << "  two servers: " << fit << "\n";
    }
    if (!fs::exists(settings_path())) std::cout << "  no settings file yet: maic settings init (or maic setup for the whole first run)\n";
    if (!fs::exists(global_instructions_path())) std::cout << "  no global MAIC.md yet: " << global_instructions_path().string() << " (name, pronouns, standing rules)\n";
    if (avail_gb < 8) std::cout << "  only " << avail_gb << " GB of RAM is free right now; models load faster with more\n";
    std::cout << "  remote models (anthropic/..., deepseek/...) need an API key in the environment; see docs/settings.md\n";
    return 0;
}

}  // namespace maic
