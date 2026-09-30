// `maic doctor`: what this machine has, what MAIC needs, and a recommended local setup.
#include "doctor.hpp"

#include "maic/instructions.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
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

struct Gpu {
    std::string name;
    int vram_mb = 0;  // 0 when unknown
    std::string note;
};

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

std::vector<std::string> ollama_models() {
    std::vector<std::string> out;
    std::string list = run("ollama list 2>/dev/null | tail -n +2 | awk '{print $1}'");
    std::istringstream in(list);
    for (std::string m; std::getline(in, m);) {
        if (!m.empty()) out.push_back(m);
    }
    return out;
}

bool has_model(const std::vector<std::string>& models, const std::string& name) {
    for (const auto& m : models) {
        if (m == name || m.rfind(name + ":", 0) == 0) return true;
    }
    return false;
}

}  // namespace

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
    line("bubblewrap (the command sandbox)", has_program("bwrap"), has_program("bwrap") ? "" : "install bubblewrap; run_shell cannot work without it");
    line("tripwire installed", fs::exists("/usr/local/sbin/maic-lock"), fs::exists("/usr/local/sbin/maic-lock") ? "" : "sudo ./harness/install-tripwire.sh");
    bool ollama_bin = has_program("ollama");
    line("ollama", ollama_bin, ollama_bin ? run("ollama --version 2>/dev/null | tail -1") : "see docs/ollama-setup.md");
    bool ollama_up = false;
    try {
        for (const auto& s : load_services(root_dir() / "services")) {
            if (s.name == "ollama") ollama_up = service_status(s).state != ServiceState::Stopped;
        }
    } catch (const std::exception&) {
    }
    line("ollama running", ollama_up, ollama_up ? "" : "maic up ollama");
    bool clip = has_program("wl-copy") || has_program("xclip") || has_program("xsel");
    line("clipboard tool (wl-copy / xclip / xsel)", clip, clip ? "" : "yanks still reach the terminal through OSC 52");
    line("nvim (for :e)", has_program("nvim"), "");
    for (const auto& e : load_vendor_manifest()) {
        auto st = vendor_status(e);
        line("vendored " + e.name + " (" + (e.kind == "submodule" ? e.ref : e.version) + ")", st.installed, st.installed ? st.target : st.note);
        if (e.name == "llamacpp" && st.installed) line("llama.cpp model", !st.model.empty(), st.model.empty() ? "maic vendor use llamacpp PATH" : st.model);
    }
    std::cout << "\n";

    // ---- models and the recommendation
    std::vector<std::string> models = ollama_bin ? ollama_models() : std::vector<std::string>{};
    std::cout << "models on ollama: ";
    if (models.empty()) std::cout << "none\n";
    else {
        for (size_t i = 0; i < models.size(); ++i) std::cout << (i ? ", " : "") << models[i];
        std::cout << "\n";
    }
    std::cout << "\nrecommendation\n";
    int vram_gb = gpu.vram_mb / 1024;
    std::string quick, deep, why;
    if (vram_gb >= 24) quick = "qwen3.5:9b", deep = "qwen3.5:27b", why = "24 GB or more fits a 27B at 4-bit with room for context";
    else if (vram_gb >= 16) quick = "qwen3.5:9b", deep = "qwen3.5:14b", why = "16 GB fits a 14B at 4-bit";
    else if (vram_gb >= 12) quick = "qwen3.5:4b", deep = "qwen3.5:14b", why = "12 GB fits a 14B at 4-bit, tightly";
    else if (vram_gb >= 8) quick = "qwen3.5:4b", deep = "qwen3.5:9b", why = "8 GB: the 4B fits entirely on the GPU, the 9B mostly (measured 81 and 21 tokens/s on an RTX 2080)";
    else if (vram_gb > 0) quick = "qwen3.5:2b", deep = "qwen3.5:4b", why = "under 8 GB: keep models small so they stay on the GPU";
    else if (ram_gb >= 32) quick = "qwen3.5:4b", deep = "qwen3.5:9b", why = "no usable GPU found; a 4B on CPU is workable, a 9B is slow";
    else quick = "qwen3.5:2b", deep = "qwen3.5:4b", why = "no usable GPU and limited RAM";
    std::cout << "  " << why << "\n";
    // llama.cpp first: every sampler, logit bias and grammars reach it (docs/llamacpp.md). Ollama stays for pulling
    // models, and its blobs are the GGUFs to link.
    auto lc = find_vendor("llamacpp");
    VendorStatus lcs = lc ? vendor_status(*lc) : VendorStatus{};
    std::cout << "  llama.cpp (default, llamacpp/current):  ";
    if (!lcs.installed) std::cout << "not built  ->  maic vendor add llamacpp\n";
    else if (lcs.model.empty()) std::cout << "no GGUF linked yet  ->  maic vendor use llamacpp PATH (an Ollama blob works: ollama show --modelfile " << quick << ")\n";
    else std::cout << lcs.model << "  ->  maic up llamacpp\n";
    std::cout << "  ollama quick model (ollama/" << quick << "):  " << (has_model(models, quick) ? "installed" : "ollama pull " + quick) << "\n";
    std::cout << "  ollama deep model (ollama/" << deep << "):   " << (has_model(models, deep) ? "installed" : "ollama pull " + deep) << "\n";
    if (settings.model != "llamacpp/current") std::cout << "  your settings choose \"" << settings.model << "\"; the default is llamacpp/current\n";
    if (!fs::exists(settings_path())) std::cout << "  no settings file yet: maic settings init\n";
    if (!fs::exists(global_instructions_path())) std::cout << "  no global MAIC.md yet: " << global_instructions_path().string() << " (name, pronouns, standing rules)\n";
    if (avail_gb < 8) std::cout << "  only " << avail_gb << " GB of RAM is free right now; models load faster with more\n";
    std::cout << "  remote models (anthropic/..., deepseek/...) need an API key in the environment; see docs/settings.md\n";
    return 0;
}

}  // namespace maic
