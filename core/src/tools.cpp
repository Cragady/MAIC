#include "maic/tools.hpp"

#include "maic/sandbox.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxReadLines = 2000;
constexpr size_t kMaxLineChars = 2000;
constexpr size_t kMaxListEntries = 500;
constexpr size_t kMaxSearchHits = 200;
constexpr int kDefaultShellTimeout = 120;
constexpr int kMaxShellTimeout = 600;

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
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't open " + p.string());
    }
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_whole(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) {
        throw std::runtime_error("can't write " + p.string());
    }
}

bool skip_dir(const fs::path& p) {
    static const std::vector<std::string> skip = {".git", "build", "node_modules", ".venv", "__pycache__", "vcpkg_installed"};
    return std::find(skip.begin(), skip.end(), p.filename().string()) != skip.end();
}

ToolResult read_file(const fs::path& p, const nlohmann::json& args) {
    size_t offset = args.value("offset", 1);
    size_t limit = std::min<size_t>(args.value("limit", kMaxReadLines), kMaxReadLines);
    std::ifstream in(p);
    if (!in) {
        return {false, "can't open " + p.string()};
    }
    std::ostringstream out;
    std::string line;
    size_t n = 0, shown = 0;
    while (std::getline(in, line)) {
        if (++n < offset) {
            continue;
        }
        if (shown == limit) {
            out << "... (more lines; read again with offset=" << n << ")\n";
            break;
        }
        if (line.size() > kMaxLineChars) {
            line = line.substr(0, kMaxLineChars) + " ...";
        }
        out << n << "\t" << line << "\n";
        ++shown;
    }
    return {true, shown ? out.str() : "(empty or past end of file)"};
}

ToolResult list_dir(const fs::path& p) {
    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(p)) {
        entries.push_back(e.path().filename().string() + (e.is_directory() ? "/" : ""));
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream out;
    for (size_t i = 0; i < entries.size() && i < kMaxListEntries; ++i) {
        out << entries[i] << "\n";
    }
    if (entries.size() > kMaxListEntries) {
        out << "... " << entries.size() - kMaxListEntries << " more\n";
    }
    return {true, entries.empty() ? "(empty directory)" : out.str()};
}

ToolResult search_files(const Harness& harness, const fs::path& root, const std::string& pattern) {
    std::regex re;
    try {
        re = std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        return {false, std::string("bad regex: ") + e.what()};
    }
    std::ostringstream out;
    size_t hits = 0;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied);
    for (; it != fs::recursive_directory_iterator() && hits < kMaxSearchHits; ++it) {
        if (it->is_directory() && (skip_dir(it->path()) || harness.is_secret(it->path()))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || it->file_size() > 2 * 1024 * 1024) {
            continue;
        }
        std::ifstream in(it->path());
        std::string line;
        for (size_t n = 1; std::getline(in, line) && hits < kMaxSearchHits; ++n) {
            if (line.find('\0') != std::string::npos) {
                break;  // binary file
            }
            if (std::regex_search(line, re)) {
                out << fs::relative(it->path(), harness.workspace()).string() << ":" << n << ": "
                    << line.substr(0, 300) << "\n";
                ++hits;
            }
        }
    }
    if (hits == kMaxSearchHits) {
        out << "... stopped at " << kMaxSearchHits << " matches\n";
    }
    return {true, hits ? out.str() : "no matches"};
}

ToolResult edit_file(const fs::path& p, const std::string& old_s, const std::string& new_s) {
    std::string text = read_whole(p);
    size_t first = text.find(old_s);
    if (old_s.empty() || first == std::string::npos) {
        return {false, "old_string not found in " + p.string()};
    }
    if (text.find(old_s, first + 1) != std::string::npos) {
        return {false, "old_string appears more than once; include more surrounding text"};
    }
    text.replace(first, old_s.size(), new_s);
    write_whole(p, text);
    return {true, "edited " + p.string()};
}

ToolResult run_shell(const Harness& harness, const nlohmann::json& args, const std::atomic<bool>& cancel) {
    int secs = std::clamp(args.value("timeout_seconds", kDefaultShellTimeout), 1, kMaxShellTimeout);
    auto r = run_sandboxed(arg(args, "command"), harness.workspace(), std::chrono::seconds(secs), cancel);
    std::string status = r.timed_out   ? "timed out after " + std::to_string(secs) + "s"
                         : r.cancelled ? "cancelled"
                                       : "exit code " + std::to_string(r.exit_code);
    return {r.exit_code == 0 && !r.timed_out && !r.cancelled, status + "\n" + r.output};
}

}  // namespace

const nlohmann::json& tool_schemas() {
    static const nlohmann::json schemas = nlohmann::json::array({
        fn("read_file", "Read a text file. Returns numbered lines.",
           {{"path", {{"type", "string"}, {"description", "File path, relative to the workspace or absolute"}}},
            {"offset", {{"type", "integer"}, {"description", "First line to read (1-based)"}}},
            {"limit", {{"type", "integer"}, {"description", "Max lines to read"}}}},
           {"path"}),
        fn("list_dir", "List a directory. Directories end with /.",
           {{"path", {{"type", "string"}, {"description", "Directory path; '.' for the workspace"}}}}, {"path"}),
        fn("search_files", "Search file contents with a regular expression (ECMAScript syntax). Skips .git, build and similar.",
           {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Directory to search; default '.'"}}}},
           {"pattern"}),
        fn("write_file", "Create or overwrite a file with the given content.",
           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}, {"path", "content"}),
        fn("edit_file", "Replace one exact occurrence of old_string with new_string in a file.",
           {{"path", {{"type", "string"}}}, {"old_string", {{"type", "string"}}}, {"new_string", {{"type", "string"}}}},
           {"path", "old_string", "new_string"}),
        fn("run_shell",
           "Run a bash command in a sandbox: only the workspace is writable, there is no network access, and no sudo.",
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
    throw std::runtime_error("unknown tool: " + name);
}

std::string tool_summary(const std::string& name, const nlohmann::json& args) {
    if (name == "run_shell") return "$ " + args.value("command", "");
    if (name == "search_files") return "search /" + args.value("pattern", "") + "/ in " + args.value("path", ".");
    return name + " " + args.value("path", "");
}

ToolResult run_tool(const Harness& harness, const std::string& name, const nlohmann::json& args,
                    const std::atomic<bool>& cancel) {
    try {
        if (name == "read_file") return read_file(harness.resolve(arg(args, "path")), args);
        if (name == "list_dir") return list_dir(harness.resolve(arg(args, "path")));
        if (name == "search_files") return search_files(harness, harness.resolve(args.value("path", ".")), arg(args, "pattern"));
        if (name == "write_file") {
            fs::path p = harness.resolve(arg(args, "path"));
            write_whole(p, arg(args, "content"));
            return {true, "wrote " + p.string()};
        }
        if (name == "edit_file") return edit_file(harness.resolve(arg(args, "path")), arg(args, "old_string"), arg(args, "new_string"));
        if (name == "run_shell") return run_shell(harness, args, cancel);
        return {false, "unknown tool: " + name};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

}  // namespace maic
