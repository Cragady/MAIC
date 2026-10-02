#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace maic {

// How MAIC finds instruction files (docs/instructions.md). Every field comes from the global settings'
// `instructions` table only; a project's copy is ignored with a warning.
struct InstructionOptions {
    std::vector<std::string> files = {"CLAUDE.md", "AGENTS.md", "MAIC.md"};  // the classes, lowest priority first
    bool highest = false;     // read = "highest": only the top class present in each directory; "all" reads each one
    bool local_files = true;  // MAIC.local.md and the like, each right after its directory's shared files
    int import_depth = 4;     // how many hops @path imports follow; 0 turns imports off
    bool extra_dirs = false;  // whether extra directories (--add-dir, when it lands) contribute their instructions
};

struct InstructionFile {
    std::filesystem::path path;
    std::string text;
    std::filesystem::path imported_by;  // empty unless an @path import brought it in
};

// Standing instructions for the agent, read in this order (later ones take precedence where they conflict):
//   1. the system directory (system_instructions_dir(), empty by default)
//   2. your config directory (user_instructions_dir())
//   3. `extra` directories, when `options.extra_dirs` (trusted ones only)
//   4. the config chain (the project root, or the top of $HOME, down to the workspace), outermost first, in
//      trusted directories only (maic/trust.hpp)
// In each directory the classes go lowest priority first, then the local files. An imported file comes right
// before the file that imports it. Each file is capped at 32 KB.
std::vector<InstructionFile> load_instructions(const std::filesystem::path& workspace, const InstructionOptions& options = {},
                                               const std::vector<std::filesystem::path>& extra = {});

// On-demand loading: the instruction files from `file`'s directory up to (not including) the workspace,
// outermost first, that a trusted directory on the chain hashes (`allowed`, from nested_allowed), skipping
// any path in `seen` and adding what it returns to it. Imports are followed as in load_instructions.
std::vector<InstructionFile> nested_instructions(const std::filesystem::path& workspace, const std::filesystem::path& file,
                                                 const InstructionOptions& options, const std::set<std::filesystem::path>& allowed,
                                                 std::set<std::filesystem::path>& seen);
// The instruction files below the workspace that its trusted chain directories hash.
std::set<std::filesystem::path> nested_allowed(const std::filesystem::path& workspace);

// The file names one directory may hold, in reading order: the classes, then (with local_files) their local
// variants (MAIC.md -> MAIC.local.md).
std::vector<std::string> instruction_names(const InstructionOptions& options);
// The @path imports in `text` (the content of `file`), resolved against the file's directory, `~` expanded, in
// order; never from code spans, fenced code blocks, block quotes or text in double quotes.
std::vector<std::filesystem::path> import_targets(const std::filesystem::path& file, const std::string& text);

// $XDG_CONFIG_HOME/maic (default ~/.config/maic) and its MAIC.md.
std::filesystem::path user_instructions_dir();
std::filesystem::path global_instructions_path();
// /etc/maic. MAIC_SYSTEM_CONFIG_DIR replaces it for tests, and only with MAIC_TESTING=1.
std::filesystem::path system_instructions_dir();

}  // namespace maic
