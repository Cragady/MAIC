// `maid setup`: the first run, step by step, out of the pieces that exist (doctor's checks, settings init,
// vendor add, the model catalog, the tripwire installer), each behind a question.
#include "setup.hpp"

#include "doctor.hpp"
#include "maid/helper.hpp"
#include "maid/models.hpp"
#include "maid/paths.hpp"
#include "maid/service.hpp"
#include "maid/settings.hpp"
#include "maid/status.hpp"
#include "maid/vendor.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace maid {

namespace fs = std::filesystem;

namespace {

long meminfo_gb() {
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("MemTotal:", 0) == 0) return std::atol(line.c_str() + 9) / 1024 / 1024;
    }
    return 0;
}

}  // namespace

int run_setup() {
    bool tty = isatty(STDIN_FILENO);
    std::cout << "MAID setup" << (tty ? "" : " (not a terminal: the plan only, nothing is done)") << "\n\n";
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
            std::cerr << "maid: " << what << ": " << e.what() << "\n";
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
                setenv("MAID_MODELS_DIR", models_dir.c_str(), 1);  // the steps below read ${MAID_MODELS}
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
        std::string what = std::string(name) == "llamacpp" ? "Build llama.cpp, the model server (maid vendor add llamacpp; about ten minutes, no download)?"
                                                             : "Install ComfyUI (maid vendor add comfyui; downloads torch and the requirements, several GB)?";
        if (ask(what)) step(std::string("vendor add ") + name, [&] { vendor_add(*e); });
    }

    // A model from the catalog (models/catalog.json, docs/models.md): the agent and vision GGUFs, the
    // recommendation for this card marked. Each install checks every file's SHA-256 and keeps what is there.
    Recommendation rec = recommend(detect_gpu(), meminfo_gb());
    std::cout << "models under " << llamacpp_models_root().string() << ": ";
    auto ids = llamacpp_model_ids();
    if (ids.empty()) std::cout << "none\n";
    else {
        for (size_t i = 0; i < ids.size(); ++i) std::cout << (i ? ", " : "") << ids[i];
        std::cout << "\n";
    }
    std::cout << "  this machine: " << rec.why << " (quick " << rec.quick << ", deep " << rec.deep << ")\n";
    std::vector<CatalogEntry> catalog;
    try {
        catalog = load_catalog();
    } catch (const std::exception& e) {
        std::cerr << "maid: model catalog: " << e.what() << "\n";
    }
    for (const auto& m : catalog) {
        if (m.root != "llamacpp" || (m.role != "agent" && m.role != "vision") || entry_installed(m, catalog)) continue;
        long bytes = 0;
        for (const auto& f : m.files) bytes += m.shares_entry.empty() ? f.size : 0;
        char size[32];
        snprintf(size, sizeof(size), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
        std::string what = m.shares_entry.empty() ? std::string(size) : "a link to " + m.shares_entry + "'s weights";
        std::string role = m.dir.rfind(rec.quick + "-", 0) == 0 ? ", the recommended quick model" : m.dir.rfind(rec.deep + "-", 0) == 0 ? ", the recommended deep model" : "";
        if (!ask("Fetch " + m.id + " (" + what + ", " + m.role + ", checked by SHA-256" + role + "; maid models info " + m.id + ")?")) continue;
        step("models install " + m.id, [&] { install_entry(m, catalog, llamacpp_current_id().empty(), std::cout); });
    }

    if (!fs::exists("/usr/local/sbin/maid-lock")) {
        fs::path script = root_dir() / "harness" / "install-tripwire.sh";
        if (ask("Install the tripwire (sudo " + script.string() + "; asks for your password)?")) {
            step("tripwire", [&] {
                if (run_helper("sudo '" + script.string() + "'") != 0) throw std::runtime_error("the installer did not finish");
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
        std::cerr << "maid: " << e.what() << "\n";
    }
    std::cout << format_status(status_report(services));
    std::cout << "next: maid up llamacpp, then maid (maid doctor for the full picture)\n";
    return 0;
}

}  // namespace maid
