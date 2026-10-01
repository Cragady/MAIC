#include "maic/lua.hpp"

#include "lua_json.hpp"
#include "maic/nvim_host.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
#include <lualib.h>
}

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <sstream>

#ifndef MAIC_VERSION
#define MAIC_VERSION "dev"
#endif

namespace maic {

namespace fs = std::filesystem;

struct Lua::State {
    lua_State* L = nullptr;
    fs::path workspace;
    std::function<void(const std::string&)> notice;
    std::string output;  // print() collects here during a run
    std::shared_ptr<NvimHost> nvim;  // set_lua_nvim_host's, when one was set at creation
    bool restricted = false;
    std::chrono::steady_clock::time_point deadline;  // restricted: when the running chunk is stopped
};

namespace {

// Lua 5.1 has no lua_absindex.
int lua_absindex_compat(lua_State* L, int idx) {
    return idx > 0 || idx <= LUA_REGISTRYINDEX ? idx : lua_gettop(L) + idx + 1;
}

Lua::State& self(lua_State* L) {
    return *static_cast<Lua::State*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// tostring(v) through Lua itself (LuaJIT has the 5.1 API, without luaL_tolstring).
std::string tostr(lua_State* L, int i) {
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, i);
    lua_call(L, 1, 1);
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    std::string out(s ? s : "", s ? len : 0);
    lua_pop(L, 1);
    return out;
}

fs::path resolve(const Lua::State& s, const char* p) {
    fs::path path = p ? p : "";
    return path.is_absolute() ? path : s.workspace / path;
}

int l_print(lua_State* L) {
    auto& s = self(L);
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += '\t';
        line += tostr(L, i);
    }
    line += '\n';
    // Restricted (a project's settings file): kept out of the terminal.
    if (s.restricted) return s.output += line, 0;
    // With a notice sink (the TUI) output is collected for the caller; headless it goes straight out, in order
    // with io.write.
    if (s.notice) s.output += line;
    else fwrite(line.data(), 1, line.size(), stdout), fflush(stdout);
    return 0;
}

int l_read(lua_State* L) {
    auto& s = self(L);
    fs::path p = resolve(s, luaL_checkstring(L, 1));
    std::ifstream in(p, std::ios::binary);
    if (!in) return luaL_error(L, "can't read %s", p.c_str());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int l_write(lua_State* L) {
    auto& s = self(L);
    fs::path p = resolve(s, luaL_checkstring(L, 1));
    size_t len = 0;
    const char* text = luaL_checklstring(L, 2, &len);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text, static_cast<std::streamsize>(len));
    if (!out) return luaL_error(L, "can't write %s", p.c_str());
    return 0;
}

int l_shell(lua_State* L) {
    auto& s = self(L);
    std::string cmd = "cd " + std::string("'") + s.workspace.string() + "' && (" + luaL_checkstring(L, 1) + ") 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return luaL_error(L, "popen failed");
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    int status = pclose(p);
    lua_pushlstring(L, out.data(), out.size());
    lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 2;
}

int l_notice(lua_State* L) {
    auto& s = self(L);
    std::string text = luaL_checkstring(L, 1);
    if (s.notice) s.notice(text);
    else s.output += text + "\n";
    return 0;
}

std::shared_ptr<NvimHost>& lua_nvim_host() {
    static std::shared_ptr<NvimHost> host;
    return host;
}

// maic.nvim.*: the host's answer as a Lua value, an error raised as a Lua error.
int nvim_call(lua_State* L, const std::function<nlohmann::json(NvimHost&)>& call) {
    auto& s = self(L);
    std::string err;
    nlohmann::json result;
    if (!s.nvim || !s.nvim->connected()) err = "maic.nvim: the nvim host is gone";
    else {
        try {
            result = call(*s.nvim);
        } catch (const std::exception& e) {
            err = std::string("maic.nvim: ") + e.what();
        }
    }
    if (!err.empty()) return luaL_error(L, "%s", err.c_str());
    json_to_lua(L, result);
    return 1;
}

int l_nvim_exec(lua_State* L) {
    std::string code = luaL_checkstring(L, 1);
    nlohmann::json args = nlohmann::json::array();
    for (int i = 2; i <= lua_gettop(L); ++i) args.push_back(lua_to_json(L, i));
    return nvim_call(L, [&](NvimHost& h) { return h.exec_lua(code, args); });
}

int l_nvim_buffers(lua_State* L) {
    return nvim_call(L, [](NvimHost& h) { return host_buffers(h); });
}

int l_nvim_diagnostics(lua_State* L) {
    std::string path = lua_isnoneornil(L, 1) ? "" : resolve(self(L), luaL_checkstring(L, 1)).string();
    return nvim_call(L, [&](NvimHost& h) { return host_diagnostics(h, path); });
}

int l_nvim_current(lua_State* L) {
    return nvim_call(L, [](NvimHost& h) { return host_current(h); });
}

constexpr auto kRestrictedLimit = std::chrono::seconds(2);
const char* const kStateKey = "maic.lua";

void restricted_hook(lua_State* L, lua_Debug*) {
    lua_getfield(L, LUA_REGISTRYINDEX, kStateKey);
    auto* s = static_cast<Lua::State*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (std::chrono::steady_clock::now() < s->deadline) return;
    luaL_where(L, 0);
    lua_pushstring(L, "stopped: restricted settings Lua may run for 2 s (an endless loop?)");
    lua_concat(L, 2);
    lua_error(L);
}

// __index of _G and os in a restricted state: upvalue 1 is the prefix ("" or "os."), upvalue 2 the set of
// names that are an error (nil: every name).
int l_blocked(lua_State* L) {
    if (!lua_isnil(L, lua_upvalueindex(2))) {
        lua_pushvalue(L, 2);
        lua_rawget(L, lua_upvalueindex(2));
        if (!lua_toboolean(L, -1)) return lua_pushnil(L), 1;
    }
    const char* key = lua_tostring(L, 2);
    return luaL_error(L, "%s%s is not available in restricted settings Lua (a project's settings file; docs/settings.md)", lua_tostring(L, lua_upvalueindex(1)), key ? key : "?");
}

// load and loadstring for a restricted state: a text chunk in a string, compiled into the same globals.
int l_load_text(lua_State* L) {
    size_t len = 0;
    const char* code = luaL_checklstring(L, 1, &len);
    if (len > 0 && code[0] == LUA_SIGNATURE[0]) return luaL_error(L, "load: bytecode is not allowed in restricted settings Lua");
    if (luaL_loadbufferx(L, code, len, luaL_optstring(L, 2, "=(load)"), "t") != 0) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

void set_blocked_index(lua_State* L, const char* prefix, std::initializer_list<const char*> names) {
    lua_newtable(L);
    lua_pushstring(L, prefix);
    if (names.size() == 0) lua_pushnil(L);
    else {
        lua_newtable(L);
        for (const char* n : names) {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, n);
        }
    }
    lua_pushcclosure(L, l_blocked, 2);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
}

void open_restricted(lua_State* L, Lua::State* st) {
    for (auto [name, open] : {std::pair<const char*, lua_CFunction>{"", luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                              {LUA_MATHLIBNAME, luaopen_math}, {LUA_BITLIBNAME, luaopen_bit}, {LUA_OSLIBNAME, luaopen_os}}) {
        lua_pushcfunction(L, open);
        lua_pushstring(L, name);
        lua_call(L, 1, 0);
    }
    for (const char* g : {"dofile", "loadfile", "collectgarbage", "gcinfo", "newproxy", "setfenv", "getfenv"}) {
        lua_pushnil(L);
        lua_setglobal(L, g);
    }
    for (const char* g : {"load", "loadstring"}) {
        lua_pushcfunction(L, l_load_text);
        lua_setglobal(L, g);
    }
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    // os keeps the clock and the environment; any other os.* is an error.
    lua_getglobal(L, LUA_OSLIBNAME);
    lua_newtable(L);
    for (const char* f : {"getenv", "time", "date", "clock"}) {
        lua_getfield(L, -2, f);
        lua_setfield(L, -2, f);
    }
    set_blocked_index(L, "os.", {});
    lua_setglobal(L, LUA_OSLIBNAME);
    lua_pop(L, 1);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    set_blocked_index(L, "", {"io", "package", "require", "module", "dofile", "loadfile", "debug", "collectgarbage", "gcinfo", "ffi", "jit", "newproxy", "setfenv", "getfenv"});
    lua_pop(L, 1);
    lua_pushlightuserdata(L, st);
    lua_setfield(L, LUA_REGISTRYINDEX, kStateKey);
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);  // the count hook must see every loop
    lua_sethook(L, restricted_hook, LUA_MASKCOUNT, 1000);
}

}  // namespace

void set_lua_nvim_host(std::shared_ptr<NvimHost> host) {
    lua_nvim_host() = std::move(host);
}

Lua::Lua(fs::path workspace, std::function<void(const std::string&)> notice, LuaLibs libs) : st_(new State) {
    st_->workspace = std::move(workspace);
    st_->notice = std::move(notice);
    st_->restricted = libs == LuaLibs::Restricted;
    lua_State* L = st_->L = luaL_newstate();
    if (st_->restricted) open_restricted(L, st_);
    else luaL_openlibs(L);
    // print() goes to the caller, not stdout, so the TUI can show it.
    lua_pushlightuserdata(L, st_);
    lua_pushcclosure(L, l_print, 1);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_pushstring(L, st_->workspace.c_str());
    lua_setfield(L, -2, "workspace");
    lua_pushstring(L, MAIC_VERSION);
    lua_setfield(L, -2, "version");
    if (const char* home = std::getenv("HOME")) {
        lua_pushstring(L, home);
        lua_setfield(L, -2, "home");
    }
    {
        char host[256] = "";
        gethostname(host, sizeof(host) - 1);
        lua_pushstring(L, host);
        lua_setfield(L, -2, "hostname");
    }
    if (st_->restricted) {
        lua_setglobal(L, "maic");
        return;
    }
    for (auto [name, fn] : {std::pair<const char*, lua_CFunction>{"read", l_read}, {"write", l_write}, {"shell", l_shell}, {"notice", l_notice}}) {
        lua_pushlightuserdata(L, st_);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    st_->nvim = lua_nvim_host();
    if (st_->nvim && st_->nvim->connected()) {
        lua_newtable(L);
        for (auto [name, fn] : {std::pair<const char*, lua_CFunction>{"exec", l_nvim_exec}, {"buffers", l_nvim_buffers}, {"diagnostics", l_nvim_diagnostics}, {"current", l_nvim_current}}) {
            lua_pushlightuserdata(L, st_);
            lua_pushcclosure(L, fn, 1);
            lua_setfield(L, -2, name);
        }
        lua_setfield(L, -2, "nvim");
    }
    lua_setglobal(L, "maic");
}

Lua::~Lua() {
    if (st_->L) lua_close(st_->L);
    delete st_;
}

Lua::Result Lua::run(const std::string& code, const std::string& chunk_name) {
    lua_State* L = st_->L;
    st_->output.clear();
    int rc = load_chunk(code, chunk_name);
    if (rc == 0) rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_pop(L, 1);
        return {false, st_->output + err};
    }
    // Values a chunk returns are shown, like a REPL.
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        st_->output += tostr(L, i);
        st_->output += '\n';
    }
    lua_settop(L, 0);
    return {true, st_->output};
}

namespace {

nlohmann::json to_json(lua_State* L, int idx, int depth) {
    if (depth > 32) return nullptr;
    idx = lua_absindex_compat(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return nullptr;
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
        case LUA_TNUMBER: {
            double d = lua_tonumber(L, idx);
            if (d == static_cast<double>(static_cast<long long>(d))) return static_cast<long long>(d);
            return d;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            return std::string(s, len);
        }
        case LUA_TTABLE: {
            // A sequence 1..n with nothing else is an array.
            size_t n = lua_objlen(L, idx), keys = 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                ++keys;
                lua_pop(L, 1);
            }
            if (n > 0 && keys == n) {
                nlohmann::json arr = nlohmann::json::array();
                for (size_t i = 1; i <= n; ++i) {
                    lua_rawgeti(L, idx, static_cast<int>(i));
                    arr.push_back(to_json(L, -1, depth + 1));
                    lua_pop(L, 1);
                }
                return arr;
            }
            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                std::string key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : std::to_string(static_cast<long long>(lua_tonumber(L, -2)));
                nlohmann::json v = to_json(L, -1, depth + 1);
                if (!v.is_null() || lua_type(L, -1) == LUA_TNIL) obj[key] = v;
                lua_pop(L, 1);
            }
            return obj;
        }
        default: return nullptr;  // functions, userdata, threads
    }
}

}  // namespace

nlohmann::json lua_to_json(lua_State* L, int idx) {
    return to_json(L, idx, 0);
}

void json_to_lua(lua_State* L, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null: lua_pushnil(L); break;
        case nlohmann::json::value_t::boolean: lua_pushboolean(L, j.get<bool>()); break;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        case nlohmann::json::value_t::number_float: lua_pushnumber(L, j.get<double>()); break;
        case nlohmann::json::value_t::string: {
            const std::string& s = j.get_ref<const std::string&>();
            lua_pushlstring(L, s.data(), s.size());
            break;
        }
        case nlohmann::json::value_t::array:
            lua_createtable(L, static_cast<int>(j.size()), 0);
            for (size_t i = 0; i < j.size(); ++i) {
                json_to_lua(L, j[i]);
                lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
            break;
        case nlohmann::json::value_t::object:
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (const auto& [k, v] : j.items()) {
                json_to_lua(L, v);
                lua_setfield(L, -2, k.c_str());
            }
            break;
        default: lua_pushnil(L); break;
    }
}

int Lua::load_chunk(const std::string& code, const std::string& chunk_name) {
    if (!st_->restricted) return luaL_loadbuffer(st_->L, code.data(), code.size(), chunk_name.c_str());
    st_->deadline = std::chrono::steady_clock::now() + kRestrictedLimit;
    if (!code.empty() && code[0] == LUA_SIGNATURE[0]) {
        lua_pushstring(st_->L, ((chunk_name[0] == '@' ? chunk_name.substr(1) : chunk_name) + ": bytecode is not allowed in restricted settings Lua").c_str());
        return LUA_ERRSYNTAX;
    }
    return luaL_loadbufferx(st_->L, code.data(), code.size(), chunk_name.c_str(), "t");
}

namespace {

// Lua shortens a long file name in an error ("...ome/x/.maic/settings.lua:2: ..."); this puts the whole one back.
std::string full_chunk_name(std::string err, const std::string& chunk_name) {
    if (chunk_name.empty() || chunk_name[0] != '@' || err.rfind("...", 0) != 0) return err;
    std::string path = chunk_name.substr(1);
    size_t colon = err.find(':');
    while (colon != std::string::npos) {
        std::string shown = err.substr(3, colon - 3);
        if (path.size() >= shown.size() && path.compare(path.size() - shown.size(), shown.size(), shown) == 0) return path + err.substr(colon);
        colon = err.find(':', colon + 1);
    }
    return err;
}

}  // namespace

nlohmann::json Lua::eval_table(const std::string& code, const std::string& chunk_name) {
    lua_State* L = st_->L;
    int rc = load_chunk(code, chunk_name);
    if (rc == 0) rc = lua_pcall(L, 0, 1, 0);
    if (rc != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_pop(L, 1);
        throw std::runtime_error(full_chunk_name(err, chunk_name));
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        throw std::runtime_error(chunk_name + ": must return a table");
    }
    nlohmann::json out = lua_to_json(L, -1);
    lua_pop(L, 1);
    return out;
}

nlohmann::json Lua::eval_table_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return eval_table(code, "@" + path.string());
}

bool Lua::incomplete(const std::string& code) {
    lua_State* L = st_->L;
    int rc = luaL_loadbuffer(L, code.data(), code.size(), "=input");
    bool unfinished = false;
    if (rc == LUA_ERRSYNTAX) {
        size_t len = 0;
        const char* msg = lua_tolstring(L, -1, &len);
        std::string m(msg ? msg : "", len);
        unfinished = m.find("'<eof>'") != std::string::npos && m.find("near '<eof>'") == m.size() - 12;
    }
    lua_pop(L, 1);
    return unfinished;
}

bool Lua::compiles(const std::string& code) {
    lua_State* L = st_->L;
    bool ok = luaL_loadbuffer(L, code.data(), code.size(), "=input") == 0;
    lua_pop(L, 1);
    return ok;
}

Lua::Result Lua::run_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "can't read " + path.string()};
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (code.rfind("#!", 0) == 0) code = "--" + code;  // keep line numbers, skip the shebang
    return run(code, "@" + path.string());
}

}  // namespace maic
