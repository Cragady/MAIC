#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maic {

// Services MAIC installs for itself, from vendor/manifest.json: each is a git submodule pinned to a tag or
// commit inside the MAIC repo. The running copy is reached through <state>/vendor/<Name>, a symlink to the
// submodule checkout or (after `adopt`) to a checkout the user already had.
struct VendorEntry {
    std::string name;
    std::string path;  // repo-relative checkout path
    std::string url;
    std::string ref;       // tag or commit
    std::string install;   // repo-relative script
    std::string description;
    std::vector<std::string> patches;  // repo-relative, applied to a submodule after checkout
    std::vector<std::string> needs;    // other entries installed first
};

std::vector<VendorEntry> load_vendor_manifest();
std::optional<VendorEntry> find_vendor(const std::string& name);

// <state>/vendor
std::filesystem::path vendor_dir();
// <state>/vendor/ComfyUI, <state>/vendor/llama.cpp: the link a service uses
std::filesystem::path vendor_link(const VendorEntry& e);

struct VendorStatus {
    bool linked = false;     // the link exists
    bool installed = false;  // the install script's `check` passes (or, without a script, the link resolves)
    std::string target;      // where the link points
    std::string note;
    std::string model;       // llamacpp: the GGUF current-model.gguf points at; empty until `vendor use`
};
VendorStatus vendor_status(const VendorEntry& e);

// Points the link at a checkout the user already has; nothing is copied or moved.
void vendor_adopt(const VendorEntry& e, const std::filesystem::path& existing);

// Fetches (submodule update), links, applies patches and runs the install script.
// Network access happens here and only here, because the user asked for it. Output streams to stdout.
void vendor_add(const VendorEntry& e);

// Removes the link only; the checkout stays.
void vendor_unlink(const VendorEntry& e);

// <state>/vendor/llamacpp/current-model.gguf: the symlink services/llamacpp.json loads. `vendor use` points it
// at a GGUF (by suffix, or by the GGUF magic when the file has none); nothing is copied.
std::filesystem::path vendor_model_link(const VendorEntry& e);
// <models_dir>/llamacpp: what the router serves. Every GGUF there (or a subdirectory holding one plus an
// mmproj) is a model whose id is the file's stem.
std::filesystem::path llamacpp_models_root();
// The router id of the GGUF `vendor use` linked as current, "" when none.
std::string llamacpp_current_id();
// "llamacpp/current" -> "llamacpp/<id of the linked GGUF>"; anything else unchanged. "" id leaves it as is.
std::string resolve_model_alias(const std::string& model);
// Router ids of the GGUFs under the models root, sorted.
std::vector<std::string> llamacpp_model_ids();
void vendor_use(const VendorEntry& e, const std::filesystem::path& model);

// Downloads a GGUF with curl into `into` (default: <models_dir>/llamacpp, else <state>/vendor/llamacpp/models),
// refuses to keep it unless its SHA-256 matches `sha256`, then links it as the current model. The only
// network access in MAIC besides `vendor add`, and only because the user typed it.
std::filesystem::path vendor_model(const VendorEntry& e, const std::string& url, const std::string& sha256, const std::filesystem::path& into = {});

}  // namespace maic
