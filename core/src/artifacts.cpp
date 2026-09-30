#include "maic/artifacts.hpp"

#include "maic/paths.hpp"
#include "maic/session.hpp"

#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace maic {

namespace fs = std::filesystem;

namespace {

bool is_old(const fs::directory_entry& e, std::optional<std::chrono::hours> older_than) {
    if (!older_than) return true;
    std::error_code ec;
    auto mtime = e.symlink_status(ec).type() == fs::file_type::symlink ? fs::file_time_type::clock::now()
                                                                        : e.last_write_time(ec);
    return !ec && fs::file_time_type::clock::now() - mtime > *older_than;
}

// Visits every file (and symlink, as itself) under an artifact without following links.
void each_file(const fs::path& root, const std::function<void(const fs::directory_entry&)>& fn) {
    std::error_code ec;
    auto st = fs::symlink_status(root, ec);
    if (ec || !fs::exists(st)) return;
    if (!fs::is_directory(st)) {
        fn(fs::directory_entry(root));
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto type = it->symlink_status(ec).type();
        if (type == fs::file_type::regular || type == fs::file_type::symlink) fn(*it);
    }
}

void require_safe(const fs::path& path) {
    fs::path home = fs::weakly_canonical(std::getenv("HOME"));
    fs::path p = fs::weakly_canonical(path);
    auto rel = p.lexically_relative(home);
    if (p == home || rel.empty() || *rel.begin() == ".." || p.is_relative()) {
        throw std::runtime_error("refusing to clean " + path.string() + ": only paths inside your home directory");
    }
}

}  // namespace

std::vector<Artifact> list_artifacts(const std::vector<ServiceDef>& services) {
    std::vector<Artifact> out = {
        {"maic", "sessions", "agent session transcripts (JSONL)", sessions_dir()},
        {"maic", "service-logs", "stdout/stderr of services MAIC started", state_dir() / "logs"},
    };
    for (const auto& s : services) {
        for (const auto& a : s.artifacts) {
            out.push_back({s.name, a.name, a.description, a.path});
        }
    }
    return out;
}

ArtifactUsage measure(const Artifact& artifact, std::optional<std::chrono::hours> older_than) {
    ArtifactUsage u;
    each_file(artifact.path, [&](const fs::directory_entry& e) {
        if (!is_old(e, older_than)) return;
        std::error_code ec;
        if (e.symlink_status(ec).type() == fs::file_type::regular) u.bytes += e.file_size(ec);
        ++u.files;
    });
    return u;
}

ArtifactUsage clean(const Artifact& artifact, std::optional<std::chrono::hours> older_than) {
    require_safe(artifact.path);
    ArtifactUsage removed;
    std::vector<fs::path> victims;
    each_file(artifact.path, [&](const fs::directory_entry& e) {
        if (!is_old(e, older_than)) return;
        std::error_code ec;
        if (e.symlink_status(ec).type() == fs::file_type::regular) removed.bytes += e.file_size(ec);
        victims.push_back(e.path());
    });
    for (const auto& v : victims) {
        std::error_code ec;
        if (fs::remove(v, ec)) ++removed.files;
    }
    // Drop directories the cleaning emptied, deepest first; keep the artifact's own directory.
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(artifact.path, ec))) {
        std::vector<fs::path> dirs;
        for (auto it = fs::recursive_directory_iterator(artifact.path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->symlink_status(ec).type() == fs::file_type::directory) dirs.push_back(it->path());
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            if (fs::is_empty(*it, ec)) fs::remove(*it, ec);
        }
    }
    return removed;
}

}  // namespace maic
