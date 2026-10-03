#include "maid/lua_tools.hpp"

#include "lua_json.hpp"
#include "maid/nvim_host.hpp"
#include "maid/paths.hpp"
#include "maid/sandbox.hpp"
#include "maid/trust.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace maid {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxOutput = 64 * 1024;
constexpr int kHookEvery = 1000;  // VM instructions between cancel and deadline checks
constexpr int kDefaultShellTimeout = 120;
constexpr int kMaxShellTimeout = 600;
const char* const kCtxKey = "maid.tool";

// What one call's C functions need; a light userdata in the registry, since every call has its own state.
struct Ctx {
    const LuaTool* tool;
    const Harness* harness;
    const Authorise* authorise;
    const std::atomic<bool>* cancel;
    std::chrono::steady_clock::time_point deadline;
    std::string output;  // print()
    NvimHost* nvim;
    const OnOutput* on_output;
    size_t shell_bytes = 0;  // maid.shell output so far this call: the next command's stream continues from here
};

Ctx& ctx(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kCtxKey);
    auto* c = static_cast<Ctx*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *c;
}

// Raised with the text as is: no position prefix, so a denial reaches the model word for word.
int fail(lua_State* L, const std::string& message) {
    lua_pushlstring(L, message.data(), message.size());
    return lua_error(L);
}

void hook(lua_State* L, lua_Debug*) {
    Ctx& c = ctx(L);
    if (c.cancel && c.cancel->load()) fail(L, "cancelled by the user");
    if (std::chrono::steady_clock::now() > c.deadline) fail(L, "tool " + c.tool->name + " aborted: it ran for longer than its time limit");
}

// A fresh state with the safe part of the standard library and nothing that reaches the machine.
lua_State* sandboxed_state() {
    lua_State* L = luaL_newstate();
    for (auto [name, open] : {std::pair<const char*, lua_CFunction>{"", luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                              {LUA_MATHLIBNAME, luaopen_math}, {LUA_BITLIBNAME, luaopen_bit}}) {
        lua_pushcfunction(L, open);
        lua_pushstring(L, name);
        lua_call(L, 1, 0);
    }
    for (const char* g : {"load", "loadstring", "dofile", "loadfile", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, g);
    }
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);  // the count hook must see every loop
    return L;
}

// Loads the tool file and leaves its table on the stack. Throws with the Lua error.
void load_table(lua_State* L, const std::string& source, const fs::path& file) {
    std::string chunk = "@" + file.string();
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        throw std::runtime_error(err);
    }
    if (!lua_istable(L, -1)) throw std::runtime_error("the file must return a table");
}

Decision gate(lua_State* L, const Action& action, const std::string& summary, const std::string& preview) {
    Ctx& c = ctx(L);
    Decision d = (*c.authorise)(action, summary, preview);
    if (d.verdict != Verdict::Allow) fail(L, d.reason);
    return d;
}

int l_print(lua_State* L) {
    Ctx& c = ctx(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) c.output += '\t';
        size_t len = 0;
        if (const char* s = lua_tolstring(L, i, &len)) c.output.append(s, len);
        else c.output += luaL_typename(L, i);
    }
    c.output += '\n';
    if (c.output.size() > kMaxOutput) fail(L, "tool " + c.tool->name + " printed more than 64 KB");
    return 0;
}

int l_read(lua_State* L) {
    Ctx& c = ctx(L);
    std::string path = luaL_checkstring(L, 1);
    fs::path p = c.harness->resolve(path);
    gate(L, {Action::Kind::Read, p, "", {}, "read_file"}, "read_file " + path, "");
    std::ifstream in(p, std::ios::binary);
    if (!in) return fail(L, "can't read " + p.string());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int l_write(lua_State* L) {
    Ctx& c = ctx(L);
    std::string path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* text = luaL_checklstring(L, 2, &len);
    nlohmann::json args = {{"path", path}, {"content", std::string(text, len)}};
    gate(L, {Action::Kind::Write, c.harness->resolve(path), "", {}, "write_file"}, "write_file " + path, tool_preview(*c.harness, "write_file", args));
    ToolResult r = run_tool(*c.harness, "write_file", args, false, *c.cancel);
    if (!r.ok) return fail(L, r.text);
    return 0;
}

int l_list(lua_State* L) {
    Ctx& c = ctx(L);
    std::string path = lua_isnoneornil(L, 1) ? "." : luaL_checkstring(L, 1);
    fs::path p = c.harness->resolve(path);
    gate(L, {Action::Kind::Read, p, "", {}, "list_dir"}, "list_dir " + path, "");
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return fail(L, "no such directory: " + p.string());
    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(p, ec)) entries.push_back(e.path().filename().string() + (e.is_directory(ec) ? "/" : ""));
    std::sort(entries.begin(), entries.end());
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        lua_pushlstring(L, entries[i].data(), entries[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_search(lua_State* L) {
    Ctx& c = ctx(L);
    std::string pattern = luaL_checkstring(L, 1);
    std::string path = lua_isnoneornil(L, 2) ? "." : luaL_checkstring(L, 2);
    gate(L, {Action::Kind::Read, c.harness->resolve(path), "", {}, "search_files"}, "search /" + pattern + "/ in " + path, "");
    ToolResult r = run_tool(*c.harness, "search_files", {{"pattern", pattern}, {"path", path}}, false, *c.cancel);
    if (!r.ok) return fail(L, r.text);
    lua_pushlstring(L, r.text.data(), r.text.size());
    return 1;
}

int l_shell(lua_State* L) {
    Ctx& c = ctx(L);
    std::string command = luaL_checkstring(L, 1);
    Action a{Action::Kind::Shell, {}, command, {}, "run_shell"};
    int secs = kDefaultShellTimeout;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "workdir");
        if (lua_isstring(L, -1)) a.workdir = c.harness->resolve(lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout");
        if (lua_isnumber(L, -1)) secs = std::clamp(static_cast<int>(lua_tonumber(L, -1)), 1, kMaxShellTimeout);
        lua_pop(L, 1);
    }
    Decision d = gate(L, a, "$ " + command, "");
    OutputTaps taps;
    if (*c.on_output) {
        taps.on_output = [&c, base = c.shell_bytes](OutputStream s, std::string_view bytes, size_t offset) { (*c.on_output)(s, bytes, base + offset); };
    }
    SandboxResult r = run_sandboxed(command, c.harness->workspace(), d.read_only_sandbox, std::chrono::seconds(secs), *c.cancel, a.workdir, taps);
    c.shell_bytes += r.output_bytes;
    if (r.cancelled) return fail(L, "cancelled by the user");
    if (r.timed_out) return fail(L, "command exceeded its timeout of " + std::to_string(secs) + " s");
    lua_pushlstring(L, r.output.data(), r.output.size());
    lua_pushinteger(L, r.exit_code);
    return 2;
}

int l_json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    std::string s = lua_to_json(L, 1).dump();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int l_json_decode(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    nlohmann::json j = nlohmann::json::parse(s, s + len, nullptr, false);
    if (j.is_discarded()) return fail(L, "json_decode: not valid JSON");
    json_to_lua(L, j);
    return 1;
}

// maid.nvim.diagnostics(path?) and maid.nvim.buffers(): reads, judged like read_file of the path (or of the
// workspace for the whole list), answering only for files in the workspace.
int nvim_answer(lua_State* L, const std::function<nlohmann::json(NvimHost&)>& call) {
    Ctx& c = ctx(L);
    std::string err;
    nlohmann::json result;
    if (!c.nvim->connected()) err = "maid.nvim: the nvim host is gone";
    else {
        try {
            result = call(*c.nvim);
        } catch (const std::exception& e) {
            err = std::string("maid.nvim: ") + e.what();
        }
    }
    if (!err.empty()) return fail(L, err);
    json_to_lua(L, result);
    return 1;
}

nlohmann::json in_workspace(const Harness& harness, const nlohmann::json& entries) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& e : entries) {
        fs::path rel = fs::path(e.value("path", "")).lexically_relative(harness.workspace());
        if (!rel.empty() && *rel.begin() != "..") out.push_back(e);
    }
    return out;
}

int l_nvim_diagnostics(lua_State* L) {
    Ctx& c = ctx(L);
    std::string path = lua_isnoneornil(L, 1) ? "" : luaL_checkstring(L, 1);
    fs::path p = path.empty() ? c.harness->workspace() : c.harness->resolve(path);
    gate(L, {Action::Kind::Read, p, "", {}, "diagnostics"}, "diagnostics " + (path.empty() ? std::string("(workspace)") : path), "");
    return nvim_answer(L, [&](NvimHost& h) {
        nlohmann::json all = host_diagnostics(h, path.empty() ? fs::path() : p);
        return path.empty() ? in_workspace(*c.harness, all) : all;
    });
}

int l_nvim_buffers(lua_State* L) {
    Ctx& c = ctx(L);
    gate(L, {Action::Kind::Read, c.harness->workspace(), "", {}, "buffers"}, "buffers (nvim)", "");
    return nvim_answer(L, [&](NvimHost& h) { return in_workspace(*c.harness, host_buffers(h)); });
}

void open_maid(lua_State* L, const Harness& harness, NvimHost* nvim) {
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_pushstring(L, harness.workspace().c_str());
    lua_setfield(L, -2, "workspace");
    for (auto [name, f] : {std::pair<const char*, lua_CFunction>{"read", l_read}, {"write", l_write}, {"list", l_list}, {"search", l_search},
                           {"shell", l_shell}, {"json_encode", l_json_encode}, {"json_decode", l_json_decode}}) {
        lua_pushcfunction(L, f);
        lua_setfield(L, -2, name);
    }
    if (nvim && nvim->connected()) {
        lua_newtable(L);
        lua_pushcfunction(L, l_nvim_diagnostics);
        lua_setfield(L, -2, "diagnostics");
        lua_pushcfunction(L, l_nvim_buffers);
        lua_setfield(L, -2, "buffers");
        lua_setfield(L, -2, "nvim");
    }
    lua_setglobal(L, "maid");
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// Reads one tool file into `out`. Throws with the reason to skip it.
LuaTool read_tool(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't read it");
    LuaTool tool;
    tool.file = file;
    tool.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    lua_State* L = sandboxed_state();
    struct Close {
        lua_State* L;
        ~Close() { lua_close(L); }
    } close{L};
    load_table(L, tool.source, file);
    nlohmann::json t = lua_to_json(L, -1);
    if (!t.contains("name") || !t["name"].is_string()) throw std::runtime_error("the table needs a string `name`");
    tool.name = t["name"];
    if (!valid_name(tool.name)) throw std::runtime_error("`name` must be lowercase letters, digits and underscores: " + tool.name);
    if (!canonical_tool_name(tool.name).empty()) throw std::runtime_error("`" + tool.name + "` is a built-in tool");
    if (!t.contains("description") || !t["description"].is_string()) throw std::runtime_error("the table needs a string `description`");
    tool.description = t["description"];
    lua_getfield(L, -1, "run");
    if (!lua_isfunction(L, -1)) throw std::runtime_error("the table needs a `run` function");
    lua_pop(L, 1);
    tool.parameters = t.contains("parameters") && t["parameters"].is_object() ? t["parameters"] : nlohmann::json::object();
    if (!tool.parameters.contains("type")) tool.parameters["type"] = "object";
    if (!tool.parameters.contains("properties")) tool.parameters["properties"] = nlohmann::json::object();
    // An empty Lua table reads as an object; `required` has to be a list.
    if (tool.parameters.contains("required") && tool.parameters["required"].is_object()) tool.parameters["required"] = nlohmann::json::array();
    return tool;
}

}  // namespace

fs::path global_tools_dir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "maid" / "tools";
    return home_dir() / ".config" / "maid" / "tools";
}

LuaToolSet load_lua_tools(const fs::path& workspace) {
    LuaToolSet set;
    for (const fs::path& dir : {workspace / ".maid" / "tools", global_tools_dir()}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        if (dir != global_tools_dir() && !trusted(workspace)) continue;  // an untrusted project's tools are never loaded
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".lua") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            try {
                LuaTool tool = read_tool(file);
                auto dup = std::find_if(set.tools.begin(), set.tools.end(), [&](const LuaTool& t) { return t.name == tool.name; });
                if (dup != set.tools.end()) throw std::runtime_error("`" + tool.name + "` is already defined by " + dup->file.string());
                set.tools.push_back(std::move(tool));
            } catch (const std::exception& e) {
                set.notices.push_back("tool skipped: " + file.string() + ": " + e.what());
            }
        }
    }
    return set;
}

ToolResult run_lua_tool(const LuaTool& tool, const nlohmann::json& args, const Harness& harness, const Authorise& authorise,
                        const std::atomic<bool>& cancel, std::chrono::seconds timeout, NvimHost* nvim, const OnOutput& on_output) {
    Ctx c{&tool, &harness, &authorise, &cancel, std::chrono::steady_clock::now() + timeout, "", nvim, &on_output};
    lua_State* L = sandboxed_state();
    struct Close {
        lua_State* L;
        ~Close() { lua_close(L); }
    } close{L};
    lua_pushlightuserdata(L, &c);
    lua_setfield(L, LUA_REGISTRYINDEX, kCtxKey);
    open_maid(L, harness, nvim);
    lua_sethook(L, hook, LUA_MASKCOUNT, kHookEvery);

    // What was printed comes before the returned value; after an error it follows the message, so the message
    // survives the cap.
    auto finish = [&](bool ok, std::string text) {
        if (!c.output.empty()) text = ok ? c.output + text : text + "\n" + c.output;
        if (text.size() > kMaxOutput) text = text.substr(0, kMaxOutput) + "\n[truncated at 64 KB]";
        if (text.empty()) text = "(no output)";
        return ToolResult{ok, text};
    };
    try {
        load_table(L, tool.source, tool.file);
    } catch (const std::exception& e) {
        return finish(false, e.what());
    }
    lua_getfield(L, -1, "run");
    json_to_lua(L, args.is_object() ? args : nlohmann::json::object());
    if (lua_pcall(L, 1, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        return finish(false, err);
    }
    std::string text;
    switch (lua_type(L, -1)) {
        case LUA_TNIL: break;
        case LUA_TTABLE: text = lua_to_json(L, -1).dump(2); break;
        default: {
            size_t len = 0;
            const char* s = lua_tolstring(L, -1, &len);
            text.assign(s ? s : "", s ? len : 0);
        }
    }
    return finish(true, text);
}

}  // namespace maid
