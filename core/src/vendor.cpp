#include "maic/vendor.hpp"

#include "maic/paths.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace maic {

namespace fs = std::filesystem;

namespace {

std::string link_name(const VendorEntry& e) {
    if (e.kind == "submodule") return fs::path(e.path).filename().string();  // vendor/ComfyUI -> ComfyUI
    return e.name;
}

int run(const std::string& command) {
    std::cout << std::flush;
    return std::system(command.c_str());
}

std::string sh(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

void run_or_throw(const std::string& command, const std::string& what) {
    if (run(command) != 0) throw std::runtime_error(what + " failed");
}

std::string install_env(const VendorEntry& e) {
    Settings s = load_settings();
    std::string env = "MAIC_VENDOR=" + sh(vendor_dir().string()) + " MAIC_STATE=" + sh(state_dir().string()) + " MAIC_ROOT=" + sh(root_dir().string());
    if (!s.models_dir.empty()) env += " MAIC_MODELS_DIR=" + sh(s.models_dir);
    if (e.kind == "release") {
        auto expand = [&](std::string t) {
            for (size_t p; (p = t.find("${VERSION}")) != std::string::npos;) t.replace(p, 10, e.version);
            return t;
        };
        env += " OLLAMA_VERSION=" + sh(e.version) + " OLLAMA_URL=" + sh(expand(e.url)) + " OLLAMA_CHECKSUMS=" + sh(expand(e.checksums));
    }
    return env;
}

void run_install(const VendorEntry& e, const char* verb) {
    if (e.install.empty()) return;
    fs::path script = root_dir() / e.install;
    if (!fs::exists(script)) throw std::runtime_error("install script missing: " + script.string());
    run_or_throw("env " + install_env(e) + " bash " + sh(script.string()) + " " + verb, e.name + " " + verb);
}

}  // namespace

std::vector<VendorEntry> load_vendor_manifest() {
    std::ifstream in(root_dir() / "vendor" / "manifest.json");
    if (!in) return {};
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
    std::vector<VendorEntry> out;
    for (const auto& [name, v] : j.items()) {
        if (name == "//" || !v.is_object()) continue;
        VendorEntry e;
        e.name = name;
        e.kind = v.value("kind", "submodule");
        e.path = v.value("path", "");
        e.url = v.value("url", "");
        e.ref = v.value("ref", "");
        e.version = v.value("version", "");
        e.checksums = v.value("checksums", "");
        e.install = v.value("install", "");
        e.description = v.value("description", "");
        e.patches = v.value("patches", std::vector<std::string>{});
        e.needs = v.value("needs", std::vector<std::string>{});
        out.push_back(e);
    }
    return out;
}

std::optional<VendorEntry> find_vendor(const std::string& name) {
    for (const auto& e : load_vendor_manifest()) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

fs::path vendor_dir() {
    return state_dir() / "vendor";
}

fs::path vendor_link(const VendorEntry& e) {
    if (e.kind == "release") return vendor_dir() / e.name / "current";
    return vendor_dir() / link_name(e);
}

VendorStatus vendor_status(const VendorEntry& e) {
    VendorStatus s;
    fs::path link = vendor_link(e);
    std::error_code ec;
    if (fs::is_symlink(link, ec)) {
        s.linked = true;
        s.target = fs::read_symlink(link, ec).string();
        if (!fs::exists(link, ec)) {
            s.note = "link target is missing";
            return s;
        }
    } else if (fs::is_directory(link, ec)) {
        s.linked = true;
        s.target = link.string();
    } else {
        s.note = e.kind == "submodule" ? "maic vendor add " + e.name + " (or adopt an existing checkout)" : "maic vendor add " + e.name + " (or adopt an existing install)";
        return s;
    }
    if (e.install.empty()) {
        s.installed = true;
        return s;
    }
    fs::path script = root_dir() / e.install;
    s.installed = fs::exists(script) && run("env " + install_env(e) + " bash " + sh(script.string()) + " check >/dev/null 2>&1") == 0;
    if (!s.installed) s.note = "linked but not installed: maic vendor add " + e.name;
    return s;
}

void vendor_adopt(const VendorEntry& e, const fs::path& existing) {
    std::error_code ec;
    fs::path target = fs::weakly_canonical(existing, ec);
    if (!fs::is_directory(target, ec)) throw std::runtime_error("not a directory: " + existing.string());
    if (e.kind == "release" && !fs::exists(target / "bin" / e.name, ec)) {
        throw std::runtime_error(target.string() + " has no bin/" + e.name + "; give the version directory (the one holding bin/)");
    }
    if (e.kind == "submodule" && e.name == "comfyui" && !fs::exists(target / "main.py", ec)) {
        throw std::runtime_error(target.string() + " does not look like a ComfyUI checkout (no main.py)");
    }
    fs::path link = vendor_link(e);
    fs::create_directories(link.parent_path());
    if (fs::is_symlink(link, ec)) fs::remove(link);
    else if (fs::exists(link, ec)) throw std::runtime_error(link.string() + " exists and is not a link; remove it first");
    fs::create_directory_symlink(target, link);
    std::cout << link.string() << " -> " << target.string() << "\n";
    // Wire it in (workflows link, custom node link, history link) without touching packages or the network.
    if (!e.install.empty()) run_install(e, "wire");
}

void vendor_add(const VendorEntry& e) {
    require_armed("install vendored services");
    for (const auto& dep : e.needs) {
        auto d = find_vendor(dep);
        if (d && !vendor_status(*d).linked) vendor_add(*d);
    }
    fs::create_directories(vendor_dir());
    std::error_code ec;
    if (e.kind == "submodule") {
        fs::path checkout = root_dir() / e.path;
        std::cout << "fetching " << e.name << " " << e.ref << " into " << checkout.string() << " ...\n";
        run_or_throw("git -C " + sh(root_dir().string()) + " submodule update --init --recursive -- " + sh(e.path), "git submodule update");
        for (const auto& p : e.patches) {
            fs::path patch = root_dir() / p;
            // Already applied on an earlier run: `--reverse --check` succeeds.
            if (run("git -C " + sh(checkout.string()) + " apply --reverse --check " + sh(patch.string()) + " >/dev/null 2>&1") == 0) continue;
            run_or_throw("git -C " + sh(checkout.string()) + " apply " + sh(patch.string()), "applying " + p);
            std::cout << "applied " << p << "\n";
        }
        fs::path link = vendor_link(e);
        if (fs::is_symlink(link, ec)) fs::remove(link);
        if (!fs::exists(link, ec)) fs::create_directory_symlink(checkout, link);
    }
    run_install(e, "install");
}

void vendor_unlink(const VendorEntry& e) {
    std::error_code ec;
    fs::path link = vendor_link(e);
    if (fs::is_symlink(link, ec)) fs::remove(link);
}

}  // namespace maic
