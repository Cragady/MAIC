#include "maic/nvim_host.hpp"

#include <stdexcept>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The window the user edits in: the current one unless it is a terminal (MAIC's own) or a float, then the
// previous window, then any plain window in the tab. nil when the tab has none.
const char* kEditingWin = R"lua(
local function editing_win()
  local function usable(w)
    return w ~= 0 and vim.api.nvim_win_is_valid(w) and vim.api.nvim_win_get_config(w).relative == ''
      and vim.bo[vim.api.nvim_win_get_buf(w)].buftype ~= 'terminal'
  end
  local cur = vim.api.nvim_get_current_win()
  if usable(cur) then return cur end
  local prev = vim.fn.win_getid(vim.fn.winnr('#'))
  if usable(prev) then return prev end
  for _, w in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if usable(w) then return w end
  end
end
)lua";

const char* kBuffers = R"lua(
local out = {}
for _, b in ipairs(vim.api.nvim_list_bufs()) do
  local name = vim.api.nvim_buf_get_name(b)
  if name ~= '' and vim.bo[b].buflisted and vim.bo[b].buftype == '' then
    out[#out + 1] = { bufnr = b, path = vim.fn.fnamemodify(name, ':p'), modified = vim.bo[b].modified, loaded = vim.api.nvim_buf_is_loaded(b) }
  end
end
return out
)lua";

const char* kDiagnostics = R"lua(
local want = ...
local uv = vim.uv or vim.loop
local function real(p) return uv.fs_realpath(p) or vim.fs.normalize(vim.fn.fnamemodify(p, ':p')) end
local list = {}
if want == '' then
  list = vim.diagnostic.get()
else
  local target = real(want)
  for _, b in ipairs(vim.api.nvim_list_bufs()) do
    local name = vim.api.nvim_buf_get_name(b)
    if name ~= '' and real(name) == target then vim.list_extend(list, vim.diagnostic.get(b)) end
  end
end
local names = { 'error', 'warn', 'info', 'hint' }
local out = {}
for _, d in ipairs(list) do
  local name = vim.api.nvim_buf_get_name(d.bufnr)
  if name ~= '' then
    out[#out + 1] = { path = real(name), line = d.lnum + 1, col = d.col + 1, severity = names[d.severity] or 'error', message = d.message, source = d.source or '' }
  end
end
table.sort(out, function(a, b)
  if a.path ~= b.path then return a.path < b.path end
  if a.line ~= b.line then return a.line < b.line end
  return a.col < b.col
end)
return out
)lua";

const char* kCurrent = R"lua(
local w = editing_win()
if not w then return vim.empty_dict() end
local b = vim.api.nvim_win_get_buf(w)
local cur = vim.api.nvim_win_get_cursor(w)
local out = { path = vim.api.nvim_buf_get_name(b), cursor = { line = cur[1], col = cur[2] + 1 } }
local s, e = vim.api.nvim_buf_get_mark(b, '<'), vim.api.nvim_buf_get_mark(b, '>')
if s[1] > 0 and e[1] >= s[1] then
  local lines = vim.api.nvim_buf_get_lines(b, s[1] - 1, e[1], false)
  out.selection = { from = { line = s[1], col = s[2] + 1 }, to = { line = e[1], col = math.min(e[2], #(lines[#lines] or '')) + 1 }, text = table.concat(lines, '\n') }
end
return out
)lua";

const char* kOpen = R"lua(
local path = ...
local w = editing_win()
if not w then
  for _, x in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
    if vim.api.nvim_win_get_config(x).relative == '' then vim.api.nvim_set_current_win(x) break end
  end
  vim.cmd('aboveleft vsplit')
  w = vim.api.nvim_get_current_win()
end
vim.api.nvim_set_current_win(w)
vim.api.nvim_exec2('drop ' .. vim.fn.fnameescape(path), {})
)lua";

const char* kDiff = R"lua(
local path, lines = ...
vim.cmd('tabnew')
vim.api.nvim_exec2('edit ' .. vim.fn.fnameescape(path), {})
vim.cmd('diffthis')
vim.cmd('rightbelow vnew')
local b = vim.api.nvim_get_current_buf()
vim.bo[b].buftype = 'nofile'
vim.bo[b].bufhidden = 'wipe'
vim.bo[b].swapfile = false
vim.api.nvim_buf_set_lines(b, 0, -1, false, lines)
pcall(vim.api.nvim_buf_set_name, b, 'maic://proposed/' .. path)
vim.bo[b].filetype = vim.filetype.match({ filename = path }) or ''
vim.bo[b].modifiable = false
vim.bo[b].readonly = true
vim.cmd('diffthis')
)lua";

const char* kFire = R"lua(
local pattern, data = ...
vim.api.nvim_exec_autocmds('User', { pattern = pattern, data = data, modeline = false })
)lua";

const char* kWatchColorscheme = R"lua(
local chan = ...
local group = vim.api.nvim_create_augroup('maic_theme_' .. chan, { clear = true })
vim.api.nvim_create_autocmd('ColorScheme', { group = group, callback = function()
  if not pcall(vim.rpcnotify, chan, 'maic_colorscheme') then return true end
end })
)lua";

const char* kTheme = R"lua(
local groups = ...
local out = { background = vim.o.background, name = vim.g.colors_name or 'default', groups = vim.empty_dict() }
for _, g in ipairs(groups) do
  local entry = {}
  for k, v in pairs(vim.api.nvim_get_hl(0, { name = g, link = false })) do
    if k == 'fg' or k == 'bg' or k == 'sp' then entry[k] = string.format('#%06x', v)
    elseif type(v) == 'boolean' or type(v) == 'number' then entry[k] = v end
  end
  if next(entry) then out.groups[g] = entry end
end
return out
)lua";

json as_array(json j) {
    return j.is_array() ? j : json::array();
}

}  // namespace

json host_buffers(NvimHost& host) {
    return as_array(host.exec_lua(kBuffers, json::array()));
}

json host_diagnostics(NvimHost& host, const fs::path& path) {
    return as_array(host.exec_lua(kDiagnostics, json::array({path.string()})));
}

json host_current(NvimHost& host) {
    json j = host.exec_lua(std::string(kEditingWin) + kCurrent, json::array());
    return j.is_object() ? j : json::object();
}

void host_open(NvimHost& host, const fs::path& path) {
    host.exec_lua(std::string(kEditingWin) + kOpen, json::array({path.string()}));
}

void host_diff(NvimHost& host, const fs::path& path, const std::string& proposed) {
    json lines = json::array();
    size_t start = 0;
    while (start < proposed.size()) {
        size_t nl = proposed.find('\n', start);
        if (nl == std::string::npos) nl = proposed.size();
        lines.push_back(proposed.substr(start, nl - start));
        start = nl + 1;
    }
    host.exec_lua(kDiff, json::array({path.string(), lines}));
}

void host_fire(NvimHost& host, const std::string& pattern, const json& data) {
    host.exec_lua_async(kFire, json::array({pattern, data}));
}

void host_checktime(NvimHost& host) {
    host.exec_lua_async("vim.cmd('silent! checktime')", json::array());
}

void host_watch_colorscheme(NvimHost& host, int64_t channel) {
    host.exec_lua(kWatchColorscheme, json::array({channel}));
}

Theme host_theme(NvimHost& host) {
    json r = host.exec_lua(kTheme, json::array({nvim_theme_groups()}));
    if (!r.is_object()) throw std::runtime_error("nvim did not return its highlight groups");
    std::string name = r.value("name", "default");
    return theme_from_nvim(r.value("groups", json::object()), "nvim:" + name, r.value("background", "dark"));
}

std::string format_diagnostics(const json& diagnostics, const fs::path& base, bool only_under) {
    std::string out;
    for (const auto& d : diagnostics) {
        if (!d.is_object()) continue;
        fs::path p = d.value("path", "");
        fs::path rel = p.lexically_relative(base);
        bool under = !rel.empty() && *rel.begin() != "..";
        if (only_under && !under) continue;
        std::string message = d.value("message", "");
        for (char& c : message) {
            if (c == '\n') c = ' ';
        }
        std::string source = d.value("source", "");
        out += (under ? rel : p).string() + ":" + std::to_string(d.value("line", 0)) + ":" + std::to_string(d.value("col", 0)) + ": " + d.value("severity", "error") + ": " +
               message + (source.empty() ? "" : " [" + source + "]") + "\n";
    }
    return out;
}

const json& diagnostics_tool_schema() {
    static const json schema = {
        {"type", "function"},
        {"function",
         {{"name", "diagnostics"},
          {"description",
           "LSP diagnostics (errors, warnings) from the user's nvim, which MAIC is running inside: for one file, or for every open "
           "file in the workspace when `path` is left out. One line each: `path:line:col: severity: message`. Only files open in nvim "
           "have diagnostics; an empty result means none were reported."},
          {"parameters", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}, {"description", "A file; leave out for the whole workspace"}}}}}}}}}};
    return schema;
}

}  // namespace maic
