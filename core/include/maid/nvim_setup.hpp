#pragma once

#include <filesystem>
#include <string>

namespace maid {

// `maid nvim setup llama-vim`: llama.vim's spec from docs/models.md as one file MAID owns, maid-llama-vim.lua in
// the directory the user's lazy.nvim spec imports. Planning only looks; nothing is written until apply. A user
// command: the harness refuses it as a tool call. docs/nvim.md.

struct LlamaVimPlan {
    // Noop: nothing to do and nothing wrong (no nvim, no lazy.nvim, no import directory, nothing to remove):
    // `text` says why. Refuse: a file or a spec of the user's is in the way. Error: nvim could not answer.
    enum class Kind { Noop, UpToDate, Write, Remove, Refuse, Error };
    Kind kind = Kind::Noop;
    std::string text;            // what to print before any question: the explanation, or the plan
    std::filesystem::path file;  // Write, Remove, UpToDate: <import dir>/maid-llama-vim.lua
    std::string content;         // Write: what the file will hold
    bool update = false;         // Write: the file is MAID's already
};

// True when `program` (a name, or a path) is an executable on PATH.
bool on_path(const std::string& program);

// The spec as docs/models.md shows it, for pasting by hand.
std::string llama_vim_spec();
// What MAID writes: a header naming MAID, `date`, the undo command and the docs, then `return` and the spec.
std::string llama_vim_file(const std::string& date);
// Looks at nvim and the user's lazy.nvim setup (for `remove`, only at the config directory). `config` non-empty:
// `nvim -u config` instead of the user's init.
LlamaVimPlan plan_llama_vim(bool remove, const std::string& config = "", const std::string& nvim = "nvim");
// Writes or removes the plan's file; throws when it cannot.
void apply_llama_vim(const LlamaVimPlan& plan);

}  // namespace maid
