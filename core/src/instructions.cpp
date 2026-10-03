#include "maid/instructions.hpp"

#include "maid/trust.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace maid {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxInstructionBytes = 32 * 1024;

fs::path resolved(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

bool under(const fs::path& p, const fs::path& dir) {
    auto rel = p.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";
}

std::string read_capped(const fs::path& p) {
    std::ifstream in(p);
    std::string text(kMaxInstructionBytes, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    if (in.peek() != EOF) text += "\n[truncated at 32 KB]";
    return text;
}

// The files `dir` contributes, in reading order: the classes present, lowest priority first (with `highest`,
// only the top one), then their local variants the same way.
std::vector<fs::path> files_in(const fs::path& dir, const InstructionOptions& o) {
    std::vector<fs::path> shared, local;
    std::error_code ec;
    for (const auto& name : instruction_names(o)) {
        fs::path f = dir / name;
        if (!fs::is_regular_file(f, ec)) continue;
        bool is_local = std::find(o.files.begin(), o.files.end(), name) == o.files.end();
        (is_local ? local : shared).push_back(f);
    }
    if (o.highest && shared.size() > 1) shared.erase(shared.begin(), shared.end() - 1);
    if (o.highest && local.size() > 1) local.erase(local.begin(), local.end() - 1);
    shared.insert(shared.end(), local.begin(), local.end());
    return shared;
}

// Reads files and their imports into `out`, each file once. An import is followed only when it lands under one
// of `roots` (the system and config directories and the trusted directories in play) or in a trusted directory.
struct Loader {
    const InstructionOptions& options;
    std::vector<fs::path> roots;
    std::set<fs::path>& seen;
    std::vector<InstructionFile> out;
    std::vector<PendingImport>* pending = nullptr;

    // Your own files: the system and config directories, the first two roots.
    bool own(const fs::path& file) const {
        return under(resolved(file), roots[0]) || under(resolved(file), roots[1]);
    }

    bool allowed(const fs::path& target) const {
        for (const auto& r : roots) {
            if (under(target, r)) return true;
        }
        for (fs::path d = target.parent_path(); !never_project(d); d = d.parent_path()) {
            if (trusted(d)) return true;
        }
        return false;
    }

    void add(const fs::path& file, int depth, const fs::path& by) {
        if (!seen.insert(resolved(file)).second) return;
        std::string text = read_capped(file);
        std::error_code ec;
        if (depth < options.import_depth) {
            for (const auto& target : import_targets(file, text)) {
                fs::path t = resolved(target);
                if (!fs::is_regular_file(t, ec)) {
                    text += "\n[MAID: @" + target.string() + " was not imported: there is no such file]";
                } else if (!allowed(t) && !own(file)) {
                    text += "\n[MAID: @" + target.string() + " was not imported: it is outside the trusted directories and your config directory]";
                } else if (Trust st = allowed(t) ? Trust::Trusted : import_exception_status(file, t).trust; st != Trust::Trusted) {
                    bool changed = st == Trust::Changed;
                    if (pending) pending->push_back({file, t, changed});
                    text += "\n[MAID: @" + target.string() + " was not imported: it is outside the trusted directories, and the user has not " +
                            (changed ? "approved it since it changed" : "approved it yet") + " (maid trust imports --approve)]";
                } else {
                    add(t, depth + 1, file);
                }
            }
        }
        out.push_back({file, text, by});
    }

    void add_dir(const fs::path& dir) {
        for (const auto& f : files_in(dir, options)) add(f, 0, {});
    }
};

std::vector<fs::path> trusted_chain(const fs::path& workspace) {
    std::vector<fs::path> out;
    for (const auto& d : config_chain(workspace)) {
        if (trusted(d)) out.push_back(d);  // an untrusted project's instructions never reach the model
    }
    return out;
}

std::vector<fs::path> base_roots(const std::vector<fs::path>& dirs) {
    std::vector<fs::path> roots = {resolved(system_instructions_dir()), resolved(user_instructions_dir())};
    for (const auto& d : dirs) roots.push_back(resolved(d));
    return roots;
}

}  // namespace

fs::path user_instructions_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "maid";
    return fs::path(std::getenv("HOME")) / ".config" / "maid";
}

fs::path global_instructions_path() {
    return user_instructions_dir() / "MAID.md";
}

fs::path system_instructions_dir() {
    const char* testing = std::getenv("MAID_TESTING");
    const char* dir = std::getenv("MAID_SYSTEM_CONFIG_DIR");
    if (testing && std::string(testing) == "1" && dir && *dir) return dir;
    return "/etc/maid";
}

std::vector<std::string> instruction_names(const InstructionOptions& options) {
    std::vector<std::string> out = options.files;
    if (!options.local_files) return out;
    for (const auto& name : options.files) {
        fs::path p(name);
        out.push_back(p.stem().string() + ".local" + p.extension().string());
    }
    return out;
}

std::vector<fs::path> import_targets(const fs::path& file, const std::string& text) {
    std::vector<fs::path> out;
    std::istringstream lines(text);
    std::string fence;  // the open fence's marker (``` or ~~~, as long as it was), empty outside one
    for (std::string line; std::getline(lines, line);) {
        size_t start = line.find_first_not_of(" \t");
        std::string body = start == std::string::npos ? "" : line.substr(start);
        if (body.rfind("```", 0) == 0 || body.rfind("~~~", 0) == 0) {
            size_t n = body.find_first_not_of(body[0]);
            std::string marker = body.substr(0, n == std::string::npos ? body.size() : n);
            if (fence.empty()) fence = marker;
            else if (marker[0] == fence[0] && marker.size() >= fence.size()) fence.clear();
            continue;
        }
        if (!fence.empty() || body.rfind('>', 0) == 0) continue;  // fenced code, a block quote
        size_t code = 0;     // the backtick run that opened the current code span, 0 outside one
        bool quoted = false;  // inside "..." or a curly-quoted span
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '`') {
                size_t n = line.find_first_not_of('`', i);
                n = (n == std::string::npos ? line.size() : n) - i;
                if (!code) code = n;
                else if (code == n) code = 0;
                i += n - 1;
                continue;
            }
            if (code) continue;
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (line.compare(i, 3, "\xE2\x80\x9C") == 0) quoted = true;   // “
            if (line.compare(i, 3, "\xE2\x80\x9D") == 0) quoted = false;  // ”
            if (quoted || c != '@' || (i > 0 && !std::isspace(static_cast<unsigned char>(line[i - 1])) && line[i - 1] != '(')) continue;
            size_t end = i + 1;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && !std::strchr(")]>,;\"'`", line[end])) ++end;
            std::string token = line.substr(i + 1, end - i - 1);
            while (!token.empty() && std::strchr(".:!?", token.back())) token.pop_back();
            i = end - 1;
            if (token.empty() || (token.find('/') == std::string::npos && token.find('.') == std::string::npos && token[0] != '~')) continue;
            fs::path p;
            if (token == "~" || token.rfind("~/", 0) == 0) p = fs::path(std::getenv("HOME")) / token.substr(token.size() > 1 ? 2 : 1);
            else if (token[0] == '/') p = token;
            else p = file.parent_path() / token;
            out.push_back(p.lexically_normal());
        }
    }
    return out;
}

std::vector<InstructionFile> load_instructions(const fs::path& workspace, const InstructionOptions& options, const std::vector<fs::path>& extra,
                                               std::vector<PendingImport>* pending) {
    std::vector<fs::path> chain = trusted_chain(workspace), extras;
    if (options.extra_dirs) {
        for (const auto& e : extra) {
            if (trusted(e)) extras.push_back(e);
        }
    }
    std::vector<fs::path> in_play = extras;
    in_play.insert(in_play.end(), chain.begin(), chain.end());
    std::set<fs::path> seen;
    Loader loader{options, base_roots(in_play), seen, {}, pending};
    loader.add_dir(system_instructions_dir());
    loader.add_dir(user_instructions_dir());
    for (const auto& d : in_play) loader.add_dir(d);
    return loader.out;
}

std::set<fs::path> nested_allowed(const fs::path& workspace) {
    std::set<fs::path> out;
    fs::path ws = resolved(workspace);
    for (const auto& d : trusted_chain(workspace)) {
        for (const auto& f : project_dir(d).nested) {
            if (under(resolved(f), ws)) out.insert(resolved(f));
        }
    }
    return out;
}

std::vector<InstructionFile> nested_instructions(const fs::path& workspace, const fs::path& file, const InstructionOptions& options,
                                                 const std::set<fs::path>& allowed, std::set<fs::path>& seen) {
    fs::path ws = resolved(workspace), dir = resolved(file).parent_path();
    if (!under(dir, ws) || dir == ws) return {};  // outside the workspace, or in it: nothing extra
    std::vector<fs::path> dirs;
    for (fs::path d = dir; d != ws && d != d.parent_path(); d = d.parent_path()) dirs.push_back(d);
    std::reverse(dirs.begin(), dirs.end());
    Loader loader{options, base_roots(trusted_chain(workspace)), seen, {}};
    for (const auto& d : dirs) {
        for (const auto& f : files_in(d, options)) {
            if (allowed.count(resolved(f))) loader.add(f, 0, {});
        }
    }
    return loader.out;
}

}  // namespace maid
