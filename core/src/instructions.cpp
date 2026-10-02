#include "maic/instructions.hpp"

#include "maic/trust.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxInstructionBytes = 32 * 1024;

void add_if_present(std::vector<InstructionFile>& out, const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return;
    for (const auto& f : out) {
        if (fs::equivalent(f.path, p, ec)) return;
    }
    std::ifstream in(p);
    std::string text(kMaxInstructionBytes, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    if (in.peek() != EOF) text += "\n[truncated at 32 KB]";
    out.push_back({p, text});
}

}  // namespace

fs::path global_instructions_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "maic" / "MAIC.md";
    }
    return fs::path(std::getenv("HOME")) / ".config" / "maic" / "MAIC.md";
}

std::vector<InstructionFile> load_instructions(const fs::path& workspace, const std::vector<std::string>& names) {
    std::vector<InstructionFile> out;
    add_if_present(out, global_instructions_path());

    // The config chain: the project root (or just below $HOME) down to the workspace. Outside $HOME, only the
    // workspace itself.
    for (const auto& d : config_chain(workspace)) {
        if (!trusted(d)) continue;  // an untrusted project's instructions never reach the model
        for (const auto& name : names) {
            add_if_present(out, d / name);
        }
    }
    return out;
}

}  // namespace maic
