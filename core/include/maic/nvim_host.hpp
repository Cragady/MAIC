#pragma once

#include "maic/theme.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace maic {

// The nvim MAIC runs inside (maic.nvim, docs/nvim.md): what core asks of it. The CLI implements the transport
// (msgpack-rpc over the $NVIM socket, after the ancestry check); everything here is a Lua chunk run in the host.
class NvimHost {
public:
    virtual ~NvimHost() = default;
    virtual bool connected() const = 0;
    // nvim_exec_lua with `args` (an array) as `...`; returns what the chunk returns. Throws on an error in the
    // host, a timeout, or a lost connection.
    virtual nlohmann::json exec_lua(const std::string& code, const nlohmann::json& args) = 0;
    // The same as a notification: nothing waits, and an error comes back as nvim's error event.
    virtual void exec_lua_async(const std::string& code, const nlohmann::json& args) = 0;
};

// The host's listed file buffers: [{bufnr, path, modified, loaded}], `path` absolute.
nlohmann::json host_buffers(NvimHost& host);
// LSP diagnostics (vim.diagnostic.get) for the buffer showing `path`, or for every buffer when `path` is empty:
// [{path, line, col, severity, message, source}], line and col 1-based, severity "error", "warn", "info" or "hint".
nlohmann::json host_diagnostics(NvimHost& host, const std::filesystem::path& path);
// The window the user edits in (not MAIC's terminal): {path, cursor = {line, col}, selection = {from, to, text}
// or nothing}; the selection is the last visual one in that buffer.
nlohmann::json host_current(NvimHost& host);
// Opens `path` with :drop in the user's editing window (not inside MAIC's terminal window).
void host_open(NvimHost& host, const std::filesystem::path& path);
// A new tab with `path` against a read-only scratch buffer holding `proposed`, both in diff mode.
void host_diff(NvimHost& host, const std::filesystem::path& path, const std::string& proposed);
// Fires `User` autocmds with that pattern and `data` (MaicTurnStart, MaicToolCall, ...). Does not wait.
void host_fire(NvimHost& host, const std::string& pattern, const nlohmann::json& data);
// :checktime in the host, so a buffer of a file MAIC changed reloads. Does not wait.
void host_checktime(NvimHost& host);
// Makes the host send `maic_colorscheme` to `channel` (MAIC's own) after every ColorScheme.
void host_watch_colorscheme(NvimHost& host, int64_t channel);
// The host's current colorscheme as a theme named "nvim:<colors_name>", through theme_from_nvim.
Theme host_theme(NvimHost& host);

// Diagnostics as the model reads them: "path:line:col: severity: message" per line, paths relative to `base`
// when under it. Entries outside `base` are dropped when `only_under` is set (the workspace-wide call).
std::string format_diagnostics(const nlohmann::json& diagnostics, const std::filesystem::path& base, bool only_under);

// The `diagnostics` tool, offered only while a host is connected.
const nlohmann::json& diagnostics_tool_schema();

}  // namespace maic
