#include "maic/nvim_setup.hpp"

#include "maic/lazy_lock.hpp"
#include "maic/paths.hpp"
#include "maic/tools.hpp"
#include "nvim_run.hpp"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* kFileName = "maic-llama-vim.lua";
const char* kMarker = "-- Written by MAIC (maic nvim setup llama-vim)";

// Asked of a headless nvim after the user's init has run: where its config and data are, whether lazy.nvim
// loads and was set up, the modules its spec imports (resolved to <config>/lua/...), and whether llama.vim is in
// the spec already (with the files that define its functions, when Lua can tell).
const char* kQuery = R"lua(
local r = { config = vim.fn.stdpath("config"), data = vim.fn.stdpath("data"), imports = {} }
local ok, err = pcall(function()
  r.lazy = pcall(require, "lazy")
  local has, cfg = pcall(require, "lazy.core.config")
  if not (r.lazy and has and type(cfg) == "table") then return end
  local opts = type(cfg.options) == "table" and cfg.options or {}
  r.setup = type(cfg.spec) == "table" or opts.spec ~= nil
  local function walk(spec, top)
    if type(spec) == "string" and top then
      table.insert(r.imports, spec)
    elseif type(spec) == "table" then
      if type(spec.import) == "string" and spec.enabled ~= false then table.insert(r.imports, spec.import) end
      for _, s in ipairs(spec) do walk(s, false) end
    end
  end
  walk(opts.spec, true)
  for i, mod in ipairs(r.imports) do
    local dir = r.config .. "/lua/" .. (mod:gsub("%.", "/"))
    r.imports[i] = { module = mod, dir = dir, exists = vim.fn.isdirectory(dir) == 1 }
  end
  local lists = { cfg.plugins, type(cfg.spec) == "table" and cfg.spec.disabled or nil }
  for _, list in pairs(lists) do
    for name, p in pairs(type(list) == "table" and list or {}) do
      local url = type(p) == "table" and (p.url or p[1]) or ""
      if name == "llama.vim" or (type(url) == "string" and url:find("/llama%.vim")) then
        local files = {}
        for _, key in ipairs({ "init", "config", "opts", "build", "keys", "cond" }) do
          local f = type(p) == "table" and rawget(p, key)
          local src = type(f) == "function" and debug.getinfo(f, "S").source or ""
          if src:sub(1, 1) == "@" then table.insert(files, vim.fn.fnamemodify(src:sub(2), ":p")) end
        end
        local frags = type(p._) == "table" and type(p._.frags) == "table" and #p._.frags or 1
        r.llama = { name = name, fragments = frags, files = files }
      end
    end
  end
end)
if not ok then r = { error = tostring(err) } end
local f = io.open(os.getenv("MAIC_LAZY_OUT"), "w")
f:write(vim.json.encode(r))
f:close()
vim.cmd("qa!")
)lua";

std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "";
}

std::string tilde(const fs::path& p) {
    std::string s = p.string(), h = home();
    if (!h.empty() && s.rfind(h + "/", 0) == 0) return "~" + s.substr(h.size());
    return s;
}

bool on_path(const std::string& program) {
    if (program.find('/') != std::string::npos) return access(program.c_str(), X_OK) == 0;
    std::istringstream dirs(std::getenv("PATH") ? std::getenv("PATH") : "");
    for (std::string dir; std::getline(dirs, dir, ':');) {
        if (!dir.empty() && access((fs::path(dir) / program).c_str(), X_OK) == 0) return true;
    }
    return false;
}

// lazy.nvim's own directory, as its bootstrap snippet puts it: stdpath("data")/lazy/lazy.nvim.
fs::path lazy_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* app = std::getenv("NVIM_APPNAME");
    fs::path data = xdg && *xdg ? fs::path(xdg) : fs::path(home()) / ".local" / "share";
    return data / (app && *app ? app : "nvim") / "lazy" / "lazy.nvim";
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool written_by_maic(const fs::path& p) {
    return read_all(p).rfind(kMarker, 0) == 0;
}

std::string without_first_line(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? "" : s.substr(nl + 1);
}

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string indent(const std::string& text) {
    std::string out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) out += (line.empty() ? "" : "    ") + line + "\n";
    return out;
}

std::string paste_by_hand() {
    return "\nThe spec from docs/models.md (Code completion), to add by hand:\n\n" + indent(llama_vim_spec());
}

// Lua and Vim files under the config directory (not hidden ones) that name llama.vim, apart from `skip`.
std::vector<std::string> files_naming_llama(const fs::path& config, const fs::path& skip) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(config, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind('.', 0) == 0) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        std::string ext = it->path().extension().string();
        if (!it->is_regular_file(ec) || (ext != ".lua" && ext != ".vim") || it->path() == skip) continue;
        if (read_all(it->path()).find("llama.vim") != std::string::npos) out.push_back(it->path().string());
    }
    return out;
}

LlamaVimPlan noop(const std::string& why, bool with_spec = true) {
    return {LlamaVimPlan::Kind::Noop, why + "\nNothing was written." + (with_spec ? paste_by_hand() : ""), {}, {}};
}

LlamaVimPlan plan_remove() {
    fs::path config = lazy_lock_path("").parent_path(), lua = config / "lua";
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(lua, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == kFileName) found.push_back(it->path());
    }
    if (found.empty()) return noop("There is no " + std::string(kFileName) + " under " + tilde(lua) + ", so there is nothing of MAIC's to remove.", false);
    if (found.size() > 1) {
        std::string list;
        for (const auto& f : found) list += "\n  " + tilde(f);
        return {LlamaVimPlan::Kind::Refuse, "There is more than one " + std::string(kFileName) + ":" + list + "\nMAIC writes one; remove the ones you do not want by hand. Nothing was removed.", {}, {}};
    }
    if (!written_by_maic(found[0])) {
        return {LlamaVimPlan::Kind::Refuse, tilde(found[0]) + " does not start with MAIC's header (" + kMarker + "), so MAIC did not write it and will not remove it. Nothing was removed.", {}, {}};
    }
    return {LlamaVimPlan::Kind::Remove, "Remove " + tilde(found[0]) + ", the llama.vim spec MAIC wrote. No other file is touched.", found[0], {}};
}

}  // namespace

std::string llama_vim_spec() {
    return "{\n"
           "    'ggml-org/llama.vim',\n"
           "    init = function()\n"
           "        vim.g.llama_config = {\n"
           "            endpoint_fim = 'http://127.0.0.1:8084/infill',\n"
           "            model_fim = 'current',  -- the coder `maic models install ID --link` chose\n"
           "            keymap_fim_trigger = '<M-f>',      -- off the leader in insert mode (below)\n"
           "            keymap_fim_accept_word = '<M-]>',\n"
           "            keymap_inst_accept = '',           -- completion only: leave normal mode alone (below)\n"
           "            keymap_inst_cancel = '',\n"
           "        }\n"
           "    end,\n"
           "}\n";
}

std::string llama_vim_file(const std::string& date) {
    std::string spec = llama_vim_spec();
    // "(below)" points into the docs page, which the header names.
    for (size_t at; (at = spec.find(" (below)")) != std::string::npos;) spec.erase(at, 8);
    return std::string(kMarker) + " on " + date + ".\n"
           "-- Undo with: maic nvim setup llama-vim --remove\n"
           "-- Why these settings: " + (root_dir() / "docs" / "models.md").string() + "#code-completion\n"
           "-- MAIC replaces this file when the setup runs again; keep changes of your own in another spec file.\n"
           "return " + spec;
}

LlamaVimPlan plan_llama_vim(bool remove, const std::string& config, const std::string& nvim) {
    if (remove) return plan_remove();
    if (!on_path(nvim)) return noop(nvim + " is not on PATH, so there is no lazy.nvim to add llama.vim to.");
    // Without lazy.nvim on disk the user's init is not run at all: a bootstrap snippet in it would clone lazy.nvim.
    bool installed = fs::is_directory(lazy_dir());
    if (!installed && config.empty()) {
        return noop("lazy.nvim is not installed: there is no " + tilde(lazy_dir()) + ". MAIC adds llama.vim only to a lazy.nvim setup, and no other plugin manager is touched.");
    }

    std::string tmpl = (fs::temp_directory_path() / "maic-lazy-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) return {LlamaVimPlan::Kind::Error, "can't create a temporary directory for the nvim run", {}, {}};
    fs::path dir = tmpl, script = dir / "query.lua", out = dir / "answer.json";
    std::ofstream(script) << kQuery;
    std::vector<std::string> args = {nvim, "--headless", "-i", "NONE", "-n"};
    if (!config.empty()) args.insert(args.end(), {"-u", config});
    args.insert(args.end(), {"-c", "lua dofile(os.getenv('MAIC_LAZY_SCRIPT'))"});
    json j;
    std::string failed;
    try {
        // GIT_ALLOW_PROTOCOL=file: a bootstrap or lazy.nvim's install of a missing plugin cannot clone while MAIC looks.
        NvimRun run = run_nvim_child(args, {"MAIC_LAZY_", "NVIM", "NVIM_LISTEN_ADDRESS"},
                                     {"MAIC_LAZY_SCRIPT=" + script.string(), "MAIC_LAZY_OUT=" + out.string(), "GIT_ALLOW_PROTOCOL=file"}, std::chrono::seconds(60));
        std::ifstream in(out);
        if (run.timed_out) failed = nvim + " did not finish within 60 s and was stopped";
        else if (!in) failed = nvim + " exited without an answer (status " + std::to_string(WIFEXITED(run.status) ? WEXITSTATUS(run.status) : 128 + WTERMSIG(run.status)) + ")";
        else j = json::parse(in, nullptr, false);
    } catch (const std::exception& e) {
        failed = e.what();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failed.empty() && (!j.is_object() || j.contains("error"))) failed = "the look inside nvim failed: " + (j.is_object() ? j.value("error", "?") : std::string("no JSON"));
    if (!failed.empty()) return {LlamaVimPlan::Kind::Error, "nvim setup: " + failed + ". Nothing was written.", {}, {}};

    fs::path nvim_config = j.value("config", "");
    if (!installed && !j.value("lazy", false)) {
        return noop("lazy.nvim is not installed: there is no " + tilde(fs::path(j.value("data", "")) / "lazy" / "lazy.nvim") + " and require(\"lazy\") fails. MAIC adds llama.vim only to a lazy.nvim setup, and no other plugin manager is touched.");
    }
    if (!j.value("setup", false)) {
        return noop("lazy.nvim is installed, but your nvim config does not call require(\"lazy\").setup(...), so lazy.nvim would not load a spec MAIC writes.");
    }

    fs::path import_dir;
    std::string module, seen;
    for (const auto& imp : j.value("imports", json::array())) {
        seen += std::string(seen.empty() ? "" : ", ") + "\"" + imp.value("module", "") + "\"";
        if (import_dir.empty() && imp.value("exists", false)) {
            import_dir = imp.value("dir", "");
            module = imp.value("module", "");
        }
    }
    if (import_dir.empty()) {
        fs::path suggested = nvim_config / "lua" / "plugins";
        return {LlamaVimPlan::Kind::Noop,
                "Your lazy.nvim spec imports no directory of your config" + (seen.empty() ? std::string() : " (it imports " + seen + ", none of them under " + tilde(nvim_config / "lua") + ")") +
                    ", so MAIC has nowhere to put a file of its own, and it does not edit your init files. Nothing was written.\n\n"
                    "To set it up by hand: add { import = \"plugins\" } to the spec you give require(\"lazy\").setup(...), then save this as " + tilde(suggested / "llama-vim.lua") + ":\n\n" +
                    indent("return " + llama_vim_spec()),
                {}, {}};
    }

    fs::path file = import_dir / kFileName;
    bool exists = fs::exists(file, ec), ours = exists && written_by_maic(file);
    if (exists && !ours) {
        return {LlamaVimPlan::Kind::Refuse, tilde(file) + " exists and MAIC did not write it (it does not start with " + kMarker + "). MAIC leaves it alone; nothing was written. Rename it to let MAIC write its own, or merge the spec into it by hand." + paste_by_hand(), {}, {}};
    }
    if (j.contains("llama") && j["llama"].is_object()) {
        const json& l = j["llama"];
        std::set<std::string> where;
        for (const auto& f : l.value("files", json::array())) {
            if (f.is_string() && fs::weakly_canonical(f.get<std::string>(), ec) != fs::weakly_canonical(file, ec)) where.insert(f.get<std::string>());
        }
        for (const auto& f : files_naming_llama(nvim_config, file)) where.insert(f);
        if (!ours || !where.empty() || l.value("fragments", 1) > 1) {
            std::string from;
            for (const auto& w : where) from += "\n  " + tilde(w);
            if (from.empty()) from = "\n  a spec MAIC cannot trace to a file in " + tilde(nvim_config) + " (a plugin's or a distribution's own spec, perhaps)";
            return {LlamaVimPlan::Kind::Refuse,
                    "llama.vim (\"" + l.value("name", "llama.vim") + "\") is already in your lazy.nvim spec, from:" + from +
                        "\nMAIC writes nothing, so lazy.nvim does not get a second spec to merge with yours. To use MAIC's settings, merge them into that spec by hand." + paste_by_hand(),
                    {}, {}};
        }
    }

    LlamaVimPlan p;
    p.kind = LlamaVimPlan::Kind::Write;
    p.file = file;
    p.update = ours;
    std::string before = ours ? read_all(file) : "";
    p.content = llama_vim_file(today());
    if (ours && without_first_line(before) == without_first_line(p.content)) {
        return {LlamaVimPlan::Kind::UpToDate, tilde(file) + " is MAIC's and already holds this spec; nothing to write.", file, {}};
    }
    std::string relies = "It relies on the import { import = \"" + module + "\" } in your lazy.nvim spec, which loads every file in " + tilde(import_dir) + ".";
    if (ours) p.text = "Update " + tilde(file) + ", which MAIC wrote:\n\n" + indent(change_lines(before, p.content, 60)) + "\n" + relies + " No other file is touched.";
    else p.text = "Write " + tilde(file) + " (a new file):\n\n" + indent(p.content) + "\n" + relies + " No other file is touched.";
    return p;
}

void apply_llama_vim(const LlamaVimPlan& plan) {
    if (plan.kind == LlamaVimPlan::Kind::Remove) {
        if (!written_by_maic(plan.file)) throw std::runtime_error(tilde(plan.file) + " no longer starts with MAIC's header; not removed");
        fs::remove(plan.file);
        return;
    }
    if (plan.kind != LlamaVimPlan::Kind::Write) return;
    // Written in place: a link into a dotfiles repository stays one.
    std::ofstream out(plan.file, std::ios::binary | std::ios::trunc);
    out << plan.content;
    if (!out.flush()) throw std::runtime_error("can't write " + tilde(plan.file));
}

}  // namespace maic
