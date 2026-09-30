#include "maic/tools.hpp"

#include "maic/sandbox.hpp"

#include <algorithm>
#include <regex.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxReadLines = 2000;
constexpr size_t kMaxReadBytes = 50 * 1024;  // one file with long lines must not eat the context
constexpr size_t kMaxLineChars = 2000;
constexpr size_t kMaxListEntries = 500;
constexpr size_t kMaxSearchHits = 200;
constexpr size_t kMaxSearchLine = 64 * 1024;  // longer lines are minified/generated; skip them
constexpr int kDefaultShellTimeout = 120;
constexpr int kMaxShellTimeout = 600;

const char* const kToolNames[] = {"read_file", "list_dir", "search_files", "write_file", "edit_file", "run_shell"};

nlohmann::json fn(const char* name, const char* description, nlohmann::json properties, std::vector<std::string> required) {
    return {{"type", "function"},
            {"function", {{"name", name},
                          {"description", description},
                          {"parameters", {{"type", "object"}, {"properties", properties}, {"required", required}}}}}};
}

std::string arg(const nlohmann::json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw std::runtime_error(std::string("missing string argument '") + key + "'");
    }
    return args[key].get<std::string>();
}

std::string read_whole(const fs::path& p) {
    if (fs::is_directory(p)) throw std::runtime_error(p.string() + " is a directory, not a file");
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + p.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_whole(const fs::path& p, const std::string& content) {
    if (fs::is_directory(p)) throw std::runtime_error(p.string() + " is a directory, not a file");
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) throw std::runtime_error("can't write " + p.string());
}

size_t count_lines(const std::string& s) {
    if (s.empty()) return 0;
    return std::count(s.begin(), s.end(), '\n') + (s.back() == '\n' ? 0 : 1);
}

bool skip_dir(const fs::path& p) {
    static const std::vector<std::string> skip = {".git", "build", "node_modules", ".venv", "__pycache__", "vcpkg_installed"};
    return std::find(skip.begin(), skip.end(), p.filename().string()) != skip.end();
}

// A NUL byte, or more than 30% non-printable bytes in the first 4 KB, means binary.
bool looks_binary(const std::string& sample) {
    if (sample.empty()) return false;
    size_t odd = 0;
    for (unsigned char c : sample) {
        if (c == 0) return true;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ++odd;
    }
    return odd * 10 > sample.size() * 3;
}

// Up to three entries of the parent directory whose names share the requested name, for a missing file.
std::string did_you_mean(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p.parent_path(), ec)) return "";
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string want = lower(p.filename().string()), want_stem = lower(p.stem().string());
    std::vector<std::string> hits;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
        std::string name = e.path().filename().string();
        std::string l = lower(name), stem = lower(e.path().stem().string());
        if (l.find(want) != std::string::npos || want.find(l) != std::string::npos || (!want_stem.empty() && (stem == want_stem || stem.find(want_stem) != std::string::npos || want_stem.find(stem) != std::string::npos))) {
            hits.push_back(name + (e.is_directory(ec) ? "/" : ""));
            if (hits.size() == 3) break;
        }
    }
    if (hits.empty()) return "";
    std::string out = " Did you mean one of these?";
    for (const auto& h : hits) out += "\n  " + (p.parent_path() / h).string();
    return out;
}

ToolResult read_file(const fs::path& p, const nlohmann::json& args) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return {false, p.string() + " is a directory; use list_dir for it"};
    std::ifstream in(p, std::ios::binary);
    if (!in) return {false, "no such file: " + p.string() + did_you_mean(p)};
    {
        char sample[4096];
        in.read(sample, sizeof(sample));
        if (looks_binary(std::string(sample, static_cast<size_t>(in.gcount())))) {
            return {false, "cannot read a binary file: " + p.string()};
        }
        in.clear();
        in.seekg(0);
    }
    size_t offset = args.value("offset", 1);
    size_t limit = std::min<size_t>(args.value("limit", kMaxReadLines), kMaxReadLines);
    std::ostringstream out;
    std::string line;
    size_t n = 0, shown = 0, first = 0, last = 0, bytes = 0;
    bool capped = false;
    while (std::getline(in, line)) {
        ++n;
        if (n < offset) continue;
        if (line.size() > kMaxLineChars) line = line.substr(0, kMaxLineChars) + " ...";
        if (shown == limit || (shown > 0 && bytes + line.size() > kMaxReadBytes)) {
            capped = true;
            // Keep counting so the footer can say how long the file is.
            while (std::getline(in, line)) ++n;
            break;
        }
        if (!shown) first = n;
        last = n;
        out << n << "\t" << line << "\n";
        bytes += line.size() + 1;
        ++shown;
    }
    if (!shown) {
        if (n == 0) return {true, "(empty file)"};
        return {true, "(offset " + std::to_string(offset) + " is past the end: the file has " + std::to_string(n) + " lines)"};
    }
    if (capped) {
        out << "(Showing lines " << first << "-" << last << " of " << n << ". Use offset=" << last + 1 << " to continue.)\n";
    } else {
        out << "(End of file - total " << n << " lines)\n";
    }
    return {true, out.str()};
}

ToolResult list_dir(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return {false, "no such directory: " + p.string() + did_you_mean(p)};
    if (!fs::is_directory(p, ec)) return {false, p.string() + " is a file, not a directory; use read_file for it"};
    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(p, ec)) {
        entries.push_back(e.path().filename().string() + (e.is_directory(ec) ? "/" : ""));
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream out;
    for (size_t i = 0; i < entries.size() && i < kMaxListEntries; ++i) out << entries[i] << "\n";
    if (entries.size() > kMaxListEntries) out << "... " << entries.size() - kMaxListEntries << " more\n";
    out << "(" << entries.size() << (entries.size() == 1 ? " entry)" : " entries)");
    return {true, out.str()};
}

// POSIX extended regex (grep -E syntax). std::regex recurses per character and overflows the stack on long
// lines, which crashed MAIC; glibc's matcher doesn't.
ToolResult search_files(const Harness& harness, const fs::path& root, const std::string& pattern) {
    regex_t re;
    if (int rc = regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB); rc != 0) {
        char msg[256];
        regerror(rc, &re, msg, sizeof(msg));
        return {false, std::string("bad regex: ") + msg};
    }
    struct Free {
        regex_t* r;
        ~Free() { regfree(r); }
    } free_re{&re};
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {false, "no such directory: " + root.string() + did_you_mean(root)};
    std::ostringstream out;
    size_t hits = 0;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
    for (; it != fs::recursive_directory_iterator() && hits < kMaxSearchHits; ++it) {
        if (it->is_directory() && (skip_dir(it->path()) || harness.is_secret(it->path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || it->file_size() > 2 * 1024 * 1024) continue;
        std::ifstream in(it->path());
        std::string line;
        for (size_t n = 1; std::getline(in, line) && hits < kMaxSearchHits; ++n) {
            if (line.find('\0') != std::string::npos) break;  // binary file
            if (line.size() > kMaxSearchLine) continue;
            if (regexec(&re, line.c_str(), 0, nullptr, 0) == 0) {
                out << fs::relative(it->path(), harness.workspace()).string() << ":" << n << ": " << line.substr(0, 300) << "\n";
                ++hits;
            }
        }
    }
    if (hits == kMaxSearchHits) out << "... stopped at " << kMaxSearchHits << " matches; narrow the pattern or the path\n";
    return {true, hits ? out.str() : "no matches for /" + pattern + "/ under " + root.string()};
}

// Lines compared with leading and trailing whitespace trimmed: the one cheap fallback when an exact match misses
// (a model that re-typed the indentation). Returns the byte range in `text`, or npos.
std::pair<size_t, size_t> find_trimmed(const std::string& text, const std::string& needle) {
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::vector<std::string> want;
    {
        std::istringstream in(needle);
        for (std::string l; std::getline(in, l);) want.push_back(trim(l));
    }
    if (want.empty()) return {std::string::npos, 0};
    std::vector<std::pair<size_t, size_t>> lines;  // byte ranges of each line, without the newline
    for (size_t start = 0; start <= text.size();) {
        size_t nl = text.find('\n', start);
        lines.push_back({start, nl == std::string::npos ? text.size() : nl});
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    for (size_t i = 0; i + want.size() <= lines.size(); ++i) {
        bool all = true;
        for (size_t j = 0; j < want.size() && all; ++j) {
            all = trim(text.substr(lines[i + j].first, lines[i + j].second - lines[i + j].first)) == want[j];
        }
        if (all) return {lines[i].first, lines[i + want.size() - 1].second};
    }
    return {std::string::npos, 0};
}

ToolResult edit_file(const fs::path& p, const std::string& old_in, const std::string& new_in, bool replace_all) {
    if (old_in.empty()) return {false, "old_string is empty. To create or replace a whole file use write_file."};
    if (old_in == new_in) return {false, "No changes to apply: old_string and new_string are identical."};
    std::string text = read_whole(p);
    // Files with CRLF endings are searched and edited as LF and written back as CRLF.
    bool crlf = text.find("\r\n") != std::string::npos;
    auto to_lf = [](std::string s) {
        for (size_t i; (i = s.find("\r\n")) != std::string::npos;) s.erase(i, 1);
        return s;
    };
    std::string body = crlf ? to_lf(text) : text;
    std::string old_s = to_lf(old_in), new_s = to_lf(new_in);

    size_t first = body.find(old_s);
    size_t len = old_s.size();
    if (first == std::string::npos) {
        auto [from, to] = find_trimmed(body, old_s);
        if (from == std::string::npos) {
            return {false, "old_string not found in " + p.string() +
                               ". It must match the file exactly, including whitespace, indentation and line endings; "
                               "copy it from a fresh read_file result without the line-number prefix."};
        }
        first = from;
        len = to - from;
        // The model typed the block with the wrong indentation: shift new_string by the same amount so the
        // replacement lands with the file's indentation.
        auto indent_of = [](const std::string& s) { return static_cast<long>(s.find_first_not_of(" \t") == std::string::npos ? s.size() : s.find_first_not_of(" \t")); };
        std::string file_first = body.substr(from, body.find('\n', from) == std::string::npos ? std::string::npos : body.find('\n', from) - from);
        long delta = indent_of(file_first) - indent_of(old_s.substr(0, old_s.find('\n')));
        if (delta != 0) {
            std::string shifted;
            std::istringstream in(new_s);
            bool first_line = true;
            for (std::string l; std::getline(in, l); first_line = false) {
                if (delta > 0) l = std::string(static_cast<size_t>(delta), ' ') + l;
                else l.erase(0, std::min<size_t>(static_cast<size_t>(-delta), l.find_first_not_of(" \t") == std::string::npos ? l.size() : l.find_first_not_of(" \t")));
                shifted += (first_line ? "" : "\n") + l;
            }
            if (!new_s.empty() && new_s.back() == '\n') shifted += '\n';
            new_s = shifted;
        }
    }
    size_t occurrences = 1;
    if (replace_all) {
        occurrences = 0;
        for (size_t pos = body.find(old_s); pos != std::string::npos; pos = body.find(old_s, pos + old_s.size())) ++occurrences;
        std::string out;
        size_t pos = 0;
        for (size_t hit; (hit = body.find(old_s, pos)) != std::string::npos; pos = hit + old_s.size()) {
            out.append(body, pos, hit - pos);
            out += new_s;
        }
        out.append(body, pos, std::string::npos);
        body = out;
    } else {
        if (body.find(old_s, first + 1) != std::string::npos && len == old_s.size()) {
            return {false, "old_string appears more than once in " + p.string() + "; include more surrounding text, or set replace_all to true"};
        }
        body.replace(first, len, new_s);
    }
    if (crlf) {
        std::string out;
        for (char c : body) {
            if (c == '\n') out += '\r';
            out += c;
        }
        body = out;
    }
    write_whole(p, body);
    size_t added = count_lines(new_s) * occurrences, removed = count_lines(old_s) * occurrences;
    std::string what = replace_all ? " (" + std::to_string(occurrences) + " occurrences, " : " (";
    return {true, "edited " + p.string() + what + "+" + std::to_string(added) + " -" + std::to_string(removed) + ")"};
}

ToolResult run_shell(const Harness& harness, const nlohmann::json& args, bool read_only, const std::atomic<bool>& cancel) {
    int secs = std::clamp(args.value("timeout_seconds", kDefaultShellTimeout), 1, kMaxShellTimeout);
    auto r = run_sandboxed(arg(args, "command"), harness.workspace(), read_only, std::chrono::seconds(secs), cancel);
    std::string status;
    if (r.timed_out) {
        status = "terminated: the command exceeded its timeout of " + std::to_string(secs) + " s. If it is expected to take longer and is not "
                 "waiting for input, retry with a larger timeout_seconds (up to " + std::to_string(kMaxShellTimeout) + ").";
    } else if (r.cancelled) {
        status = "the user cancelled the command";
    } else {
        status = "exit code " + std::to_string(r.exit_code);
    }
    std::string output = r.output;
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    if (output.empty()) output = "(no output)";
    return {r.exit_code == 0 && !r.timed_out && !r.cancelled, status + "\n" + output};
}

}  // namespace

std::string canonical_tool_name(const std::string& name) {
    // Small models emit Read_File, readFile or read-file; map them back.
    std::string snake;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]))) snake += '_';
        snake += static_cast<char>(c == '-' || c == ' ' ? '_' : std::tolower(c));
    }
    for (const char* t : kToolNames) {
        if (snake == t) return t;
    }
    return "";
}

std::string tool_names() {
    std::string out;
    for (const char* t : kToolNames) out += (out.empty() ? "" : ", ") + std::string(t);
    return out;
}

const nlohmann::json& tool_schemas() {
    static const nlohmann::json schemas = nlohmann::json::array({
        fn("read_file",
           "Read a text file. Returns numbered lines as `N<tab>content`; the number and the tab are not part of the "
           "file. Ends with a line saying whether the file continues (and the offset to use) or that this was the end. "
           "Read a large window rather than many small ones. Call it once per file; call several in parallel when you "
           "need several files. Refuses binary files.",
           {{"path", {{"type", "string"}, {"description", "File path, relative to the workspace or absolute"}}},
            {"offset", {{"type", "integer"}, {"description", "First line to read (1-based)"}}},
            {"limit", {{"type", "integer"}, {"description", "Max lines to read (default and max 2000)"}}}},
           {"path"}),
        fn("list_dir", "List a directory. Directories end with /. Ends with the entry count. Use this instead of `ls` in run_shell.",
           {{"path", {{"type", "string"}, {"description", "Directory path; '.' for the workspace"}}}}, {"path"}),
        fn("search_files",
           "Search file contents with an extended regular expression (grep -E syntax). Results are `path:line: text`. "
           "Skips .git, build and similar. Use this instead of grep in run_shell.",
           {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Directory to search; default '.'"}}}},
           {"pattern"}),
        fn("write_file", "Create or overwrite a whole file with the given content. For a change inside an existing file use edit_file.",
           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}, {"path", "content"}),
        fn("edit_file",
           "Replace one exact occurrence of old_string with new_string in a file. old_string must match the file "
           "exactly: copy it from a read_file result, taking everything after the tab and never the line number or "
           "the tab. Include enough surrounding lines to make it unique, or set replace_all for a rename. If the "
           "result says it was not found, read the file again and copy the exact text.",
           {{"path", {{"type", "string"}}},
            {"old_string", {{"type", "string"}}},
            {"new_string", {{"type", "string"}}},
            {"replace_all", {{"type", "boolean"}, {"description", "Replace every occurrence (default false)"}}}},
           {"path", "old_string", "new_string"}),
        fn("run_shell",
           "Run a bash command in a sandbox: only the workspace is writable, no network, no sudo, a timeout (default "
           "120 s, max 600). For builds, tests, git and package tools. NOT for reading, listing, searching or editing "
           "files: use read_file, list_dir, search_files, edit_file and write_file for those. Do not use echo or "
           "printf to talk to the user; write your answer as text.",
           {{"command", {{"type", "string"}}}, {"timeout_seconds", {{"type", "integer"}, {"description", "Default 120, max 600"}}}},
           {"command"}),
    });
    return schemas;
}

Action tool_action(const Harness& harness, const std::string& name, const nlohmann::json& args) {
    using K = Action::Kind;
    if (name == "read_file") return {K::Read, harness.resolve(arg(args, "path")), ""};
    if (name == "list_dir") return {K::Read, harness.resolve(arg(args, "path")), ""};
    if (name == "search_files") return {K::Read, harness.resolve(args.value("path", ".")), ""};
    if (name == "write_file" || name == "edit_file") return {K::Write, harness.resolve(arg(args, "path")), ""};
    if (name == "run_shell") return {K::Shell, {}, arg(args, "command")};
    throw std::runtime_error("unknown tool: " + name + " (the tools are " + tool_names() + ")");
}

std::string tool_summary(const std::string& name, const nlohmann::json& args) {
    if (name == "run_shell") return "$ " + args.value("command", "");
    if (name == "search_files") return "search /" + args.value("pattern", "") + "/ in " + args.value("path", ".");
    return name + " " + args.value("path", "");
}

ToolResult run_tool(const Harness& harness, const std::string& name, const nlohmann::json& args,
                    bool read_only_sandbox, const std::atomic<bool>& cancel) {
    try {
        if (name == "read_file") return read_file(harness.resolve(arg(args, "path")), args);
        if (name == "list_dir") return list_dir(harness.resolve(arg(args, "path")));
        if (name == "search_files") return search_files(harness, harness.resolve(args.value("path", ".")), arg(args, "pattern"));
        if (name == "write_file") {
            fs::path p = harness.resolve(arg(args, "path"));
            std::string content = arg(args, "content");
            size_t before = 0;
            bool existed = fs::is_regular_file(p);
            if (existed) before = count_lines(read_whole(p));
            write_whole(p, content);
            size_t after = count_lines(content);
            return {true, (existed ? "overwrote " : "wrote ") + p.string() + " (+" + std::to_string(after) + " -" + std::to_string(before) + ")"};
        }
        if (name == "edit_file") {
            bool all = args.contains("replace_all") && args["replace_all"].is_boolean() && args["replace_all"].get<bool>();
            return edit_file(harness.resolve(arg(args, "path")), arg(args, "old_string"), arg(args, "new_string"), all);
        }
        if (name == "run_shell") return run_shell(harness, args, read_only_sandbox, cancel);
        return {false, "unknown tool: " + name + " (the tools are " + tool_names() + ")"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

}  // namespace maic
