#pragma once

#include <filesystem>
#include <map>
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
    std::map<std::string, std::string> models;  // comfyui: model category -> subfolder under models_dir, for extra_model_paths.yaml
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
    std::string model;       // llamacpp: the GGUF current-model.gguf points at; whisper: the file current.bin points at; empty until `vendor use`
};
VendorStatus vendor_status(const VendorEntry& e);

// Points the link at a checkout the user already has; nothing is copied or moved.
void vendor_adopt(const VendorEntry& e, const std::filesystem::path& existing);

// Wires a linked checkout in without the network: the install script's `wire` step (custom node links,
// workflows folder), and for comfyui the `maic:` block of extra_model_paths.yaml regenerated from the
// manifest's models map and models_dir. Idempotent; `add` and `adopt` run it too.
void vendor_wire(const VendorEntry& e);
// The yaml with its `maic:` root key replaced (or added) by one that maps every category in `models` under
// `base_path`; every other root key and comment is kept as it was. Pure, for vendor_wire and its test.
std::string merge_model_paths_yaml(const std::string& existing, const std::string& base_path, const std::map<std::string, std::string>& models);

// Fetches (submodule update), links, applies patches and runs the install script.
// Network access happens here and only here, because the user asked for it. Output streams to stdout.
void vendor_add(const VendorEntry& e);

// Removes the link only; the checkout stays.
void vendor_unlink(const VendorEntry& e);

// <state>/vendor/llamacpp/current-model.gguf: the symlink services/llamacpp.json loads. `vendor use` points it
// at a GGUF (by suffix, or by the GGUF magic when the file has none); nothing is copied. For whisper it is
// <models_dir>/whisper/current.bin, the ggml file services/whisper.json loads.
std::filesystem::path vendor_model_link(const VendorEntry& e);
// <models_dir>/whisper: the ggml speech models, and the current.bin link whisper-server loads.
std::filesystem::path whisper_models_root();
// <models_dir>/llamacpp: what the router serves. Every GGUF there (or a subdirectory holding one plus an
// mmproj) is a model whose id is the file's stem.
std::filesystem::path llamacpp_models_root();
// The router id of the GGUF `vendor use` linked as current, "" when none.
std::string llamacpp_current_id();
// "llamacpp/current" -> "llamacpp/<id of the linked GGUF>"; anything else unchanged. "" id leaves it as is.
std::string resolve_model_alias(const std::string& model);
// <models_dir>/fim: the code completion models services/llamacpp-fim.json serves, each in a folder, and
// current.gguf, a relative link to the one `maic models install ID --link` chose (the router's model "current").
std::filesystem::path fim_models_root();
std::filesystem::path fim_model_link();
// The folder current.gguf points into, "" when none is linked.
std::string fim_current_id();
// Router ids of the GGUFs under the models root, sorted.
std::vector<std::string> llamacpp_model_ids();
// Links `model` as the entry's current model; for llamacpp, a GGUF under <models_dir>/fim becomes llamacpp-fim's
// current.gguf instead (the caller reloads a running completion server: reload_fim).
void vendor_use(const VendorEntry& e, const std::filesystem::path& model);

// Downloads a GGUF with curl into `into` (default: <models_dir>/llamacpp; for whisper a ggml .bin into <models_dir>/whisper),
// refuses to keep it unless its SHA-256 matches `sha256`, then links it as the current model. The only
// network access in MAIC besides `vendor add`, and only because the user typed it.
std::filesystem::path vendor_model(const VendorEntry& e, const std::string& url, const std::string& sha256, const std::filesystem::path& into = {});
// The checked download under both: curl into <dir>/<name>.part, kept as <dir>/<name> only when its SHA-256 is
// `sha256`, else discarded. `name` "" takes it from the URL. Refuses an existing file; links nothing.
std::filesystem::path download_verified(const std::string& url, const std::string& sha256, const std::filesystem::path& dir, std::string name = "");
// sha256sum's hex digest of a file (through a link), "" when it cannot be read.
std::string file_sha256(const std::filesystem::path& p);

}  // namespace maic
