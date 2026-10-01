#include "maic/vendor.hpp"

#include "maic/paths.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace maic {

namespace fs = std::filesystem;

namespace {

std::string link_name(const VendorEntry& e) {
    return fs::path(e.path).filename().string();  // vendor/ComfyUI -> ComfyUI
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

std::string install_env() {
    Settings s = load_settings();
    std::string env = "MAIC_VENDOR=" + sh(vendor_dir().string()) + " MAIC_STATE=" + sh(state_dir().string()) + " MAIC_ROOT=" + sh(root_dir().string());
    if (!s.models_dir.empty()) env += " MAIC_MODELS_DIR=" + sh(s.models_dir);
    return env;
}

bool is_gguf(const fs::path& p) {
    if (p.extension() == ".gguf") return true;
    std::ifstream in(p, std::ios::binary);
    char magic[4] = {};
    return in.read(magic, sizeof magic) && std::string_view(magic, sizeof magic) == "GGUF";
}

void run_install(const VendorEntry& e, const char* verb) {
    if (e.install.empty()) return;
    fs::path script = root_dir() / e.install;
    if (!fs::exists(script)) throw std::runtime_error("install script missing: " + script.string());
    run_or_throw("env " + install_env() + " bash " + sh(script.string()) + " " + verb, e.name + " " + verb);
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
        e.path = v.value("path", "");
        e.url = v.value("url", "");
        e.ref = v.value("ref", "");
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
    return vendor_dir() / link_name(e);
}

VendorStatus vendor_status(const VendorEntry& e) {
    VendorStatus s;
    fs::path link = vendor_link(e);
    std::error_code ec;
    if (e.name == "llamacpp") {
        std::string id = llamacpp_current_id();
        if (!id.empty()) s.model = id + "  (" + fs::read_symlink(vendor_model_link(e), ec).string() + ")";
    }
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
        s.note = "maic vendor add " + e.name + " (or adopt an existing checkout)";
        return s;
    }
    if (e.install.empty()) {
        s.installed = true;
        return s;
    }
    fs::path script = root_dir() / e.install;
    s.installed = fs::exists(script) && run("env " + install_env() + " bash " + sh(script.string()) + " check >/dev/null 2>&1") == 0;
    if (!s.installed) s.note = "linked but not installed: maic vendor add " + e.name;
    return s;
}

void vendor_adopt(const VendorEntry& e, const fs::path& existing) {
    std::error_code ec;
    fs::path target = fs::weakly_canonical(existing, ec);
    if (!fs::is_directory(target, ec)) throw std::runtime_error("not a directory: " + existing.string());
    if (e.name == "comfyui" && !fs::exists(target / "main.py", ec)) {
        throw std::runtime_error(target.string() + " does not look like a ComfyUI checkout (no main.py)");
    }
    if (e.name == "llamacpp" && !fs::exists(target / "ggml", ec)) {
        throw std::runtime_error(target.string() + " does not look like a llama.cpp checkout (no ggml/)");
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
    run_install(e, "install");
}

void vendor_unlink(const VendorEntry& e) {
    std::error_code ec;
    fs::path link = vendor_link(e);
    if (fs::is_symlink(link, ec)) fs::remove(link);
}

fs::path vendor_model_link(const VendorEntry& e) {
    return vendor_dir() / e.name / "current-model.gguf";
}

fs::path llamacpp_models_root() {
    Settings s = load_settings();
    return (s.models_dir.empty() ? state_dir() / "models" : fs::path(s.models_dir)) / "llamacpp";
}

namespace {

// A GGUF's router id: the subdirectory's name when it sits in one (the multimodal layout), else its stem.
std::string router_id(const fs::path& gguf, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::weakly_canonical(gguf, ec).lexically_relative(fs::weakly_canonical(root, ec));
    if (rel.empty() || *rel.begin() == "..") return "";
    if (rel.has_parent_path() && !rel.parent_path().empty()) return rel.begin()->string();
    return gguf.stem().string();
}

}  // namespace

std::string llamacpp_current_id() {
    auto e = find_vendor("llamacpp");
    if (!e) return "";
    std::error_code ec;
    fs::path link = vendor_model_link(*e);
    if (!fs::is_symlink(link, ec)) return "";
    return router_id(fs::read_symlink(link, ec), llamacpp_models_root());
}

std::string resolve_model_alias(const std::string& model) {
    if (model != "llamacpp/current") return model;
    std::string id = llamacpp_current_id();
    return id.empty() ? model : "llamacpp/" + id;
}

std::vector<std::string> llamacpp_model_ids() {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path root = llamacpp_models_root();
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) {
            for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                std::string n = f.path().filename().string();
                if (f.path().extension() == ".gguf" && n.rfind("mmproj", 0) != 0) {
                    out.push_back(entry.path().filename().string());
                    break;
                }
            }
        } else if (entry.path().extension() == ".gguf") {
            out.push_back(entry.path().stem().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void vendor_use(const VendorEntry& e, const fs::path& model) {
    if (e.name != "llamacpp") throw std::runtime_error("maic vendor use picks the GGUF for llamacpp; " + e.name + " takes no model");
    std::error_code ec;
    fs::path target = fs::weakly_canonical(model, ec);
    if (!fs::is_regular_file(target, ec)) throw std::runtime_error("not a file: " + model.string());
    if (!is_gguf(target)) throw std::runtime_error(model.string() + " is not a GGUF (no .gguf suffix and no GGUF header)");
    if (router_id(target, llamacpp_models_root()).empty()) {
        throw std::runtime_error("the server only sees GGUFs under " + llamacpp_models_root().string() + "; move or link the file there (a subdirectory named after the file when an mmproj goes with it), or maic vendor model llamacpp URL SHA256");
    }
    fs::path link = vendor_model_link(e);
    fs::create_directories(link.parent_path());
    if (fs::is_symlink(link, ec)) fs::remove(link);
    else if (fs::exists(link, ec)) throw std::runtime_error(link.string() + " exists and is not a link; remove it first");
    fs::create_symlink(target, link);
    std::cout << link.string() << " -> " << target.string() << "\n";
}

fs::path vendor_model(const VendorEntry& e, const std::string& url, const std::string& sha256, const fs::path& into) {
    if (e.name != "llamacpp") throw std::runtime_error("maic vendor model fetches a GGUF for llamacpp; " + e.name + " takes no model");
    require_armed("download a model");
    if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::runtime_error("the SHA-256 is required (64 hex characters; Hugging Face shows it under the file's LFS details), so a bad download is never linked");
    }
    fs::path dir = into;
    if (dir.empty()) {
        Settings s = load_settings();
        dir = llamacpp_models_root();
    }
    fs::create_directories(dir);
    std::string name = url.substr(url.find_last_of('/') + 1);
    if (auto q = name.find('?'); q != std::string::npos) name = name.substr(0, q);
    if (name.empty() || name.find("..") != std::string::npos) throw std::runtime_error("cannot take a file name from " + url);
    fs::path part = dir / (name + ".part"), final = dir / name;
    std::error_code ec;
    if (fs::exists(final, ec)) throw std::runtime_error(final.string() + " already exists; maic vendor use llamacpp " + final.string() + " links it");
    std::cout << "downloading " << name << " ...\n";
    run_or_throw("curl -fL --retry 3 --progress-bar -o " + sh(part.string()) + " " + sh(url), "download");
    std::string want = sha256;
    for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string have;
    {
        FILE* p = popen(("sha256sum " + sh(part.string())).c_str(), "r");
        char buf[128] = "";
        if (p && fgets(buf, sizeof(buf), p)) have = std::string(buf).substr(0, 64);
        if (p) pclose(p);
    }
    if (have != want) {
        fs::remove(part, ec);
        throw std::runtime_error("SHA-256 mismatch for " + name + ": expected " + want + ", got " + (have.empty() ? "nothing" : have) + "; the file was discarded");
    }
    fs::rename(part, final);
    std::cout << "sha256 ok: " << final.string() << "\n";
    if (router_id(final, llamacpp_models_root()).empty()) {
        std::cout << "not under " << llamacpp_models_root().string() << ", so the server will not list it; move it there to use it\n";
    } else {
        vendor_use(e, final);
    }
    return final;
}

}  // namespace maic
