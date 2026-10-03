#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maic::server {

// Artifacts: pages maic-server serves at /a/<id>/ from <state>/artifacts/<id>/, sandboxed (docs/artifacts.md).

constexpr size_t kArtifactDataMax = 1 << 20;  // a data document's size cap, 1 MiB

// An artifact id or a data document's name: [A-Za-z0-9][A-Za-z0-9_-]{0,63}.
bool artifact_name_ok(std::string_view name);

// The file a relative path names inside an artifact folder ("" is index.html), or nothing: every component a safe
// name (letters, digits, `_`, `-`, `.`, never a leading dot), and the path with symlinks resolved still inside the
// folder and free of dotfiles.
std::optional<std::filesystem::path> artifact_file(const std::filesystem::path& dir, std::string_view rel);

std::string artifact_content_type(const std::filesystem::path& file);

// A data document's revision: the first 16 hex digits of its SHA-256, so an agent's edit on disk changes it too.
std::string data_rev(const std::string& body);

// A data document (data/<name>.json): its revision now, or nothing when it does not exist yet; and the write, a
// temporary file renamed over it. Both throw when data/ or the document is a symlink or not a plain file.
std::optional<std::string> data_revision(const std::filesystem::path& dir, const std::string& name);
void write_data(const std::filesystem::path& dir, const std::string& name, const std::string& body);

// index.html with the capability as <meta name="maic-artifact-token">, first in <head>.
std::string page_with_token(std::string html, const std::string& token);

// The trust recorded beside an artifact (`.maic-artifact.json`): "trusted" only when it says exactly that, and
// "sandboxed" for anything else, missing or unreadable. Nothing reads it to loosen the sandbox.
std::string artifact_trust(const std::filesystem::path& dir);

struct ArtifactInfo {
    std::string id;
    std::string trust;
    std::string added;
    std::vector<std::string> data;  // the data documents' names
};
std::vector<ArtifactInfo> list_artifacts(const std::filesystem::path& root);

// Copies a built page folder into root/id, over what is there: dotfiles and symlinks are skipped, and the
// artifact's own data/ documents are never overwritten (the folder's seed one only fills a missing one). Records
// the trust as sandboxed the first time. Returns what it skipped.
std::vector<std::string> add_artifact(const std::filesystem::path& root, const std::filesystem::path& src, const std::string& id);

// One-time browser logins: `maic artifact open` writes one under <state>/artifact-logins/ (only the code's SHA-256
// names the file), and the server claims it once, before it expires.
std::string new_artifact_login(const std::filesystem::path& state, int seconds = 120);
bool claim_artifact_login(const std::filesystem::path& state, const std::string& code);

// The server's secrets for artifacts, in memory only, 12 hours each: a browser login (the cookie) and per-artifact
// capabilities. A capability names one artifact and is never a login; a login is never a capability.
class ArtifactGrants {
public:
    std::string login();
    bool logged_in(const std::string& secret);
    std::string grant(const std::string& artifact);
    bool granted(const std::string& secret, const std::string& artifact);

private:
    struct Grant {
        std::string artifact;  // "" for a login
        std::chrono::steady_clock::time_point expires;
    };
    std::string mint(const std::string& artifact);
    bool valid(const std::string& secret, const std::string& artifact);
    std::mutex mu_;
    std::map<std::string, Grant> grants_;  // by the secret's SHA-256
};

}  // namespace maic::server
