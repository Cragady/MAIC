#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace maic {

// The model catalog (models/catalog.json, docs/models.md): every model MAIC knows how to install, each file
// pinned to a commit with its SHA-256 and size. ~/.config/maic/models.json adds entries or overrides fields by id.
struct CatalogFile {
    std::string name;    // the file's name in the install folder
    std::string url;     // resolve/<revision>/ on Hugging Face
    std::string sha256;
    long size = 0;
    std::string kind;    // weights | mmproj | vad
};

struct CatalogEntry {
    std::string id;
    std::string name;
    std::string role;    // agent | vision | scribe | completion | speech | vad
    std::string brief;
    std::string license;
    std::string license_url;
    std::string repo;
    std::string revision;
    std::vector<CatalogFile> files;
    std::string root;    // llamacpp | whisper | fim: <models_dir>/<root>
    std::string dir;     // the folder under the root ("" for whisper's flat layout)
    std::string shares_entry;  // this entry's weights are a relative link to that entry's file
    std::string shares_file;
    std::vector<std::pair<int, double>> vram;  // context -> estimated GB (context 0: a whisper model, no context)
    int context = 0;
    std::vector<std::string> presets;
    std::string notes;
};

// models/catalog.json under the MAIC root, and the user's own file ($XDG_CONFIG_HOME/maic/models.json).
std::filesystem::path catalog_path();
std::filesystem::path user_catalog_path();

// The shipped catalog with the user's merged in: an entry with a known id replaces the fields it names, an
// unknown id is added. Pure, for load_catalog and its test.
nlohmann::json merge_catalog(const nlohmann::json& shipped, const nlohmann::json& user);
std::vector<CatalogEntry> parse_catalog(const nlohmann::json& j);
std::vector<CatalogEntry> load_catalog();
const CatalogEntry* find_entry(const std::vector<CatalogEntry>& all, const std::string& id);

// Problems with the catalog, offline: missing url, sha256 or size, duplicate ids, a share that does not resolve,
// an unknown role, kind or root, a Hugging Face url not pinned to the revision, a vram figure that disagrees
// with the arithmetic. Empty when it is sound.
std::vector<std::string> check_catalog(const nlohmann::json& shipped, const nlohmann::json& user);

// <models_dir>/<root>, and where an entry's files go.
std::filesystem::path models_root(const std::string& root);
std::filesystem::path entry_dir(const CatalogEntry& e);

// Every file present with the catalog's size (a share: the link resolves to the shared entry's file). No hashing.
bool entry_installed(const CatalogEntry& e, const std::vector<CatalogEntry>& all);
// The entry linked as its root's current model: llamacpp's current-model.gguf, whisper's current.bin, fim's current.gguf.
bool entry_current(const CatalogEntry& e);
// Preset names that use it: the catalog's own list plus any preset in `presets` whose model names its folder.
std::vector<std::string> entry_presets(const CatalogEntry& e, const std::vector<std::pair<std::string, std::string>>& presets);
// The estimate line for an entry ("about 5.4 GB at 8k, 6.4 GB at 16k (estimates)"), from its file sizes.
std::string vram_line(const CatalogEntry& e);

// Installs every file through the checked download (a file already there with the right size is kept, not
// fetched); a share makes the relative link only, installing the shared entry first. `link` makes it its
// root's current model. Progress goes to `out`.
void install_entry(const CatalogEntry& e, const std::vector<CatalogEntry>& all, bool link, std::ostream& out);
// Makes an installed entry its root's current model.
void link_entry(const CatalogEntry& e, std::ostream& out);
// Hashes the files that are present against the catalog; true when every one is present and matches.
bool verify_entry(const CatalogEntry& e, std::ostream& out);
// Installed entries that share this one's files, and why a removal must not go ahead ("" when it may).
std::vector<std::string> dependents(const CatalogEntry& e, const std::vector<CatalogEntry>& all);
std::string remove_blocker(const CatalogEntry& e, const std::vector<CatalogEntry>& all);
// Removes the entry's files (a share: its link) and its folder when nothing else is left in it.
void remove_entry(const CatalogEntry& e, const std::vector<CatalogEntry>& all, std::ostream& out);

}  // namespace maic
