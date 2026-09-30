#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maic {

// Services MAIC installs for itself, from vendor/manifest.json. A submodule entry is a pinned git checkout
// inside the MAIC repo; a release entry is a pinned binary download verified against its published checksum.
// Either way the running copy is reached through <state>/vendor/<Name>, which is a symlink: to the submodule
// checkout, to a release directory, or (after `adopt`) to an install the user already had.
struct VendorEntry {
    std::string name;
    std::string kind;  // "submodule" or "release"
    std::string path;  // submodule: repo-relative checkout path
    std::string url;
    std::string ref;       // submodule: tag or commit
    std::string version;   // release
    std::string checksums; // release: URL of the published sha256 file
    std::string install;   // repo-relative script
    std::string description;
    std::vector<std::string> patches;  // repo-relative, applied to a submodule after checkout
    std::vector<std::string> needs;    // other entries installed first
};

std::vector<VendorEntry> load_vendor_manifest();
std::optional<VendorEntry> find_vendor(const std::string& name);

// <state>/vendor
std::filesystem::path vendor_dir();
// <state>/vendor/ComfyUI, <state>/vendor/ollama/current: the link a service uses
std::filesystem::path vendor_link(const VendorEntry& e);

struct VendorStatus {
    bool linked = false;     // the link exists
    bool installed = false;  // the install script's `check` passes (or, without a script, the link resolves)
    std::string target;      // where the link points
    std::string note;
    std::string model;       // llamacpp: the GGUF current-model.gguf points at; empty until `vendor use`
};
VendorStatus vendor_status(const VendorEntry& e);

// Points the link at an install the user already has; nothing is copied or moved. Ollama expects the
// directory that holds bin/ollama; ComfyUI expects the checkout.
void vendor_adopt(const VendorEntry& e, const std::filesystem::path& existing);

// Fetches (submodule update / release download), links, applies patches and runs the install script.
// Network access happens here and only here, because the user asked for it. Output streams to stdout.
void vendor_add(const VendorEntry& e);

// Removes the link only; a checkout or downloaded release stays until the user deletes it.
void vendor_unlink(const VendorEntry& e);

// <state>/vendor/llamacpp/current-model.gguf: the symlink services/llamacpp.json loads. `vendor use` points it
// at a GGUF (by suffix, or by the GGUF magic for a suffixless Ollama blob); nothing is copied.
std::filesystem::path vendor_model_link(const VendorEntry& e);
void vendor_use(const VendorEntry& e, const std::filesystem::path& model);

}  // namespace maic
