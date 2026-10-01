// `maic setup`: the first run, step by step, out of the pieces that exist (doctor's checks, settings init,
// vendor add, vendor model, the tripwire installer), each behind a question.
#include "setup.hpp"

#include "doctor.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/vendor.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace maic {

namespace fs = std::filesystem;

namespace {

// The GGUFs `maic vendor model` can fetch without a hash lookup: upstream unsloth files, hashes from Hugging
// Face's LFS details (docs/llamacpp.md). A model with a projector lives in a folder named after it.
struct KnownModel {
    const char* id;
    const char* repo;
    const char* sha256;
    const char* mmproj;  // nullptr: text only
    const char* mmproj_sha256;
    const char* size;
};
const KnownModel KNOWN[] = {
    {"Qwen3.5-4B-Q4_K_M", "unsloth/Qwen3.5-4B-GGUF", "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4", "mmproj-F16.gguf",
     "cd88edcf8d031894960bb0c9c5b9b7e1fea6ebee02b9f7ce925a00d12891f864", "2.5 GB plus a 0.8 GB vision projector"},
    {"Qwen3.5-9B-Q4_K_M", "unsloth/Qwen3.5-9B-GGUF", "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8", nullptr, nullptr, "5.3 GB"},
};

std::string hf_url(const KnownModel& m, const std::string& file) {
    return std::string("https://huggingface.co/") + m.repo + "/resolve/main/" + file;
}

long meminfo_gb() {
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("MemTotal:", 0) == 0) return std::atol(line.c_str() + 9) / 1024 / 1024;
    }
    return 0;
}

bool have_model(const std::string& id) {
    for (const auto& m : llamacpp_model_ids()) {
        if (m == id) return true;
    }
    return false;
}

}  // namespace

int run_setup() {
    bool tty = isatty(STDIN_FILENO);
    std::cout << "MAIC setup" << (tty ? "" : " (not a terminal: the plan only, nothing is done)") << "\n\n";
    std::vector<std::string> plan;
    // A yes/no on a terminal; off one, the step goes on the plan and the answer is no.
    auto ask = [&](const std::string& question) {
        if (!tty) {
            plan.push_back(question);
            return false;
        }
        std::cout << question << " [y/N] " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        return line == "y" || line == "Y" || line == "yes";
    };
    auto step = [&](const std::string& what, auto&& work) {
        try {
            work();
        } catch (const std::exception& e) {
            std::cerr << "maic: " << what << ": " << e.what() << "\n";
        }
    };

    std::cout << "prerequisites\n";
    bool missing_required = false;
    for (const auto& c : prerequisites()) {
        std::cout << "  " << (c.ok ? "ok  " : "--  ") << c.what << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
        if (!c.ok && !c.optional && c.what.rfind("tripwire", 0) != 0) missing_required = true;
    }
    if (missing_required) std::cout << "  install what is marked -- first; the steps below assume them\n";
    std::cout << "\n";

    // Settings first: models_dir decides where the GGUFs and ComfyUI's model folders go.
    fs::path lua = settings_path();
    lua.replace_extension(".lua");
    if (!fs::exists(lua) && !fs::exists(settings_path())) {
        if (ask("Write the global settings file (" + lua.string() + ")?")) {
            std::string models_dir;
            fs::path suggested = state_dir() / "models";
            std::cout << "  models directory, where GGUFs and ComfyUI's model folders live [" << suggested.string() << "]: " << std::flush;
            std::getline(std::cin, models_dir);
            if (models_dir.empty()) models_dir = suggested.string();
            if (!models_dir.empty() && models_dir[0] == '~') models_dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + models_dir.substr(1);
            step("settings", [&] {
                write_default_settings(false, models_dir);
                setenv("MAIC_MODELS_DIR", models_dir.c_str(), 1);  // the steps below read ${MAIC_MODELS}
                std::cout << "  wrote " << lua.string() << "\n";
            });
        }
    } else {
        std::cout << "settings: " << (fs::exists(lua) ? lua : settings_path()).string() << " exists\n";
    }

    for (const char* name : {"llamacpp", "comfyui"}) {
        auto e = find_vendor(name);
        if (!e) continue;
        VendorStatus st = vendor_status(*e);
        if (st.installed) {
            std::cout << name << ": installed (" << st.target << ")\n";
            continue;
        }
        std::string what = std::string(name) == "llamacpp" ? "Build llama.cpp, the model server (maic vendor add llamacpp; about ten minutes, no download)?"
                                                             : "Install ComfyUI (maic vendor add comfyui; downloads torch and the requirements, several GB)?";
        if (ask(what)) step(std::string("vendor add ") + name, [&] { vendor_add(*e); });
    }

    // A model: the recommendation for this card, the other known one offered too.
    Recommendation rec = recommend(detect_gpu(), meminfo_gb());
    auto lc = find_vendor("llamacpp");
    std::cout << "models under " << llamacpp_models_root().string() << ": ";
    auto ids = llamacpp_model_ids();
    if (ids.empty()) std::cout << "none\n";
    else {
        for (size_t i = 0; i < ids.size(); ++i) std::cout << (i ? ", " : "") << ids[i];
        std::cout << "\n";
    }
    std::cout << "  this machine: " << rec.why << " (quick " << rec.quick << ", deep " << rec.deep << ")\n";
    for (const auto& m : KNOWN) {
        if (have_model(m.id)) continue;
        std::string id = m.id;
        std::string role = id.rfind(rec.quick, 0) == 0 ? ", the recommended quick model" : id.rfind(rec.deep, 0) == 0 ? ", the recommended deep model" : "";
        std::string q = "Fetch " + id + " (" + m.size + ", checked by SHA-256" + role + ")?";
        if (!lc || !ask(q)) continue;
        step(std::string("vendor model ") + m.id, [&] {
            fs::path into = m.mmproj ? llamacpp_models_root() / m.id : llamacpp_models_root();
            vendor_model(*lc, hf_url(m, std::string(m.id) + ".gguf"), m.sha256, into);
            if (m.mmproj) vendor_model(*lc, hf_url(m, m.mmproj), m.mmproj_sha256, into);
        });
    }

    if (!fs::exists("/usr/local/sbin/maic-lock")) {
        fs::path script = root_dir() / "harness" / "install-tripwire.sh";
        if (ask("Install the tripwire (sudo " + script.string() + "; asks for your password)?")) {
            step("tripwire", [&] {
                if (std::system(("sudo '" + script.string() + "'").c_str()) != 0) throw std::runtime_error("the installer did not finish");
            });
        }
    } else {
        std::cout << "tripwire: installed\n";
    }

    if (!tty) {
        std::cout << "\nplan (each a yes/no in a terminal):\n";
        for (const auto& p : plan) std::cout << "  - " << p << "\n";
        if (plan.empty()) std::cout << "  nothing left to set up\n";
        return 2;
    }
    std::cout << "\n";
    std::vector<ServiceDef> services;
    try {
        services = load_services(root_dir() / "services");
    } catch (const std::exception& e) {
        std::cerr << "maic: " << e.what() << "\n";
    }
    std::cout << format_status(status_report(services));
    std::cout << "next: maic up llamacpp, then maic (maic doctor for the full picture)\n";
    return 0;
}

}  // namespace maic
