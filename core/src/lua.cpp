#include "maic/lua.hpp"

#include "lua_json.hpp"
#include "maic/nvim_host.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
#include <lualib.h>
}

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
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
    size_t memory_limit = size_t(256) << 20;         // restricted: the heap it may grow to, in bytes
    std::string stopped;                             // restricted: why the running chunk was stopped, once it was
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

constexpr int kRestrictedSeconds = 2;
const char* const kStateKey = "maic.lua";

Lua::State& restricted_state(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kStateKey);
    auto* s = static_cast<Lua::State*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

std::string mb(size_t bytes) {
    return std::to_string(bytes >> 20) + " MB";
}

// Every 1000 instructions: the time limit, and the heap against the memory limit (an early refusal; in the
// sandbox tier the child's RLIMIT_AS is the real cap). Once either is passed the hook fires on every instruction,
// so a pcall around a loop can't swallow the stop: the next instruction outside it raises again.
void restricted_hook(lua_State* L, lua_Debug*) {
    auto& s = restricted_state(L);
    std::string& why = s.stopped;
    if (why.empty() && std::chrono::steady_clock::now() >= s.deadline) why = "stopped: restricted settings Lua may run for 2 s (an endless loop?)";
    else if (why.empty() && size_t(lua_gc(L, LUA_GCCOUNT, 0)) * 1024 > s.memory_limit) why = "stopped: settings Lua passed its memory limit (" + mb(s.memory_limit) + ")";
    if (why.empty()) return;
    lua_sethook(L, restricted_hook, LUA_MASKCOUNT, 1);
    luaL_where(L, 0);
    lua_pushstring(L, why.c_str());
    lua_concat(L, 2);
    lua_error(L);
}

// string.rep and table.concat make one string in one allocation, before the hook could see the heap grow:
// these check the size first. Upvalue 1 is the original function.
int refuse_big(lua_State* L, const char* what, double bytes) {
    auto& s = restricted_state(L);
    if (bytes + double(lua_gc(L, LUA_GCCOUNT, 0)) * 1024 <= double(s.memory_limit)) return 0;
    return luaL_error(L, "%s would make a %d MB string, over the memory limit (%s) of settings Lua", what, int(bytes / (1 << 20)), mb(s.memory_limit).c_str());
}

int call_original(lua_State* L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

int l_rep(lua_State* L) {
    size_t len = 0, sep = 0;
    luaL_checklstring(L, 1, &len);
    double n = luaL_checknumber(L, 2);
    if (!lua_isnoneornil(L, 3)) luaL_checklstring(L, 3, &sep);
    refuse_big(L, "string.rep", n > 0 ? double(len) * n + double(sep) * (n - 1) : 0);
    return call_original(L);
}

int l_concat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t sep = 0;
    if (!lua_isnoneornil(L, 2)) luaL_checklstring(L, 2, &sep);
    double first = luaL_optnumber(L, 3, 1), last = lua_isnoneornil(L, 4) ? double(lua_objlen(L, 1)) : luaL_checknumber(L, 4);
    double bytes = 0;
    for (double i = first; i <= last; ++i) {
        lua_rawgeti(L, 1, int(i));
        bytes += lua_type(L, -1) == LUA_TSTRING ? double(lua_objlen(L, -1)) : 24;  // a number: at most this long
        lua_pop(L, 1);
        if (i > first) bytes += double(sep);
    }
    refuse_big(L, "table.concat", bytes);
    return call_original(L);
}

void wrap(lua_State* L, const char* lib, const char* name, lua_CFunction fn) {
    lua_getglobal(L, lib);
    lua_getfield(L, -1, name);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
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
    return luaL_error(L, "%s%s is not available in restricted settings Lua (docs/settings.md)", lua_tostring(L, lua_upvalueindex(1)), key ? key : "?");
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
    // LuaJIT's string.format already refuses a width or precision over two digits.
    wrap(L, LUA_STRLIBNAME, "rep", l_rep);
    wrap(L, LUA_TABLIBNAME, "concat", l_concat);
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
    if (!L) {
        delete st_;
        throw std::bad_alloc();
    }
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
    st_->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRestrictedSeconds);
    st_->stopped.clear();
    lua_sethook(st_->L, restricted_hook, LUA_MASKCOUNT, 1000);
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

namespace {

// A data file's table, refusing what is not data: functions, userdata and threads name their key.
nlohmann::json data_json(lua_State* L, int idx, const std::string& key, int depth) {
    if (depth > 32) throw std::runtime_error("`" + key + "` nests deeper than 32 tables");
    idx = lua_absindex_compat(L, idx);
    int type = lua_type(L, idx);
    if (type != LUA_TTABLE) {
        if (type == LUA_TFUNCTION || type == LUA_TUSERDATA || type == LUA_TLIGHTUSERDATA || type == LUA_TTHREAD) {
            throw std::runtime_error("`" + key + "` is a " + lua_typename(L, type) + "; a settings file holds only tables, strings, numbers and booleans");
        }
        return lua_to_json(L, idx);
    }
    size_t n = lua_objlen(L, idx), keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++keys;
        lua_pop(L, 1);
    }
    bool array = n > 0 && keys == n;
    nlohmann::json out = array ? nlohmann::json::array() : nlohmann::json::object();
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        std::string k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : std::to_string(static_cast<long long>(lua_tonumber(L, -2)));
        nlohmann::json v = data_json(L, -1, key.empty() ? k : key + "." + k, depth + 1);
        if (array) out[size_t(lua_tonumber(L, -2)) - 1] = v;
        else out[k] = v;
        lua_pop(L, 1);
    }
    return out;
}

}  // namespace

nlohmann::json Lua::eval_table(const std::string& code, const std::string& chunk_name) {
    lua_State* L = st_->L;
    int rc = load_chunk(code, chunk_name);
    if (rc == 0) rc = lua_pcall(L, 0, 1, 0);
    std::string file = !chunk_name.empty() && chunk_name[0] == '@' ? chunk_name.substr(1) : chunk_name;
    if (rc == LUA_ERRMEM) {
        lua_settop(L, 0);
        throw std::runtime_error(file + " exceeded its memory limit (" + mb(st_->memory_limit) + ")");
    }
    if (rc != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_pop(L, 1);
        throw std::runtime_error(full_chunk_name(err, chunk_name));
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        throw std::runtime_error(file + ": must return a table");
    }
    nlohmann::json out;
    try {
        out = st_->restricted ? data_json(L, -1, "", 0) : lua_to_json(L, -1);
    } catch (const std::exception& e) {
        lua_settop(L, 0);
        throw std::runtime_error(file + ": " + e.what());
    }
    lua_pop(L, 1);
    return out;
}

void Lua::limit_memory(size_t megabytes) {
    st_->memory_limit = megabytes << 20;
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

std::optional<LuaTier> parse_lua_tier(const std::string& name) {
    if (name == "full") return LuaTier::Full;
    if (name == "sandbox") return LuaTier::Sandbox;
    if (name == "restricted") return LuaTier::Restricted;
    return std::nullopt;
}

const char* lua_tier_name(LuaTier tier) {
    switch (tier) {
        case LuaTier::Full: return "full";
        case LuaTier::Sandbox: return "sandbox";
        case LuaTier::Restricted: return "restricted";
    }
    return "";
}

namespace {

std::mutex g_limits_mu;
LuaDataLimits g_limits;

bool write_all(int fd, const std::string& data) {
    for (size_t done = 0; done < data.size();) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

// The child's side: only the result pipe stays open (as fd 3), stdio is /dev/null, then the limits.
[[noreturn]] void child_run(int out, const std::function<std::string()>& work, rlim_t address_space, int seconds) {
    if (out != 3) {
        dup2(out, 3);
        out = 3;
    }
    int null = open("/dev/null", O_RDWR);
    for (int fd = 0; fd < 3; ++fd) {
        if (null >= 0 && null != fd) dup2(null, fd);
    }
    if (syscall(SYS_close_range, 4U, ~0U, 0U) != 0) {
        for (int fd = 4; fd < 65536; ++fd) close(fd);
    }
    rlimit as{address_space, address_space}, cpu{rlim_t(seconds + 1), rlim_t(seconds + 2)}, files{8, 8};
    setrlimit(RLIMIT_AS, &as);
    setrlimit(RLIMIT_CPU, &cpu);
    setrlimit(RLIMIT_NOFILE, &files);
    std::string message;
    try {
        message = "O" + work();
    } catch (const std::bad_alloc&) {
        message = "M";
    } catch (const std::exception& e) {
        message = std::string("E") + e.what();
    }
    write_all(out, message);
    _exit(0);
}

}  // namespace

void set_lua_data_limits(LuaDataLimits limits) {
    std::lock_guard lock(g_limits_mu);
    g_limits = limits;
}

LuaDataLimits lua_data_limits() {
    std::lock_guard lock(g_limits_mu);
    return g_limits;
}

std::string run_in_child(const std::function<std::string()>& work, const std::string& what, size_t memory_mb, int seconds) {
    // The child starts as a copy of this process, so its cap is what it already maps plus memory_mb.
    size_t pages = 0;
    std::ifstream("/proc/self/statm") >> pages;
    rlim_t address_space = rlim_t(pages) * rlim_t(sysconf(_SC_PAGESIZE)) + (rlim_t(memory_mb) << 20);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) throw ChildUnavailable(std::string("no pipe: ") + std::strerror(errno));
    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        throw ChildUnavailable(std::string("fork failed: ") + std::strerror(err));
    }
    if (pid == 0) child_run(fds[1], work, address_space, seconds);
    close(fds[1]);
    std::string data;
    bool late = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 3);
    for (;;) {
        int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            late = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        char buf[65536];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        data.append(buf, size_t(n));
    }
    close(fds[0]);
    if (late) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::string limit_mb = " exceeded its memory limit (" + std::to_string(memory_mb) + " MB)";
    if (late || (WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU)) throw std::runtime_error(what + " exceeded its time limit (" + std::to_string(seconds) + " s)");
    if (WIFSIGNALED(status)) throw std::runtime_error(what + " crashed with signal " + std::to_string(WTERMSIG(status)));
    if (data.empty()) throw std::runtime_error(what + " crashed (its evaluation ended without a result)");
    if (data[0] == 'M') throw std::runtime_error(what + limit_mb);
    if (data[0] == 'E') throw std::runtime_error(data.substr(1));
    return data.substr(1);
}

nlohmann::json eval_lua_data(const std::string& code, const std::string& chunk_name, const fs::path& workspace, LuaTier tier, size_t memory_mb,
                             std::vector<std::string>* warnings) {
    if (tier == LuaTier::Full) return Lua(workspace).eval_table(code, chunk_name);
    auto restricted = [&] {
        Lua lua(workspace, {}, LuaLibs::Restricted);
        lua.limit_memory(memory_mb);
        return lua.eval_table(code, chunk_name);
    };
    if (tier == LuaTier::Restricted) return restricted();
    std::string what = !chunk_name.empty() && chunk_name[0] == '@' ? chunk_name.substr(1) : chunk_name;
    try {
        std::string out = run_in_child([&] { return restricted().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }, what, memory_mb, kRestrictedSeconds);
        return nlohmann::json::parse(out);
    } catch (const ChildUnavailable& e) {
        std::string note = what + ": the sandbox could not start (" + e.what() + "); evaluated restricted in this process instead";
        if (warnings) warnings->push_back(note);
        else fprintf(stderr, "maic: %s\n", note.c_str());
        return restricted();
    }
}

nlohmann::json eval_lua_data_file(const fs::path& path, const fs::path& workspace, LuaTier tier, size_t memory_mb, std::vector<std::string>* warnings) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return eval_lua_data(code, "@" + path.string(), workspace, tier, memory_mb, warnings);
}

}  // namespace maic
