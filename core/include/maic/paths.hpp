#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace maic {

// MAIC_HOME if set, otherwise the source tree this binary was built from.
std::filesystem::path root_dir();

// $XDG_STATE_HOME/maic, falling back to ~/.local/state/maic. Holds PID files and logs.
std::filesystem::path state_dir();

// Replaces ${NAME} with the environment variable NAME. Unset variables are an error.
std::string expand_vars(std::string_view text);

}  // namespace maic
