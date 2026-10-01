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

std::string capture(const std::string& command) {
    FILE* p = popen(command.c_str(), "r");
    if (!p) return "";
    char buf[512] = "";
    std::string out = fgets(buf, sizeof(buf), p) ? buf : "";
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
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

// A whisper.cpp model: a .bin, or by the ggml magic (0x67676d6c, little-endian) when the name says nothing.
bool is_ggml(const fs::path& p) {
    if (p.extension() == ".bin") return true;
    std::ifstream in(p, std::ios::binary);
    char magic[4] = {};
    return in.read(magic, sizeof magic) && std::string_view(magic, sizeof magic) == "lmgg";
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
        e.models = v.value("models", std::map<std::string, std::string>{});
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
    } else if (e.name == "whisper" && fs::is_symlink(vendor_model_link(e), ec)) {
        s.model = fs::read_symlink(vendor_model_link(e), ec).string();
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
    if (e.name == "whisper" && !fs::exists(target / "examples" / "server", ec)) {
        throw std::runtime_error(target.string() + " does not look like a whisper.cpp checkout (no examples/server/)");
    }
    fs::path link = vendor_link(e);
    fs::create_directories(link.parent_path());
    if (fs::is_symlink(link, ec)) fs::remove(link);
    else if (fs::exists(link, ec)) throw std::runtime_error(link.string() + " exists and is not a link; remove it first");
    fs::create_directory_symlink(target, link);
    std::cout << link.string() << " -> " << target.string() << "\n";
    // What the checkout is, from git: its origin against the upstream name, its ref against the manifest's.
    std::string origin = capture("git -C " + sh(target.string()) + " config --get remote.origin.url 2>/dev/null");
    std::string upstream = fs::path(e.url).filename().string();  // ComfyUI, llama.cpp
    if (!origin.empty() && origin.find(upstream) == std::string::npos) std::cout << "note: its origin is " << origin << ", not a " << upstream << " repository\n";
    if (std::string head = capture("git -C " + sh(target.string()) + " describe --tags --always 2>/dev/null"); !head.empty()) {
        std::cout << "checked out " << head << (head == e.ref ? " (the manifest's ref)" : ", the manifest pins " + e.ref) << "\n";
    }
    vendor_wire(e);
}

std::string merge_model_paths_yaml(const std::string& existing, const std::string& base_path, const std::map<std::string, std::string>& models) {
    // Drop the old maic: block: its key line and every indented or blank line after it, up to the next root key.
    std::string kept;
    bool in_block = false;
    size_t pos = 0;
    while (pos < existing.size()) {
        size_t nl = existing.find('\n', pos);
        std::string line = existing.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? existing.size() : nl + 1;
        bool root_key = !line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '#';
        if (root_key) in_block = line == "maic:" || line.rfind("maic: ", 0) == 0;
        if (!in_block) kept += line + "\n";
    }
    while (kept.size() > 1 && kept.compare(kept.size() - 2, 2, "\n\n") == 0) kept.pop_back();
    if (kept == "\n") kept.clear();
    std::string base = base_path;
    if (base.empty() || base.back() != '/') base += '/';
    std::string block = "maic:\n    base_path: \"" + base + "\"\n";
    for (const auto& [category, folder] : models) block += "    " + category + ": " + folder + (folder.empty() || folder.back() == '/' ? "" : "/") + "\n";
    return kept + (kept.empty() ? "" : "\n") + block;
}

void vendor_wire(const VendorEntry& e) {
    if (e.name == "comfyui" && !e.models.empty()) {
        Settings s = load_settings();
        if (s.models_dir.empty()) throw std::runtime_error("models_dir is not set in settings (maic settings init), so extra_model_paths.yaml cannot be written");
        fs::path yaml = vendor_link(e) / "extra_model_paths.yaml";
        std::ifstream in(yaml);
        std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::string merged = merge_model_paths_yaml(existing, s.models_dir, e.models);
        if (merged != existing) {
            fs::path tmp = yaml;
            tmp += ".tmp";
            std::ofstream(tmp, std::ios::trunc) << merged;
            fs::rename(tmp, yaml);
            std::cout << "wrote the maic: block of " << yaml.string() << " -> " << s.models_dir << " (" << e.models.size() << " categories)\n";
        }
    }
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
    vendor_wire(e);
}

void vendor_unlink(const VendorEntry& e) {
    std::error_code ec;
    fs::path link = vendor_link(e);
    if (fs::is_symlink(link, ec)) fs::remove(link);
}

fs::path vendor_model_link(const VendorEntry& e) {
    if (e.name == "whisper") return whisper_models_root() / "current.bin";
    return vendor_dir() / e.name / "current-model.gguf";
}

fs::path llamacpp_models_root() {
    Settings s = load_settings();
    return (s.models_dir.empty() ? state_dir() / "models" : fs::path(s.models_dir)) / "llamacpp";
}

fs::path whisper_models_root() {
    Settings s = load_settings();
    return (s.models_dir.empty() ? state_dir() / "models" : fs::path(s.models_dir)) / "whisper";
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
    if (e.name != "llamacpp" && e.name != "whisper") throw std::runtime_error("maic vendor use picks the model for llamacpp or whisper; " + e.name + " takes no model");
    std::error_code ec;
    fs::path target = fs::weakly_canonical(model, ec);
    if (!fs::is_regular_file(target, ec)) throw std::runtime_error("not a file: " + model.string());
    if (e.name == "whisper") {
        if (!is_ggml(target)) throw std::runtime_error(model.string() + " is not a whisper.cpp model (no .bin suffix and no ggml header)");
        if (target.filename().string().rfind("ggml-silero", 0) == 0) throw std::runtime_error(target.filename().string() + " is the VAD model: services/whisper.json loads it by its own name from " + whisper_models_root().string() + ", beside the speech model");
    } else if (!is_gguf(target)) {
        throw std::runtime_error(model.string() + " is not a GGUF (no .gguf suffix and no GGUF header)");
    }
    if (e.name == "llamacpp" && router_id(target, llamacpp_models_root()).empty()) {
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
    if (e.name != "llamacpp" && e.name != "whisper") throw std::runtime_error("maic vendor model fetches a model for llamacpp or whisper; " + e.name + " takes no model");
    require_armed("download a model");
    if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::runtime_error("the SHA-256 is required (64 hex characters; Hugging Face shows it under the file's LFS details), so a bad download is never linked");
    }
    fs::path dir = into;
    if (dir.empty()) dir = e.name == "whisper" ? whisper_models_root() : llamacpp_models_root();
    fs::create_directories(dir);
    std::string name = url.substr(url.find_last_of('/') + 1);
    if (auto q = name.find('?'); q != std::string::npos) name = name.substr(0, q);
    if (name.empty() || name.find("..") != std::string::npos) throw std::runtime_error("cannot take a file name from " + url);
    fs::path part = dir / (name + ".part"), final = dir / name;
    std::error_code ec;
    if (fs::exists(final, ec)) throw std::runtime_error(final.string() + " already exists; maic vendor use " + e.name + " " + final.string() + " links it");
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
    if (e.name == "whisper" && name.rfind("ggml-silero", 0) == 0) {
        std::cout << "the VAD model: services/whisper.json loads it by this name; the current speech model is unchanged\n";
    } else if (e.name == "whisper") {
        vendor_use(e, final);
    } else if (name.rfind("mmproj", 0) == 0) {
        std::cout << "a vision projector: the server loads it with the GGUF in the same folder; the current model is unchanged\n";
    } else if (router_id(final, llamacpp_models_root()).empty()) {
        std::cout << "not under " << llamacpp_models_root().string() << ", so the server will not list it; move it there to use it\n";
    } else {
        vendor_use(e, final);
    }
    return final;
}

}  // namespace maic
