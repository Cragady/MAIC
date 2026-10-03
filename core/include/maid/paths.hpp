#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace maid {

// MAID_HOME if set, else <prefix>/share/maid when it has services, else the source tree this binary was built from.
std::filesystem::path root_dir();

// The places root_dir() looks, for a message when none of them has what was wanted.
std::string root_dir_looked();

// $HOME; throws "HOME is not set" when it is unset or empty.
std::filesystem::path home_dir();

// $XDG_STATE_HOME/maid, falling back to ~/.local/state/maid. Holds PID files and logs.
std::filesystem::path state_dir();

// Replaces ${NAME} with the environment variable NAME. Unset variables are an error.
std::string expand_vars(std::string_view text);

// What an aggregate loader skipped (one bad item of many): into `problems` when given, else to stderr once per process.
void report_skipped(std::vector<std::string>* problems, std::string problem);

}  // namespace maid
