#pragma once

#include "maid/service.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maid {

// Something MAID or one of its services leaves on disk: transcripts, logs, generated outputs.
struct Artifact {
    std::string owner;  // "maid" or a service name
    std::string name;
    std::string description;
    std::filesystem::path path;  // a directory or a single file
    std::string resolved;        // where it really is, when `path` goes through a symlink; else ""
};

struct ArtifactUsage {
    uintmax_t bytes = 0;
    size_t files = 0;
};

// MAID's own artifacts plus every service's declared ones.
std::vector<Artifact> list_artifacts(const std::vector<ServiceDef>& services);

ArtifactUsage measure(const Artifact& artifact, std::optional<std::chrono::hours> older_than = std::nullopt);

// Deletes the files in an artifact (the artifact's own directory stays), optionally only older ones.
// Never follows symlinks and refuses paths outside $HOME. Returns what was removed.
ArtifactUsage clean(const Artifact& artifact, std::optional<std::chrono::hours> older_than = std::nullopt);

}  // namespace maid
