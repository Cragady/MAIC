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

}  // namespace

void set_lua_nvim_host(std::shared_ptr<NvimHost> host) {
    lua_nvim_host() = std::move(host);
}

Lua::Lua(fs::path workspace, std::function<void(const std::string&)> notice) : st_(new State) {
    st_->workspace = std::move(workspace);
    st_->notice = std::move(notice);
    lua_State* L = st_->L = luaL_newstate();
    luaL_openlibs(L);
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
    int rc = luaL_loadbuffer(L, code.data(), code.size(), chunk_name.c_str());
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

nlohmann::json Lua::eval_table(const std::string& code, const std::string& chunk_name) {
    lua_State* L = st_->L;
    int rc = luaL_loadbuffer(L, code.data(), code.size(), chunk_name.c_str());
    if (rc == 0) rc = lua_pcall(L, 0, 1, 0);
    if (rc != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error";
        lua_pop(L, 1);
        throw std::runtime_error(err);
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

namespace {

constexpr int kRestrictedInstructions = 10'000'000;

// Past the limit every further instruction fails too, so a pcall in the chunk cannot swallow it and carry on.
void restricted_hook(lua_State* L, lua_Debug*) {
    lua_sethook(L, restricted_hook, LUA_MASKCOUNT, 1);
    luaL_where(L, 0);
    lua_pushstring(L, "instruction limit reached (a loop that never ends?)");
    lua_concat(L, 2);
    lua_error(L);
}

// load and loadstring with the mode forced to text, so compiled bytecode never runs here.
int l_load_text(lua_State* L) {
    lua_settop(L, 4);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 4, LUA_MULTRET);
    return lua_gettop(L);
}

// stderr, so the stdout of whoever evaluates the file stays the JSON it reads.
int l_print_stderr(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) fprintf(stderr, "%s%s", i > 1 ? "\t" : "", tostr(L, i).c_str());
    fputc('\n', stderr);
    return 0;
}

}  // namespace

lua_State* make_restricted_lua_state() {
    lua_State* L = luaL_newstate();
    for (auto [name, open] : {std::pair<const char*, lua_CFunction>{"", luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
                              {LUA_MATHLIBNAME, luaopen_math}, {LUA_BITLIBNAME, luaopen_bit}, {LUA_OSLIBNAME, luaopen_os}}) {
        lua_pushcfunction(L, open);
        lua_pushstring(L, name);
        lua_call(L, 1, 0);
    }
    lua_newtable(L);
    lua_getglobal(L, "os");
    for (const char* f : {"getenv", "time", "date", "clock"}) {
        lua_getfield(L, -1, f);
        lua_setfield(L, -3, f);
    }
    lua_pop(L, 1);
    lua_setglobal(L, "os");
    for (const char* g : {"dofile", "loadfile", "require", "module"}) {
        lua_pushnil(L);
        lua_setglobal(L, g);
    }
    lua_getglobal(L, "load");
    for (const char* g : {"load", "loadstring"}) {
        lua_pushvalue(L, -1);
        lua_pushcclosure(L, l_load_text, 1);
        lua_setglobal(L, g);
    }
    lua_pop(L, 1);
    lua_pushcfunction(L, l_print_stderr);
    lua_setglobal(L, "print");
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);  // the count hook must see every loop
    lua_sethook(L, restricted_hook, LUA_MASKCOUNT, kRestrictedInstructions);
    return L;
}

nlohmann::json eval_restricted_table_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    lua_State* L = make_restricted_lua_state();
    struct Close {
        lua_State* L;
        ~Close() { lua_close(L); }
    } close{L};
    std::string chunk = "@" + path.string();
    if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != 0 || lua_pcall(L, 0, 1, 0) != 0)
        throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
    if (!lua_istable(L, -1)) throw std::runtime_error(path.string() + ": must return a table");
    return lua_to_json(L, -1);
}

Lua::Result Lua::run_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {false, "can't read " + path.string()};
    std::string code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (code.rfind("#!", 0) == 0) code = "--" + code;  // keep line numbers, skip the shebang
    return run(code, "@" + path.string());
}

}  // namespace maic
