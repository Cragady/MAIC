#include "maic/script_tools.hpp"

#include "maic/lua_tools.hpp"
#include "maic/sandbox.hpp"
#include "maic/trust.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int kMaxTimeout = 600;
const char* const kTypes[] = {"object", "array", "string", "integer", "number", "boolean", "null"};

bool snake_case(const std::string& name) {
    if (name.empty() || name.size() > 64 || !std::islower(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; });
}

bool executable(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

// run[0] as the sandbox will find it: an absolute path, a path relative to the tool's directory, or a name on PATH.
bool program_found(const std::string& prog, const fs::path& dir) {
    if (prog.find('/') != std::string::npos) return executable(prog[0] == '/' ? fs::path(prog) : dir / prog);
    std::istringstream path(std::getenv("PATH") ? std::getenv("PATH") : "");
    for (std::string entry; std::getline(path, entry, ':');) {
        if (!entry.empty() && executable(fs::path(entry) / prog)) return true;
    }
    return false;
}

std::vector<std::string> string_list(const json& manifest, const char* key) {
    std::vector<std::string> out;
    if (!manifest.contains(key)) return out;
    if (!manifest[key].is_array()) throw std::runtime_error(std::string("`") + key + "` must be a list of glob patterns");
    for (const auto& v : manifest[key]) {
        if (!v.is_string() || v.get<std::string>().empty()) throw std::runtime_error(std::string("`") + key + "` must be a list of glob patterns");
        out.push_back(v.get<std::string>());
    }
    return out;
}

std::string type_name(const json& v) {
    switch (v.type()) {
        case json::value_t::object: return "object";
        case json::value_t::array: return "array";
        case json::value_t::string: return "string";
        case json::value_t::boolean: return "boolean";
        case json::value_t::null: return "null";
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: return "integer";
        default: return "number";
    }
}

bool is_type(const json& v, const std::string& t) {
    if (t == "integer") return v.is_number_integer() || (v.is_number_float() && v.get<double>() == static_cast<long long>(v.get<double>()));
    if (t == "number") return v.is_number();
    return type_name(v) == t;
}

std::string check_schema_at(const json& s, const std::string& where) {
    if (!s.is_object()) return where + " must be an object";
    if (s.contains("type")) {
        std::vector<std::string> types;
        if (s["type"].is_string()) types.push_back(s["type"]);
        else if (s["type"].is_array()) for (const auto& t : s["type"]) types.push_back(t.is_string() ? t.get<std::string>() : "?");
        else return where + ": `type` must be a string or a list of strings";
        for (const auto& t : types) {
            if (std::find(std::begin(kTypes), std::end(kTypes), t) == std::end(kTypes)) return where + ": unknown type `" + t + "`";
        }
    }
    if (s.contains("properties")) {
        if (!s["properties"].is_object()) return where + ": `properties` must be an object";
        for (const auto& [k, v] : s["properties"].items()) {
            if (std::string e = check_schema_at(v, where + "." + k); !e.empty()) return e;
        }
    }
    if (s.contains("required")) {
        if (!s["required"].is_array()) return where + ": `required` must be a list of property names";
        for (const auto& r : s["required"]) {
            if (!r.is_string()) return where + ": `required` must be a list of property names";
            if (!s.contains("properties") || !s["properties"].contains(r.get<std::string>())) return where + ": required `" + r.get<std::string>() + "` is not in `properties`";
        }
    }
    if (s.contains("items")) {
        if (std::string e = check_schema_at(s["items"], where + "[]"); !e.empty()) return e;
    }
    if (s.contains("enum") && (!s["enum"].is_array() || s["enum"].empty())) return where + ": `enum` must be a non-empty list";
    return "";
}

std::string check_value(const json& s, const json& v, const std::string& where) {
    if (s.contains("type")) {
        std::vector<std::string> types;
        if (s["type"].is_string()) types.push_back(s["type"]);
        else for (const auto& t : s["type"]) types.push_back(t);
        if (std::none_of(types.begin(), types.end(), [&](const std::string& t) { return is_type(v, t); })) {
            std::string want;
            for (const auto& t : types) want += (want.empty() ? "" : " or ") + t;
            return "argument " + where + " must be " + (want == "array" || want == "object" || want == "integer" ? "an " : "a ") + want + ", not " + type_name(v);
        }
    }
    if (s.contains("enum") && s["enum"].is_array() && std::find(s["enum"].begin(), s["enum"].end(), v) == s["enum"].end()) {
        std::string opts;
        for (const auto& o : s["enum"]) opts += (opts.empty() ? "" : ", ") + (o.is_string() ? o.get<std::string>() : o.dump());
        return "argument " + where + " must be one of: " + opts;
    }
    if (v.is_object()) {
        const json props = s.value("properties", json::object());
        if (s.contains("required") && s["required"].is_array()) {
            for (const auto& r : s["required"]) {
                if (r.is_string() && !v.contains(r.get<std::string>())) return "missing required argument " + (where.empty() ? "" : where + ".") + r.get<std::string>();
            }
        }
        bool closed = s.contains("additionalProperties") && s["additionalProperties"].is_boolean() && !s["additionalProperties"].get<bool>();
        for (const auto& [k, val] : v.items()) {
            std::string path = where.empty() ? k : where + "." + k;
            if (props.contains(k)) {
                if (std::string e = check_value(props[k], val, path); !e.empty()) return e;
            } else if (closed) {
                return "unknown argument " + path;
            }
        }
    }
    if (v.is_array() && s.contains("items") && s["items"].is_object()) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (std::string e = check_value(s["items"], v[i], where + "[" + std::to_string(i) + "]"); !e.empty()) return e;
        }
    }
    return "";
}

// The part of a glob before its first wildcard component: what the harness can judge as a path.
std::string glob_prefix(const std::string& glob) {
    std::string prefix;
    std::istringstream in(glob);
    bool first = true;
    for (std::string part; std::getline(in, part, '/'); first = false) {
        if (part.find_first_of("*?[") != std::string::npos) break;
        if (first && part.empty()) {
            prefix = "/";
            continue;
        }
        if (!prefix.empty() && prefix != "/") prefix += '/';
        prefix += part;
    }
    return prefix.empty() ? "." : prefix;
}

bool inside(const fs::path& p, const fs::path& root) {
    fs::path rel = p.lexically_relative(root);
    return !rel.empty() && *rel.begin() != "..";
}

const char* stub_name(const std::string& lang) {
    if (lang == "python") return "main.py";
    if (lang == "sh") return "main.sh";
    if (lang == "perl") return "main.pl";
    if (lang == "node") return "main.js";
    return nullptr;
}

std::string stub_text(const std::string& lang, const std::string& name) {
    std::string head = name + ": a MAIC script tool. The arguments arrive as JSON on stdin; whatever is printed is the result.";
    if (lang == "python") {
        return "#!/usr/bin/env python3\n\"\"\"" + head + "\"\"\"\nimport json\nimport sys\n\nargs = json.load(sys.stdin)\nprint(json.dumps(args, indent=2))\n";
    }
    if (lang == "sh") {
        return "#!/bin/sh\n# " + head + "\nargs=$(cat)\nprintf '%s\\n' \"$args\"\n";
    }
    if (lang == "perl") {
        return "#!/usr/bin/env perl\n# " + head + "\nuse strict;\nuse warnings;\nuse JSON::PP;\n\nlocal $/;\nmy $args = decode_json(<STDIN>);\nprint JSON::PP->new->pretty->canonical->encode($args);\n";
    }
    return "// " + head + "\nlet input = \"\";\nprocess.stdin.on(\"data\", (chunk) => (input += chunk));\nprocess.stdin.on(\"end\", () => {\n  const args = JSON.parse(input || \"{}\");\n  console.log(JSON.stringify(args, null, 2));\n});\n";
}

}  // namespace

ScriptTool read_script_tool(const fs::path& manifest) {
    std::ifstream in(manifest, std::ios::binary);
    if (!in) throw std::runtime_error("can't read it");
    json m = json::parse(in, nullptr, false);
    if (m.is_discarded()) throw std::runtime_error("not valid JSON");
    if (!m.is_object()) throw std::runtime_error("the manifest must be a JSON object");
    ScriptTool tool;
    tool.dir = manifest.parent_path();
    if (!m.contains("name") || !m["name"].is_string()) throw std::runtime_error("the manifest needs a string `name`");
    tool.name = m["name"];
    if (!snake_case(tool.name)) throw std::runtime_error("`name` must be snake_case (a lowercase letter, then lowercase letters, digits and underscores): " + tool.name);
    if (!canonical_tool_name(tool.name).empty()) throw std::runtime_error("`" + tool.name + "` is a built-in tool");
    if (!m.contains("description") || !m["description"].is_string() || m["description"].get<std::string>().empty()) throw std::runtime_error("the manifest needs a string `description`");
    tool.description = m["description"];
    tool.parameters = m.contains("parameters") ? m["parameters"] : json{{"type", "object"}, {"properties", json::object()}};
    if (!tool.parameters.is_object()) throw std::runtime_error("`parameters` must be a JSON schema object");
    if (!tool.parameters.contains("type")) tool.parameters["type"] = "object";
    if (!tool.parameters.contains("properties")) tool.parameters["properties"] = json::object();
    if (tool.parameters["type"] != "object") throw std::runtime_error("`parameters` must describe an object (type: object)");
    if (std::string e = check_schema(tool.parameters); !e.empty()) throw std::runtime_error("`parameters`: " + e);
    if (!m.contains("run") || !m["run"].is_array() || m["run"].empty()) throw std::runtime_error("`run` must be a non-empty list: the program and its arguments");
    for (const auto& a : m["run"]) {
        if (!a.is_string() || a.get<std::string>().empty()) throw std::runtime_error("`run` must be a list of strings");
        std::string arg = a;
        std::error_code ec;
        if (tool.run.size() > 0 && fs::is_regular_file(tool.dir / arg, ec)) arg = (tool.dir / arg).string();
        tool.run.push_back(arg);
    }
    if (!program_found(tool.run[0], tool.dir)) throw std::runtime_error("`" + tool.run[0] + "` is not on PATH (and not a file in the tool's directory)");
    if (tool.run[0].find('/') != std::string::npos && tool.run[0][0] != '/') tool.run[0] = (tool.dir / tool.run[0]).string();
    if (m.contains("timeout_s")) {
        if (!m["timeout_s"].is_number_integer() || m["timeout_s"].get<int>() < 1 || m["timeout_s"].get<int>() > kMaxTimeout) {
            throw std::runtime_error("`timeout_s` must be a whole number of seconds from 1 to " + std::to_string(kMaxTimeout));
        }
        tool.timeout_s = m["timeout_s"];
    }
    if (m.contains("network")) {
        if (!m["network"].is_boolean()) throw std::runtime_error("`network` must be true or false");
        if (m["network"].get<bool>()) throw std::runtime_error("per-tool network grants are not implemented yet; set `network` to false or leave it out");
    }
    tool.reads = string_list(m, "reads");
    tool.writes = string_list(m, "writes");
    return tool;
}

ScriptToolSet load_script_tools(const fs::path& workspace, const std::vector<std::string>& taken) {
    ScriptToolSet set;
    for (const fs::path& dir : {workspace / ".maic" / "tools", global_tools_dir()}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        if (dir != global_tools_dir() && !trusted(workspace)) continue;  // an untrusted project's tools are never loaded
        std::vector<fs::path> manifests;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_directory(ec) && fs::is_regular_file(e.path() / "tool.json", ec)) manifests.push_back(e.path() / "tool.json");
        }
        std::sort(manifests.begin(), manifests.end());
        for (const auto& manifest : manifests) {
            try {
                ScriptTool tool = read_script_tool(manifest);
                if (std::find(taken.begin(), taken.end(), tool.name) != taken.end()) throw std::runtime_error("`" + tool.name + "` is already defined by a Lua tool");
                auto dup = std::find_if(set.tools.begin(), set.tools.end(), [&](const ScriptTool& t) { return t.name == tool.name; });
                if (dup != set.tools.end()) throw std::runtime_error("`" + tool.name + "` is already defined by " + (dup->dir / "tool.json").string());
                set.tools.push_back(std::move(tool));
            } catch (const std::exception& e) {
                set.notices.push_back("tool skipped: " + manifest.string() + ": " + e.what());
            }
        }
    }
    return set;
}

std::string script_tool_language(const ScriptTool& tool) {
    std::string prog = fs::path(tool.run[0]).filename().string();
    if (prog.rfind("python", 0) == 0) return "python";
    if (prog == "sh" || prog == "bash" || prog == "dash" || prog == "zsh") return "sh";
    if (prog == "perl") return "perl";
    if (prog == "node") return "node";
    if (prog == "deno") return "deno";
    if (prog == "wasmtime") return "wasm";
    return prog;
}

std::string check_schema(const json& schema) {
    return check_schema_at(schema, "parameters");
}

std::string check_arguments(const json& schema, const json& args) {
    if (!args.is_object()) return "the arguments must be a JSON object";
    return check_value(schema, args, "");
}

std::vector<Action> script_tool_actions(const Harness& harness, const ScriptTool& tool) {
    std::vector<Action> actions;
    for (const auto& glob : tool.reads) actions.push_back({Action::Kind::Read, harness.resolve(glob_prefix(glob)), "", {}, tool.name});
    for (const auto& glob : tool.writes) {
        fs::path p = harness.resolve(glob_prefix(glob));
        if (!inside(p, harness.workspace()) && p != harness.workspace()) {
            throw std::runtime_error("a script tool writes only inside the workspace; `" + glob + "` in " + tool.name + "'s manifest reaches " + p.string());
        }
        actions.push_back({Action::Kind::Write, p, "", {}, tool.name});
    }
    return actions;
}

ToolResult run_script_tool(const ScriptTool& tool, const json& args, const Harness& harness, bool read_only, const std::atomic<bool>& cancel) {
    SandboxResult r = run_sandboxed_argv(tool.run, args.dump(), harness.workspace(), read_only, std::chrono::seconds(tool.timeout_s), cancel);
    auto trimmed = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    std::string out = trimmed(r.output), err = trimmed(r.error);
    if (r.cancelled) return {false, "the user cancelled " + tool.name};
    if (r.timed_out) {
        return {false, "terminated: " + tool.name + " exceeded its timeout of " + std::to_string(tool.timeout_s) + " s (timeout_s in its manifest)" + (out.empty() ? "" : "\n" + out) + (err.empty() ? "" : "\nstderr:\n" + err)};
    }
    if (r.exit_code != 0) {
        return {false, "exit code " + std::to_string(r.exit_code) + (out.empty() ? "" : "\n" + out) + (err.empty() ? "" : "\nstderr:\n" + err)};
    }
    return {true, out.empty() ? "(no output)" : out};
}

std::vector<fs::path> scaffold_script_tool(const fs::path& dir, const std::string& name, const std::string& lang) {
    if (!snake_case(name)) throw std::runtime_error("the name must be snake_case: a lowercase letter, then lowercase letters, digits and underscores");
    if (!canonical_tool_name(name).empty()) throw std::runtime_error("`" + name + "` is a built-in tool");
    const char* stub = stub_name(lang);
    if (!stub) throw std::runtime_error("--lang must be python, sh, perl or node");
    std::error_code ec;
    if (fs::exists(dir, ec)) throw std::runtime_error(dir.string() + " already exists");
    fs::create_directories(dir);
    const char* program = lang == "python" ? "python3" : lang == "sh" ? "sh" : lang == "perl" ? "perl" : "node";
    json manifest = {
        {"name", name},
        {"description", "Say what the tool does and when the model should use it."},
        {"parameters", {{"type", "object"}, {"properties", {{"text", {{"type", "string"}, {"description", "What to work on"}}}}}, {"required", {"text"}}}},
        {"run", {program, stub}},
        {"timeout_s", 60},
        {"network", false},
        {"reads", json::array()},
        {"writes", json::array()},
    };
    std::ofstream(dir / "tool.json") << manifest.dump(2) << "\n";
    std::ofstream(dir / stub) << stub_text(lang, name);
    fs::permissions(dir / stub, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
    return {dir / "tool.json", dir / stub};
}

}  // namespace maic
