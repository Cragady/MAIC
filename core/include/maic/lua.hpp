#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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
//
// LuaLibs::Restricted is the state a settings-like data file runs in below full trust (docs/settings.md): base,
// string, table, math and bit; os with only getenv, time, date and clock; load and loadstring for text chunks
// only; no io, package, require, dofile, loadfile, debug, collectgarbage, ffi, jit, newproxy, setfenv, getfenv
// or string.dump; `maic` holds only workspace, version, home and hostname. The JIT is off; a chunk that runs
// longer than 2 s, or whose heap passes its memory limit, is stopped with an error at its line; string.rep and
// table.concat refuse a result over the limit before making it. Reaching for a missing library is an error at
// its line, and the table it returns may hold only tables, strings, numbers and booleans.
enum class LuaLibs { Full, Restricted };

class Lua {
public:
    explicit Lua(std::filesystem::path workspace, std::function<void(const std::string&)> notice = {}, LuaLibs libs = LuaLibs::Full);
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
    // Restricted states: the heap a chunk may grow to (default 256 MB).
    void limit_memory(size_t mb);

    struct State;  // public for the C callbacks; not part of the interface

private:
    int load_chunk(const std::string& code, const std::string& chunk_name);  // luaL_loadbuffer, text only when restricted
    State* st_;
};

// How a settings-like Lua data file is evaluated (docs/harness.md, Settings Lua tiers):
//   Full        the whole standard library and the maic table, as the user: their own files by default
//   Sandbox     a forked child process (run_in_child) running the restricted state, its table sent back as JSON
//   Restricted  the restricted state in this process, with its in-process memory checks
enum class LuaTier { Full, Sandbox, Restricted };
std::optional<LuaTier> parse_lua_tier(const std::string& name);
const char* lua_tier_name(LuaTier tier);

// The tier and memory cap for the user's own data files (global settings.lua, themes, diction.lua): `global_lua`
// and `lua_memory_mb` in the global settings, set by load_settings.
struct LuaDataLimits {
    LuaTier tier = LuaTier::Full;
    size_t memory_mb = 256;
};
void set_lua_data_limits(LuaDataLimits limits);
LuaDataLimits lua_data_limits();

// Evaluates a chunk that returns a table (chunk_name "@path") at `tier` and returns the table as JSON. Throws
// with the file (and line) on any error, the limits included. When the sandbox can't fork it falls back to
// Restricted and says so in `warnings` (stderr when null).
nlohmann::json eval_lua_data(const std::string& code, const std::string& chunk_name, const std::filesystem::path& workspace, LuaTier tier,
                             size_t memory_mb, std::vector<std::string>* warnings = nullptr);
nlohmann::json eval_lua_data_file(const std::filesystem::path& path, const std::filesystem::path& workspace, LuaTier tier, size_t memory_mb,
                                  std::vector<std::string>* warnings = nullptr);

// The sandbox tier's process: `work` runs in a forked child whose address space may grow by `memory_mb`
// (RLIMIT_AS), with RLIMIT_CPU just past `seconds`, RLIMIT_NOFILE small, stdin, stdout and stderr on /dev/null
// and no other file descriptor than the result pipe; returns what `work` returned. Throws "WHAT exceeded its
// memory limit (N MB)", "WHAT exceeded its time limit (N s)", "WHAT crashed with signal N", or work's own
// error; ChildUnavailable when there is no child to run it in.
struct ChildUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};
std::string run_in_child(const std::function<std::string()>& work, const std::string& what, size_t memory_mb, int seconds);

// The host nvim for the user's own Lua (the session's settings files, :lua, :luafile): every state created while
// one is set gets `maic.nvim`. The sandboxed tool states never see it (they get their own read-only pair).
// nullptr clears it.
void set_lua_nvim_host(std::shared_ptr<NvimHost> host);

}  // namespace maic
