#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct lua_State;

namespace maic {

class NvimHost;

// LuaJIT, built from the pinned vendor/lua-pins tree, for things that are easier to script than to type:
// `:lua` in a session, `maic lua FILE`, and (later) tools written in Lua. This runs as the user, with the
// standard library, like a command in their shell; it is never given to the model.
//
// A `maic` table is preloaded:
//   maic.workspace           the workspace path
//   maic.version             the maic version string
//   maic.read(path)          file contents (relative to the workspace)
//   maic.write(path, text)   create or overwrite a file
//   maic.shell(cmd)          run cmd in the user's shell; returns output, exit code
//   maic.notice(text)        show a line in the conversation window (or print, headless)
//   maic.nvim                when MAIC runs inside a connected nvim (docs/nvim.md): exec(lua, ...), buffers(),
//                            diagnostics(path?), current(), all run in the host
class Lua {
public:
    explicit Lua(std::filesystem::path workspace, std::function<void(const std::string&)> notice = {});
    ~Lua();
    Lua(const Lua&) = delete;
    Lua& operator=(const Lua&) = delete;

    struct Result {
        bool ok;
        std::string output;  // everything print()ed, then the error message when !ok
    };
    Result run(const std::string& code, const std::string& chunk_name = "=input");
    // True when `code` is syntactically unfinished (a REPL should keep reading lines).
    bool incomplete(const std::string& code);
    // True when `code` parses (nothing is run).
    bool compiles(const std::string& code);
    Result run_file(const std::filesystem::path& path);

    // Runs a chunk that returns a table and converts it to JSON (settings.lua). Sequences (1..n) become arrays,
    // other tables objects; functions and userdata are dropped. Throws with the Lua error on failure.
    nlohmann::json eval_table(const std::string& code, const std::string& chunk_name = "=settings");
    nlohmann::json eval_table_file(const std::filesystem::path& path);

    struct State;  // public for the C callbacks; not part of the interface

private:
    State* st_;
};

// The host nvim for the user's own Lua (the session's settings files, :lua, :luafile): every state created while
// one is set gets `maic.nvim`. The sandboxed tool states never see it (they get their own read-only pair).
// nullptr clears it.
void set_lua_nvim_host(std::shared_ptr<NvimHost> host);

// A fresh state for configuration other programs read through maic (diction.lua): base, string, table, math, bit,
// and os with only getenv, time, date and clock. No io, package, require, dofile, loadfile, debug, ffi or jit;
// load and loadstring take text only; print goes to stderr; a run stops after a fixed instruction count. The caller
// lua_close()s it.
lua_State* make_restricted_lua_state();

// Evaluates a file returning a table in that state, as JSON; {} when the file does not exist. Throws with the Lua
// error (file:line: message).
nlohmann::json eval_restricted_table_file(const std::filesystem::path& path);

}  // namespace maic
