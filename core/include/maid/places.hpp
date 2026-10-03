#pragma once

#include "maid/service.hpp"
#include "maid/settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maid {

// Every place MAID knows, by a short name: `maid path NAME`, `:path NAME`, `mcd NAME` in the shell.
struct Place {
    std::string name;         // "workspace", "sessions", "comfyui/workflows", "vendor/llamacpp", ...
    std::filesystem::path path;
    std::string description;
    bool is_file = false;
};

// The registry for a workspace: MAID's own directories and files, the models directory and each vendored
// service's link, then every `maid artifacts` entry under its owner/name. `session` is the current transcript
// when one is given. Names are unique; order is stable for listing.
std::vector<Place> known_places(const Settings& settings, const std::filesystem::path& workspace, const std::vector<ServiceDef>& services,
                                const std::optional<std::filesystem::path>& session = std::nullopt);

// Exact name first, then a unique prefix (also matching after the "/" of owner/name). Throws with the
// candidates when the prefix is ambiguous, or with the list when nothing matches.
const Place& find_place(const std::vector<Place>& places, const std::string& query);

// Where `:cd ARG` goes: a directory (absolute, `~` or `~/...`, or relative to `workspace`), else a place by name
// whose path is a directory; "-" is `previous`. Throws with the reason when it is none of these.
std::filesystem::path cd_target(const std::string& arg, const std::filesystem::path& workspace, const std::filesystem::path& previous,
                                const std::vector<Place>& places);

// Shell functions for `eval "$(maid shell-init)"`: mcd NAME (cd there), mpath NAME (print), mcp NAME (copy),
// with completion of the names for zsh and bash. `shell` is "zsh", "bash" or "fish".
std::string shell_init(const std::string& shell);

// The command that opens `url` in the chosen browser ("default" = xdg-open, "firefox", "chrome"; chrome tries
// google-chrome, chromium and chrome). Detached, output discarded.
std::string browser_command(const std::string& browser, const std::string& url);

}  // namespace maid
