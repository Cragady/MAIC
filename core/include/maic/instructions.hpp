#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace maic {

struct InstructionFile {
    std::filesystem::path path;
    std::string text;
};

// Standing instructions for the agent, like CLAUDE.md:
//   1. the global file: $XDG_CONFIG_HOME/maic/MAIC.md (default ~/.config/maic/MAIC.md)
//   2. each `names` file found on the config chain (the project root, or the top of $HOME, down to the
//      workspace), outermost first, in trusted directories only (maic/trust.hpp)
// Later files are more specific. Each file is capped at 32 KB.
std::vector<InstructionFile> load_instructions(const std::filesystem::path& workspace,
                                               const std::vector<std::string>& names = {"MAIC.md", "AGENTS.md"});

std::filesystem::path global_instructions_path();

}  // namespace maic
