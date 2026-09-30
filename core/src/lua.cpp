#include "maic/lua.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
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

}  // namespace

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
    nlohmann::json out = to_json(L, -1, 0);
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
