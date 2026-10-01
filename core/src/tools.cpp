#include "maic/tools.hpp"

#include "maic/sandbox.hpp"

#include <algorithm>
#include <regex.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxReadLines = 2000;
constexpr size_t kMaxReadBytes = 50 * 1024;  // one file with long lines must not eat the context
constexpr size_t kMaxLineChars = 2000;
constexpr size_t kMaxListEntries = 500;
constexpr int kMaxListDepth = 4;
constexpr size_t kMaxGlobHits = 500;
constexpr size_t kMaxSearchHits = 200;
constexpr size_t kMaxSearchLine = 64 * 1024;  // longer lines are minified/generated; skip them
constexpr int kDefaultShellTimeout = 120;
constexpr int kMaxShellTimeout = 600;

const char* const kToolNames[] = {"read_file", "list_dir", "glob", "search_files", "write_file", "edit_file", "multi_edit", "apply_patch",
                                  "move_file", "copy_file", "delete_file", "make_dir", "run_shell", "question", "todo", "delegate"};

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

bool flag(const nlohmann::json& args, const char* key) {
    return args.contains(key) && args[key].is_boolean() && args[key].get<bool>();
}

// POSIX extended regex (grep -E syntax). std::regex recurses per character and overflows the stack on long
// lines, which crashed MAIC; glibc's matcher doesn't. `error` is set instead of throwing so each tool can
// word the failure.
struct Regex {
    regex_t re{};
    std::string error;
    explicit Regex(const std::string& pattern) {
        if (int rc = regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB); rc != 0) {
            char msg[256];
            regerror(rc, &re, msg, sizeof(msg));
            error = msg;
        }
    }
    ~Regex() {
        if (error.empty()) regfree(&re);
    }
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;
    bool matches(const std::string& s) const { return regexec(&re, s.c_str(), 0, nullptr, 0) == 0; }
};

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

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

// Files with CRLF endings are searched and edited as LF and written back as CRLF.
std::string to_lf(std::string s) {
    for (size_t i; (i = s.find("\r\n")) != std::string::npos;) s.erase(i, 1);
    return s;
}

std::string to_crlf(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
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
    std::string pattern = args.contains("grep") && args["grep"].is_string() ? args["grep"].get<std::string>() : "";
    Regex re(pattern);
    if (!re.error.empty()) return {false, "bad grep regex: " + re.error};
    size_t offset = args.value("offset", 1);
    size_t limit = std::min<size_t>(args.value("limit", kMaxReadLines), kMaxReadLines);
    std::ostringstream out;
    std::string line;
    size_t n = 0, shown = 0, first = 0, last = 0, bytes = 0;
    bool capped = false;
    while (std::getline(in, line)) {
        ++n;
        if (n < offset) continue;
        if (!pattern.empty() && !re.matches(line)) continue;
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
        if (!pattern.empty()) return {true, "(no lines match /" + pattern + "/" + (offset > 1 ? " from line " + std::to_string(offset) : "") + "; the file has " + std::to_string(n) + " lines)"};
        return {true, "(offset " + std::to_string(offset) + " is past the end: the file has " + std::to_string(n) + " lines)"};
    }
    if (!pattern.empty()) {
        if (capped) out << "(Showing " << shown << " matching lines up to line " << last << " of " << n << ". Use offset=" << last + 1 << " to continue.)\n";
        else out << "(" << shown << " matching line" << (shown == 1 ? "" : "s") << "; the file has " << n << " lines)\n";
    } else if (capped) {
        out << "(Showing lines " << first << "-" << last << " of " << n << ". Use offset=" << last + 1 << " to continue.)\n";
    } else {
        out << "(End of file - total " << n << " lines)\n";
    }
    return {true, out.str()};
}

// One level of the tree: entries sorted, directories with their entry count, descended to `max_depth`
// except the directories glob skips and secret ones. Stops adding lines at the cap but keeps counting.
void list_tree(const Harness& harness, const fs::path& dir, int depth, int max_depth, std::vector<std::string>& lines, size_t& total) {
    std::error_code ec;
    std::vector<fs::directory_entry> entries(fs::directory_iterator(dir, ec), fs::directory_iterator());
    std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path().filename() < b.path().filename(); });
    for (const auto& e : entries) {
        ++total;
        if (lines.size() >= kMaxListEntries) continue;
        std::string line(static_cast<size_t>(2 * (depth - 1)), ' ');
        line += e.path().filename().string();
        bool dir_entry = e.is_directory(ec);
        bool secret = dir_entry && harness.is_secret(e.path());
        if (dir_entry) {
            line += "/";
            if (!secret) {
                size_t n = std::distance(fs::directory_iterator(e.path(), ec), fs::directory_iterator());
                line += " (" + std::to_string(n) + (n == 1 ? " entry)" : " entries)");
            }
        }
        lines.push_back(line);
        if (dir_entry && !secret && depth < max_depth && !skip_dir(e.path()) && !e.is_symlink(ec)) list_tree(harness, e.path(), depth + 1, max_depth, lines, total);
    }
}

ToolResult list_dir(const Harness& harness, const fs::path& p, const nlohmann::json& args) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return {false, "no such directory: " + p.string() + did_you_mean(p)};
    if (!fs::is_directory(p, ec)) return {false, p.string() + " is a file, not a directory; use read_file for it"};
    int depth = std::clamp(args.value("depth", 1), 1, kMaxListDepth);
    std::vector<std::string> lines;
    size_t total = 0;
    list_tree(harness, p, 1, depth, lines, total);
    size_t top = std::distance(fs::directory_iterator(p, ec), fs::directory_iterator());
    std::ostringstream out;
    for (const auto& l : lines) out << l << "\n";
    if (total > lines.size()) out << "... " << total - lines.size() << " more; list a subdirectory, or a smaller depth\n";
    out << "(" << top << (top == 1 ? " entry" : " entries");
    if (depth > 1) out << ", " << total << " to depth " << depth;
    out << ")";
    return {true, out.str()};
}

// A glob over relative paths as an anchored POSIX regex: `*` and `?` stay inside one path segment, `**`
// spans segments, `[...]` classes pass through. A pattern with no `/` matches at any depth.
std::string glob_to_regex(const std::string& pattern) {
    std::string glob = pattern;
    while (glob.rfind("./", 0) == 0) glob.erase(0, 2);
    if (glob.find('/') == std::string::npos) glob = "**/" + glob;
    std::string re = "^";
    for (size_t i = 0; i < glob.size();) {
        char c = glob[i];
        if (c == '*' && i + 1 < glob.size() && glob[i + 1] == '*') {
            if (i + 2 < glob.size() && glob[i + 2] == '/') {
                re += "([^/]*/)*";
                i += 3;
            } else {
                re += ".*";
                i += 2;
            }
            continue;
        }
        if (c == '*') re += "[^/]*";
        else if (c == '?') re += "[^/]";
        else if (c == '[') {
            size_t close = glob.find(']', i + 1);
            if (close == std::string::npos) re += "\\[";
            else {
                re += glob.substr(i, close - i + 1);
                i = close;
            }
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-') re += c;
        else re += std::string("\\") + c;
        ++i;
    }
    return re + "$";
}

ToolResult glob_files(const Harness& harness, const fs::path& root, const std::string& pattern) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {false, "no such directory: " + root.string() + did_you_mean(root)};
    if (pattern.empty()) return {false, "pattern is empty"};
    Regex re(glob_to_regex(pattern));
    if (!re.error.empty()) return {false, "bad pattern: " + re.error};
    std::vector<std::string> hits;
    bool capped = false;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && (skip_dir(it->path()) || harness.is_secret(it->path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || harness.is_secret(it->path())) continue;
        std::string rel = it->path().lexically_relative(root).generic_string();
        if (!re.matches(rel)) continue;
        if (hits.size() == kMaxGlobHits) {
            capped = true;
            break;
        }
        hits.push_back(rel);
    }
    if (hits.empty()) return {true, "no files match " + pattern + " under " + root.string()};
    std::sort(hits.begin(), hits.end());
    std::ostringstream out;
    for (const auto& h : hits) out << h << "\n";
    if (capped) out << "... stopped at " << kMaxGlobHits << " matches; narrow the pattern or the path\n";
    else out << "(" << hits.size() << (hits.size() == 1 ? " file)" : " files)");
    return {true, out.str()};
}

ToolResult search_files(const Harness& harness, const fs::path& root, const std::string& pattern) {
    Regex re(pattern);
    if (!re.error.empty()) return {false, "bad regex: " + re.error};
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
            if (re.matches(line)) {
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

struct Edit {
    size_t added = 0, removed = 0, occurrences = 0;
    std::string error;  // set: `body` is untouched
};

// One replacement in LF text. `where` names the file in the error.
Edit replace_in(std::string& body, const std::string& old_s, std::string new_s, bool replace_all, const std::string& where) {
    Edit e;
    if (old_s.empty()) {
        e.error = "old_string is empty. To create or replace a whole file use write_file.";
        return e;
    }
    if (old_s == new_s) {
        e.error = "No changes to apply: old_string and new_string are identical.";
        return e;
    }
    size_t first = body.find(old_s);
    size_t len = old_s.size();
    if (first == std::string::npos) {
        auto [from, to] = find_trimmed(body, old_s);
        if (from == std::string::npos) {
            e.error = "old_string not found in " + where +
                      ". It must match the file exactly, including whitespace, indentation and line endings; "
                      "copy it from a fresh read_file result without the line-number prefix.";
            return e;
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
    e.occurrences = 1;
    if (replace_all) {
        e.occurrences = 0;
        for (size_t pos = body.find(old_s); pos != std::string::npos; pos = body.find(old_s, pos + old_s.size())) ++e.occurrences;
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
            e.error = "old_string appears more than once in " + where + "; include more surrounding text, or set replace_all to true";
            return e;
        }
        body.replace(first, len, new_s);
    }
    e.added = count_lines(new_s) * e.occurrences;
    e.removed = count_lines(old_s) * e.occurrences;
    return e;
}

std::string delta(const Edit& e, bool replace_all) {
    return (replace_all ? "(" + std::to_string(e.occurrences) + " occurrences, " : "(") + "+" + std::to_string(e.added) + " -" + std::to_string(e.removed) + ")";
}

ToolResult edit_file(const fs::path& p, const std::string& old_in, const std::string& new_in, bool replace_all) {
    std::string text = read_whole(p);
    bool crlf = text.find("\r\n") != std::string::npos;
    std::string body = crlf ? to_lf(text) : text;
    Edit e = replace_in(body, to_lf(old_in), to_lf(new_in), replace_all, p.string());
    if (!e.error.empty()) return {false, e.error};
    write_whole(p, crlf ? to_crlf(body) : body);
    return {true, "edited " + p.string() + " " + delta(e, replace_all)};
}

// Every edit in order on one copy of the file; the file is written only when all of them applied.
ToolResult multi_edit(const fs::path& p, const nlohmann::json& edits) {
    if (!edits.is_array() || edits.empty()) return {false, "edits must be a non-empty array of {old_string, new_string, replace_all?}"};
    std::string text = read_whole(p);
    bool crlf = text.find("\r\n") != std::string::npos;
    std::string body = crlf ? to_lf(text) : text;
    std::string report;
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& ed = edits[i];
        std::string label = "edit " + std::to_string(i + 1) + " of " + std::to_string(edits.size());
        if (!ed.is_object() || !ed.contains("old_string") || !ed["old_string"].is_string() || !ed.contains("new_string") || !ed["new_string"].is_string()) {
            return {false, label + " needs old_string and new_string. Nothing was written."};
        }
        bool all = flag(ed, "replace_all");
        Edit e = replace_in(body, to_lf(ed["old_string"].get<std::string>()), to_lf(ed["new_string"].get<std::string>()), all, p.string());
        if (!e.error.empty()) return {false, label + ": " + e.error + " Nothing was written; fix that edit and send the whole list again."};
        report += (i ? ", " : "") + std::to_string(i + 1) + " " + delta(e, all);
    }
    write_whole(p, crlf ? to_crlf(body) : body);
    return {true, "edited " + p.string() + " with " + std::to_string(edits.size()) + " edits: " + report};
}

// A unified diff, as `diff -u` or `git diff` print it. Only the headers and hunk lines are read; `diff --git`,
// `index` and mode lines are skipped. Line numbers in hunk headers locate a hunk; the context must match exactly.
struct Hunk {
    size_t old_start = 0;
    size_t line = 0;  // where the @@ header sits in the patch, for error messages
    std::vector<std::pair<char, std::string>> lines;  // ' ', '-' or '+'
    bool no_newline_old = false, no_newline_new = false;  // "\ No newline at end of file" markers
};

struct FilePatch {
    std::string old_path, new_path;  // "" for /dev/null
    std::vector<Hunk> hunks;
    const std::string& path() const { return old_path.empty() ? new_path : old_path; }
};

std::string patch_path(std::string s) {
    if (auto t = s.find('\t'); t != std::string::npos) s.erase(t);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s == "/dev/null") return "";
    if (s.rfind("a/", 0) == 0 || s.rfind("b/", 0) == 0) s.erase(0, 2);
    return s;
}

std::vector<FilePatch> parse_patch(const std::string& patch) {
    std::vector<std::string> lines = lines_of(patch);
    for (auto& l : lines) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::vector<FilePatch> files;
    Hunk* hunk = nullptr;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        std::string at = "patch line " + std::to_string(i + 1) + ": ";
        if (l.rfind("--- ", 0) == 0) {
            if (i + 1 >= lines.size() || lines[i + 1].rfind("+++ ", 0) != 0) throw std::runtime_error(at + "a '--- old' line must be followed by a '+++ new' line");
            FilePatch f{patch_path(l.substr(4)), patch_path(lines[i + 1].substr(4))};
            if (f.old_path.empty() && f.new_path.empty()) throw std::runtime_error(at + "both sides are /dev/null");
            if (!f.old_path.empty() && !f.new_path.empty() && f.old_path != f.new_path) {
                throw std::runtime_error(at + "the patch renames " + f.old_path + " to " + f.new_path + "; apply_patch does not rename, use move_file and then patch the new path");
            }
            files.push_back(f);
            hunk = nullptr;
            ++i;
            continue;
        }
        if (l.rfind("@@ -", 0) == 0) {
            if (files.empty()) throw std::runtime_error(at + "a hunk before any '--- old' / '+++ new' header");
            Hunk h;
            h.line = i + 1;
            if (std::sscanf(l.c_str(), "@@ -%zu", &h.old_start) != 1) throw std::runtime_error(at + "bad hunk header: " + l);
            files.back().hunks.push_back(h);
            hunk = &files.back().hunks.back();
            continue;
        }
        if (!hunk) continue;  // diff --git, index, mode lines, prose between files
        char tag = l.empty() ? ' ' : l[0];
        if (tag == ' ' || tag == '-' || tag == '+') hunk->lines.push_back({tag, l.empty() ? "" : l.substr(1)});
        else if (tag == '\\' && !hunk->lines.empty()) {
            char last = hunk->lines.back().first;
            if (last != '+') hunk->no_newline_old = true;
            if (last != '-') hunk->no_newline_new = true;
        } else hunk = nullptr;
    }
    if (files.empty()) throw std::runtime_error("no '--- old' / '+++ new' file headers found: send a unified diff (the output of diff -u or git diff), not a description of the change");
    for (const auto& f : files) {
        if (f.hunks.empty() && !f.old_path.empty()) throw std::runtime_error("no hunks for " + f.path());
    }
    return files;
}

struct PatchedFile {
    fs::path path;
    std::string content;
    bool created = false, deleted = false;
    size_t hunks = 0, added = 0, removed = 0;
};

// The new content of one file, or an error naming the hunk and line. Hunks are placed at their stated line,
// or at the nearest exact match of their old lines when the numbers are off; context never matches loosely.
std::string patch_file(const fs::path& p, const FilePatch& f, PatchedFile& out) {
    std::error_code ec;
    out.path = p;
    out.created = f.old_path.empty();
    out.deleted = f.new_path.empty();
    out.hunks = f.hunks.size();
    std::string text;
    if (out.created) {
        if (fs::exists(p, ec)) return p.string() + " already exists, but the patch creates it (--- /dev/null). Patch the existing file instead.";
    } else {
        if (!fs::is_regular_file(p, ec)) return "no such file: " + p.string() + did_you_mean(p);
        text = read_whole(p);
    }
    bool crlf = text.find("\r\n") != std::string::npos;
    if (crlf) text = to_lf(text);
    bool ends_nl = !text.empty() && text.back() == '\n';
    std::vector<std::string> orig = lines_of(text);
    std::vector<std::string> result;
    size_t cursor = 0;
    for (size_t hi = 0; hi < f.hunks.size(); ++hi) {
        const Hunk& h = f.hunks[hi];
        std::string which = "hunk " + std::to_string(hi + 1) + " of " + p.string() + " (patch line " + std::to_string(h.line) + ")";
        std::vector<std::string> old_lines;
        for (const auto& [tag, s] : h.lines) {
            if (tag != '+') old_lines.push_back(s);
        }
        size_t want = std::min(h.old_start ? h.old_start - 1 : 0, orig.size());
        size_t at = std::string::npos;
        if (old_lines.empty()) {
            // Nothing to match: a pure insertion goes after the stated line.
            at = std::min(h.old_start, orig.size());
            if (at < cursor) return which + " is out of order: it inserts at line " + std::to_string(at + 1) + ", before the previous hunk ended. Nothing was applied.";
        } else {
            auto matches_at = [&](size_t pos) {
                if (pos < cursor || pos + old_lines.size() > orig.size()) return false;
                for (size_t j = 0; j < old_lines.size(); ++j) {
                    if (orig[pos + j] != old_lines[j]) return false;
                }
                return true;
            };
            for (size_t d = 0; at == std::string::npos && (want + d + old_lines.size() <= orig.size() || d <= want); ++d) {
                if (matches_at(want + d)) at = want + d;
                else if (d <= want && matches_at(want - d)) at = want - d;
            }
            if (at == std::string::npos) {
                std::string why;
                if (want >= orig.size()) why = "the file has only " + std::to_string(orig.size()) + " lines";
                else {
                    size_t j = 0;
                    while (j < old_lines.size() && want + j < orig.size() && orig[want + j] == old_lines[j]) ++j;
                    why = want + j < orig.size() ? "at line " + std::to_string(want + j + 1) + " the file has \"" + orig[want + j].substr(0, 120) + "\" but the patch expects \"" + old_lines[std::min(j, old_lines.size() - 1)].substr(0, 120) + "\""
                                                 : "the file ends at line " + std::to_string(orig.size()) + " but the patch expects \"" + old_lines[std::min(j, old_lines.size() - 1)].substr(0, 120) + "\"";
                }
                return which + " does not match: " + why + ". Nothing was applied. Read the file again and rebuild the hunk from what is there, or use edit_file.";
            }
        }
        result.insert(result.end(), orig.begin() + static_cast<long>(cursor), orig.begin() + static_cast<long>(at));
        for (const auto& [tag, s] : h.lines) {
            if (tag != '-') result.push_back(s);
            if (tag == '+') ++out.added;
            if (tag == '-') ++out.removed;
        }
        cursor = at + old_lines.size();
    }
    result.insert(result.end(), orig.begin() + static_cast<long>(cursor), orig.end());
    if (!f.hunks.empty()) {
        const Hunk& last = f.hunks.back();
        if (last.no_newline_new) ends_nl = false;
        else if (last.no_newline_old || out.created) ends_nl = true;
    }
    for (size_t i = 0; i < result.size(); ++i) out.content += (i ? "\n" : "") + result[i];
    if (ends_nl && !result.empty()) out.content += "\n";
    if (crlf) out.content = to_crlf(out.content);
    return "";
}

ToolResult apply_patch(const Harness& harness, const std::string& patch) {
    std::vector<FilePatch> files = parse_patch(patch);
    std::vector<PatchedFile> results(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        std::string err = patch_file(harness.resolve(files[i].path()), files[i], results[i]);
        if (!err.empty()) return {false, err};
    }
    std::string report;
    for (const auto& r : results) {
        if (r.deleted) fs::remove(r.path);
        else write_whole(r.path, r.content);
        report += (report.empty() ? "" : ", ") + r.path.string() + " (" + (r.deleted ? "deleted" : r.created ? "created, " : std::to_string(r.hunks) + (r.hunks == 1 ? " hunk, " : " hunks, "));
        if (!r.deleted) report += "+" + std::to_string(r.added) + " -" + std::to_string(r.removed);
        report += ")";
    }
    return {true, "applied the patch: " + report};
}

void count_tree(const fs::path& dir, size_t& files, size_t& dirs) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) ++dirs;
        else ++files;
    }
}

std::string exists_already(const fs::path& to) {
    std::error_code ec;
    return to.string() + " already exists; " + (fs::is_directory(to, ec) ? "to put something inside a directory give the full new path, like " + (to / "name").string() : "delete it first with delete_file, or pick another name");
}

ToolResult move_tool(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return {false, "no such file or directory: " + from.string() + did_you_mean(from)};
    if (fs::exists(to, ec)) return {false, exists_already(to)};
    if (from == to) return {false, "from and to are the same path"};
    fs::create_directories(to.parent_path(), ec);
    if (ec = move_path(from, to); ec) return {false, "could not move " + from.string() + " to " + to.string() + ": " + ec.message()};
    return {true, "moved " + from.string() + " to " + to.string()};
}

ToolResult copy_tool(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return {false, "no such file or directory: " + from.string() + did_you_mean(from)};
    if (fs::exists(to, ec)) return {false, exists_already(to)};
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) return {false, "could not copy " + from.string() + " to " + to.string() + ": " + ec.message()};
    std::string what;
    if (fs::is_directory(to, ec)) {
        size_t files = 0, dirs = 0;
        count_tree(to, files, dirs);
        what = " (" + std::to_string(files) + " files)";
    }
    return {true, "copied " + from.string() + " to " + to.string() + what};
}

ToolResult delete_file(const Harness& harness, const fs::path& p, bool recursive) {
    std::error_code ec;
    if (p == harness.workspace()) return {false, "refusing to delete the workspace itself"};
    if (!fs::exists(p, ec)) return {false, "no such file or directory: " + p.string() + did_you_mean(p)};
    if (!fs::is_directory(p, ec)) {
        if (!fs::remove(p, ec)) return {false, "could not delete " + p.string() + ": " + ec.message()};
        return {true, "deleted " + p.string()};
    }
    size_t files = 0, dirs = 0;
    count_tree(p, files, dirs);
    if (files + dirs > 0 && !recursive) {
        return {false, p.string() + " is a directory with " + std::to_string(files) + " files and " + std::to_string(dirs) +
                           " directories inside. To delete all of it, call delete_file again with recursive set to true."};
    }
    fs::remove_all(p, ec);
    if (ec) return {false, "could not delete " + p.string() + ": " + ec.message()};
    return {true, "deleted " + p.string() + (files + dirs ? " (" + std::to_string(files) + " files, " + std::to_string(dirs) + " directories)" : " (empty directory)")};
}

ToolResult make_dir(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return {true, p.string() + " already exists"};
    if (fs::exists(p, ec)) return {false, p.string() + " exists and is a file, not a directory"};
    fs::create_directories(p, ec);
    if (ec) return {false, "could not create " + p.string() + ": " + ec.message()};
    return {true, "created " + p.string()};
}

ToolResult run_shell(const Harness& harness, const nlohmann::json& args, bool read_only, const std::atomic<bool>& cancel) {
    int secs = std::clamp(args.value("timeout_seconds", kDefaultShellTimeout), 1, kMaxShellTimeout);
    fs::path workdir;
    if (args.contains("workdir") && args["workdir"].is_string() && !args["workdir"].get<std::string>().empty()) {
        workdir = harness.resolve(args["workdir"].get<std::string>());
        std::error_code ec;
        if (!fs::is_directory(workdir, ec)) return {false, "workdir is not a directory: " + workdir.string()};
    }
    auto r = run_sandboxed(arg(args, "command"), harness.workspace(), read_only, std::chrono::seconds(secs), cancel, workdir);
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

// Removed and added lines between two texts, with the common head and tail left out. Capped.
std::string change_lines(const std::string& before, const std::string& after, size_t cap) {
    auto a = lines_of(before), b = lines_of(after);
    size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    size_t tail = 0;
    while (tail + head < a.size() && tail + head < b.size() && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    std::string out;
    size_t n = 0;
    for (size_t i = head; i + tail < a.size() && n < cap; ++i, ++n) out += "- " + a[i] + "\n";
    for (size_t i = head; i + tail < b.size() && n < cap; ++i, ++n) out += "+ " + b[i] + "\n";
    if ((a.size() - head - tail) + (b.size() - head - tail) > cap) out += "  … more\n";
    return out;
}

}  // namespace

std::error_code move_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec != std::errc::cross_device_link) return ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (!ec) fs::remove_all(from, ec);
    return ec;
}

std::string snake_tool_name(const std::string& name) {
    // Small models emit Read_File, readFile or read-file; map them back.
    std::string snake;
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isupper(c) && i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]))) snake += '_';
        snake += static_cast<char>(c == '-' || c == ' ' ? '_' : std::tolower(c));
    }
    return snake;
}

std::string canonical_tool_name(const std::string& name) {
    std::string snake = snake_tool_name(name);
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
           "need several files. For a big file, `grep` returns only the lines matching a regex, with their numbers: read "
           "only what matches, then read a window around the lines you need. Refuses binary files.",
           {{"path", {{"type", "string"}, {"description", "File path, relative to the workspace or absolute"}}},
            {"offset", {{"type", "integer"}, {"description", "First line to read (1-based)"}}},
            {"limit", {{"type", "integer"}, {"description", "Max lines to read (default and max 2000)"}}},
            {"grep", {{"type", "string"}, {"description", "Return only lines matching this extended regex (grep -E syntax)"}}}},
           {"path"}),
        fn("list_dir",
           "List a directory. Directories end with / and show their entry count; `depth` above 1 lists subdirectories too, "
           "as an indented tree (max 4; .git, build and similar are not expanded). Ends with the entry count. Use this "
           "instead of `ls` or `tree` in run_shell.",
           {{"path", {{"type", "string"}, {"description", "Directory path; '.' for the workspace"}}},
            {"depth", {{"type", "integer"}, {"description", "How many levels to show (default 1, max 4)"}}}},
           {"path"}),
        fn("glob",
           "Find files by name pattern, one relative path per line, sorted. `*` and `?` match within one path segment, "
           "`**` spans directories: `*.cpp` finds them at any depth, `src/*.cpp` only directly in src, `src/**/*.cpp` "
           "under src at any depth. Skips .git, build and similar. Use this instead of `find` in run_shell.",
           {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Directory to search; default '.'"}}}},
           {"pattern"}),
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
           "result says it was not found, read the file again and copy the exact text. For several changes to one "
           "file use multi_edit.",
           {{"path", {{"type", "string"}}},
            {"old_string", {{"type", "string"}}},
            {"new_string", {{"type", "string"}}},
            {"replace_all", {{"type", "boolean"}, {"description", "Replace every occurrence (default false)"}}}},
           {"path", "old_string", "new_string"}),
        fn("multi_edit",
           "Several edit_file replacements in one file, applied in order in one step: the file is written only if every "
           "edit matches, so a failed edit changes nothing. Each edit's old_string must match the file as the earlier "
           "edits left it. Same rules as edit_file for old_string. Prefer this over several edit_file calls to one file.",
           {{"path", {{"type", "string"}}},
            {"edits", {{"type", "array"},
                       {"items", {{"type", "object"},
                                  {"properties", {{"old_string", {{"type", "string"}}}, {"new_string", {{"type", "string"}}}, {"replace_all", {{"type", "boolean"}}}}},
                                  {"required", {"old_string", "new_string"}}}}}}},
           {"path", "edits"}),
        fn("apply_patch",
           "Apply a unified diff (the format of `diff -u` and `git diff`) to one or more files under the workspace: "
           "`--- old` and `+++ new` headers with paths relative to the workspace (a/ and b/ prefixes are fine), "
           "`@@ -start,count +start,count @@` hunks, lines starting with a space (context), - (removed) or + (added). "
           "Context lines must match the file exactly. `--- /dev/null` creates a file, `+++ /dev/null` deletes one. "
           "If any hunk fails, nothing is written and the result names the hunk and line. For one small change "
           "edit_file is simpler.",
           {{"patch", {{"type", "string"}, {"description", "The unified diff text"}}}}, {"patch"}),
        fn("move_file",
           "Move or rename a file or directory. `to` is the full new path (not a directory to move into) and must not "
           "exist yet; missing parent directories are created. Use this instead of `mv` in run_shell.",
           {{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}, {"from", "to"}),
        fn("copy_file",
           "Copy a file or directory. `to` is the full new path and must not exist yet; missing parent directories "
           "are created. Use this instead of `cp` in run_shell.",
           {{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}, {"from", "to"}),
        fn("delete_file",
           "Delete a file or an empty directory. A directory with contents needs recursive set to true; the result says "
           "how much went. Use this instead of `rm` in run_shell.",
           {{"path", {{"type", "string"}}}, {"recursive", {{"type", "boolean"}, {"description", "Also delete everything inside a directory (default false)"}}}},
           {"path"}),
        fn("make_dir", "Create a directory, with any missing parents. Fine if it already exists. Use this instead of `mkdir` in run_shell.",
           {{"path", {{"type", "string"}}}}, {"path"}),
        fn("run_shell",
           "Run a bash command in a sandbox: only the workspace is writable, no network, no sudo, a timeout (default "
           "120 s, max 600). For builds, tests, git and package tools. NOT for reading, listing, finding, searching, "
           "editing, moving, copying or deleting files: use read_file, list_dir, glob (instead of find), search_files "
           "(instead of grep), edit_file, write_file, move_file, copy_file, delete_file and make_dir for those; a "
           "shell mv, cp, rm or mkdir asks the user, the tools do not. Do not use echo or printf to talk to the user; "
           "write your answer as text.",
           {{"command", {{"type", "string"}}},
            {"timeout_seconds", {{"type", "integer"}, {"description", "Default 120, max 600"}}},
            {"workdir", {{"type", "string"}, {"description", "Directory to run in, instead of `cd dir && ...`"}}}},
           {"command"}),
        fn("question",
           "Ask the user one question and wait for the answer. Give options when there is a fixed choice (the user "
           "picks by number or types something else). Use it when a decision is theirs to make; do not use it for "
           "things you can find out with the other tools. The result is their answer, or a note that they gave none.",
           {{"question", {{"type", "string"}}},
            {"options", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Choices to offer, in order"}}}},
           {"question"}),
        fn("todo",
           "Your plan for work with several steps, shown to the user. Send the whole list every time: it replaces "
           "the previous one. Mark items done as you finish them and add items as you learn what is needed.",
           {{"items", {{"type", "array"},
                       {"items", {{"type", "object"},
                                  {"properties", {{"text", {{"type", "string"}}}, {"done", {{"type", "boolean"}}}}},
                                  {"required", {"text"}}}}}}},
           {"items"}),
        fn("delegate",
           "Hand a task to a subagent that runs in this workspace under a named profile and reports back; its "
           "answer is the result. Profiles: scout (reads and read-only commands only, for a long search or a read "
           "of many files you do not want in your own context: ask for a short report with paths and line numbers), "
           "reviewer (read-only, plan mode: a review of a change you made, against what was asked), builder (edits "
           "inside the workspace, for a self-contained piece of work). Give the whole task in `task`; the subagent "
           "has none of this conversation. `context` carries what it needs to know (file names, decisions). A "
           "subagent cannot delegate, ask the user or keep a plan; it works within its profile's budget and asks the "
           "user through you when its mode requires.",
           {{"profile", {{"type", "string"}, {"description", "scout, reviewer, builder, or a profile from settings"}}},
            {"task", {{"type", "string"}, {"description", "What to do and what to report back"}}},
            {"context", {{"type", "string"}, {"description", "Background the subagent needs (optional)"}}}},
           {"profile", "task"}),
    });
    return schemas;
}

namespace {

std::vector<Action> actions_of(const Harness& harness, const std::string& name, const nlohmann::json& args) {
    using K = Action::Kind;
    if (name == "read_file" || name == "list_dir") return {{K::Read, harness.resolve(arg(args, "path")), ""}};
    if (name == "glob" || name == "search_files") return {{K::Read, harness.resolve(args.value("path", ".")), ""}};
    if (name == "write_file" || name == "edit_file" || name == "multi_edit" || name == "delete_file" || name == "make_dir") {
        return {{K::Write, harness.resolve(arg(args, "path")), ""}};
    }
    if (name == "move_file") return {{K::Write, harness.resolve(arg(args, "from")), ""}, {K::Write, harness.resolve(arg(args, "to")), ""}};
    if (name == "copy_file") return {{K::Read, harness.resolve(arg(args, "from")), ""}, {K::Write, harness.resolve(arg(args, "to")), ""}};
    if (name == "apply_patch") {
        std::vector<Action> actions;
        for (const auto& f : parse_patch(arg(args, "patch"))) actions.push_back({K::Write, harness.resolve(f.path()), ""});
        return actions;
    }
    if (name == "run_shell") {
        Action a{K::Shell, {}, arg(args, "command")};
        if (args.contains("workdir") && args["workdir"].is_string() && !args["workdir"].get<std::string>().empty()) {
            a.workdir = harness.resolve(args["workdir"].get<std::string>());
        }
        return {a};
    }
    throw std::runtime_error("unknown tool: " + name + " (the tools are " + tool_names() + ")");
}

}  // namespace

std::vector<Action> tool_actions(const Harness& harness, const std::string& name, const nlohmann::json& args) {
    std::vector<Action> actions = actions_of(harness, name, args);
    for (auto& a : actions) a.tool = name;
    return actions;
}

std::string tool_preview(const Harness& harness, const std::string& name, const nlohmann::json& args) {
    try {
        std::error_code ec;
        if (name == "edit_file") return change_lines(arg(args, "old_string"), arg(args, "new_string"), 40);
        if (name == "multi_edit") {
            std::string out;
            const auto& edits = args.at("edits");
            for (size_t i = 0; i < edits.size(); ++i) {
                out += "edit " + std::to_string(i + 1) + " of " + std::to_string(edits.size()) + ":\n" + change_lines(arg(edits[i], "old_string"), arg(edits[i], "new_string"), 40);
            }
            return out;
        }
        if (name == "write_file") {
            fs::path p = harness.resolve(arg(args, "path"));
            std::string content = arg(args, "content");
            if (fs::is_regular_file(p, ec)) {
                std::string before = read_whole(p);
                return "replaces " + std::to_string(count_lines(before)) + " lines with " + std::to_string(count_lines(content)) + ":\n" + change_lines(before, content, 40);
            }
            return "new file, " + std::to_string(count_lines(content)) + " lines:\n" + change_lines("", content, 12);
        }
        if (name == "apply_patch") {
            auto lines = lines_of(arg(args, "patch"));
            std::string out;
            for (size_t i = 0; i < lines.size() && i < 60; ++i) out += lines[i] + "\n";
            if (lines.size() > 60) out += "  … " + std::to_string(lines.size() - 60) + " more lines\n";
            return out;
        }
        if (name == "delete_file") {
            fs::path p = harness.resolve(arg(args, "path"));
            if (fs::is_directory(p, ec)) {
                size_t files = 0, dirs = 0;
                count_tree(p, files, dirs);
                if (files + dirs == 0) return "deletes an empty directory\n";
                return "deletes a directory with " + std::to_string(files) + " files and " + std::to_string(dirs) + " directories inside\n";
            }
            if (!fs::is_regular_file(p, ec)) return "";
            std::string content = read_whole(p);
            if (looks_binary(content.substr(0, 4096))) return "deletes a binary file of " + std::to_string(content.size()) + " bytes\n";
            return "deletes " + std::to_string(count_lines(content)) + " lines:\n" + change_lines(content, "", 12);
        }
    } catch (const std::exception&) {
    }
    return "";
}

std::string tool_summary(const std::string& name, const nlohmann::json& args) {
    if (name == "run_shell") return "$ " + args.value("command", "");
    if (name == "search_files") return "search /" + args.value("pattern", "") + "/ in " + args.value("path", ".");
    if (name == "glob") return "glob " + args.value("pattern", "") + " in " + args.value("path", ".");
    if (name == "question") return "question: " + args.value("question", "");
    if (name == "todo") return "todo (" + std::to_string(args.contains("items") && args["items"].is_array() ? args["items"].size() : 0) + " items)";
    if (name == "delegate") {
        std::string task = args.value("task", "");
        for (char& c : task) if (c == '\n') c = ' ';
        return "delegate " + args.value("profile", "") + ": " + (task.size() > 100 ? task.substr(0, 97) + "..." : task);
    }
    if (name == "move_file" || name == "copy_file") return name + " " + args.value("from", "") + " -> " + args.value("to", "");
    if (name == "delete_file") return "delete_file " + args.value("path", "") + (flag(args, "recursive") ? " (recursive)" : "");
    if (name == "multi_edit") return "multi_edit " + args.value("path", "") + " (" + std::to_string(args.contains("edits") && args["edits"].is_array() ? args["edits"].size() : 0) + " edits)";
    if (name == "apply_patch") {
        std::string files;
        try {
            for (const auto& f : parse_patch(args.value("patch", ""))) files += (files.empty() ? "" : ", ") + f.path();
        } catch (const std::exception&) {
            files = "(not a unified diff)";
        }
        return "apply_patch " + files;
    }
    return name + " " + args.value("path", "");
}

ToolResult run_tool(const Harness& harness, const std::string& name, const nlohmann::json& args,
                    bool read_only_sandbox, const std::atomic<bool>& cancel) {
    try {
        if (name == "read_file") return read_file(harness.resolve(arg(args, "path")), args);
        if (name == "list_dir") return list_dir(harness, harness.resolve(arg(args, "path")), args);
        if (name == "glob") return glob_files(harness, harness.resolve(args.value("path", ".")), arg(args, "pattern"));
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
        if (name == "edit_file") return edit_file(harness.resolve(arg(args, "path")), arg(args, "old_string"), arg(args, "new_string"), flag(args, "replace_all"));
        if (name == "multi_edit") return multi_edit(harness.resolve(arg(args, "path")), args.value("edits", nlohmann::json()));
        if (name == "apply_patch") return apply_patch(harness, arg(args, "patch"));
        if (name == "move_file") return move_tool(harness.resolve(arg(args, "from")), harness.resolve(arg(args, "to")));
        if (name == "copy_file") return copy_tool(harness.resolve(arg(args, "from")), harness.resolve(arg(args, "to")));
        if (name == "delete_file") return delete_file(harness, harness.resolve(arg(args, "path")), flag(args, "recursive"));
        if (name == "make_dir") return make_dir(harness.resolve(arg(args, "path")));
        if (name == "run_shell") return run_shell(harness, args, read_only_sandbox, cancel);
        return {false, "unknown tool: " + name + " (the tools are " + tool_names() + ")"};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

}  // namespace maic
