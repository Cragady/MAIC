#include "maic/theme.hpp"

#include "maic/lua.hpp"
#include "maic/paths.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

extern char** environ;

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool valid_theme_name(const std::string& name) {
    return !name.empty() && name[0] != '.' && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

// The colour forms a style accepts; the names are the ones the TUI's parse_color knows.
bool valid_color(const std::string& c) {
    static const std::set<std::string> names = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "gray", "gray_light", "gray_dark", "red_light",
                                                "green_light", "yellow_light", "blue_light", "magenta_light", "cyan_light", "default"};
    if (names.count(c)) return true;
    if (c.size() == 7 && c[0] == '#') return std::all_of(c.begin() + 1, c.end(), [](unsigned char x) { return std::isxdigit(x); });
    return !c.empty() && c.size() <= 3 && std::all_of(c.begin(), c.end(), [](unsigned char x) { return std::isdigit(x); }) && std::stoi(c) <= 255;
}

// The line of `source` where `key = ` first appears (as a bare key or ["key"]), 0 when it does not.
int line_of(const std::string& source, const std::string& key) {
    int line = 1;
    for (size_t pos = 0; pos < source.size();) {
        size_t nl = source.find('\n', pos);
        std::string l = source.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        for (size_t at = l.find(key); at != std::string::npos; at = l.find(key, at + 1)) {
            bool starts = at == 0 || !(std::isalnum(static_cast<unsigned char>(l[at - 1])) || l[at - 1] == '_');
            size_t after = at + key.size();
            while (after < l.size() && (l[after] == '"' || l[after] == '\'' || l[after] == ']' || l[after] == ' ')) ++after;
            if (starts && after < l.size() && l[after] == '=' && (after + 1 >= l.size() || l[after + 1] != '=')) return line;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++line;
    }
    return 0;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Lua turns an empty table into an empty array.
bool table_like(const json& j) {
    return j.is_object() || (j.is_array() && j.empty());
}

}  // namespace

fs::path user_themes_dir() {
    return settings_path().parent_path() / "themes";
}

fs::path builtin_themes_dir() {
    return root_dir() / "themes";
}

Theme parse_theme(const json& table, const std::string& where, const std::string& source) {
    auto fail = [&](const std::string& key, const std::string& what) {
        int line = key.empty() ? 0 : line_of(source, key);
        throw std::runtime_error(where + (line ? ":" + std::to_string(line) : "") + ": " + what);
    };
    if (!table.is_object()) fail("", "a theme returns a table { name = ..., background = ..., styles = { ... } }");
    Theme t;
    for (const auto& [k, v] : table.items()) {
        if (k == "name") {
            if (!v.is_string()) fail("name", "name must be a string");
            t.name = v.get<std::string>();
        } else if (k == "background") {
            if (!v.is_string() || (v != "dark" && v != "light")) fail("background", "background must be \"dark\" or \"light\"");
            t.background = v.get<std::string>();
        } else if (k == "styles") {
            if (!table_like(v)) fail("styles", "styles must be a table of roles");
        } else {
            fail(k, "unknown key " + k + " (a theme has name, background and styles)");
        }
    }
    if (!table.contains("styles") || !table["styles"].is_object()) return t;
    const auto& roles = default_styles();
    for (const auto& [role, sj] : table["styles"].items()) {
        if (!roles.count(role)) fail(role, "unknown role " + role + " (the roles are listed in docs/settings.md)");
        if (!table_like(sj)) fail(role, "styles." + role + " must be a table { fg = ..., bg = ..., bold = true, ... }");
        Style s;
        if (sj.is_object()) {
            for (const auto& [k, v] : sj.items()) {
                if (k == "fg" || k == "bg") {
                    if (!v.is_string() || !valid_color(v.get<std::string>())) fail(role, "styles." + role + "." + k + " must be \"#rrggbb\", a colour name or 0-255, not " + v.dump());
                    (k == "fg" ? s.fg : s.bg) = v.get<std::string>();
                } else if (k == "bold" || k == "dim" || k == "italic" || k == "underline" || k == "inverted") {
                    if (!v.is_boolean()) fail(role, "styles." + role + "." + k + " must be true or false");
                    bool b = v.get<bool>();
                    if (k == "bold") s.bold = b;
                    else if (k == "dim") s.dim = b;
                    else if (k == "italic") s.italic = b;
                    else if (k == "underline") s.underline = b;
                    else s.inverted = b;
                } else {
                    fail(role, "styles." + role + ": unknown attribute " + k + " (fg, bg, bold, dim, italic, underline, inverted)");
                }
            }
        }
        t.styles[role] = s;
    }
    return t;
}

Theme load_theme_file(const fs::path& path) {
    std::string source = read_text(path);
    json table;
    try {
        // Named by the file alone: Lua cuts a long chunk name to its last 60 characters, which loses the path.
        Lua lua(path.parent_path());
        table = lua.eval_table(source, "@" + path.filename().string());
    } catch (const std::exception& e) {
        std::string msg = e.what();
        if (msg[0] == '@') msg.erase(0, 1);
        throw std::runtime_error(msg.rfind(path.filename().string(), 0) == 0 ? (path.parent_path() / msg).string() : path.string() + ": " + msg);
    }
    Theme t = parse_theme(table, path.string(), source);
    t.name = path.stem().string();  // the file name is the theme's name; the name inside is for the reader
    t.path = path;
    return t;
}

Theme load_theme(const std::string& name) {
    if (!valid_theme_name(name)) throw std::runtime_error("a theme name is letters, digits, _ - and ., not \"" + name + "\"");
    std::error_code ec;
    if (fs::is_regular_file(user_themes_dir() / (name + ".lua"), ec)) return load_theme_file(user_themes_dir() / (name + ".lua"));
    // The shipped default.lua is a copy of the compiled-in table; with no file of the user's, the table is used.
    if (name == "default") return Theme{"default"};
    if (fs::is_regular_file(builtin_themes_dir() / (name + ".lua"), ec)) return load_theme_file(builtin_themes_dir() / (name + ".lua"));
    throw std::runtime_error("no theme " + name + " in " + user_themes_dir().string() + " or " + builtin_themes_dir().string() + " (:theme lists them)");
}

std::vector<ThemeInfo> list_themes() {
    std::map<std::string, fs::path> found;
    for (const auto& dir : {builtin_themes_dir(), user_themes_dir()}) {  // the user's last, so they shadow
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".lua" && valid_theme_name(it->path().stem().string())) found[it->path().stem().string()] = it->path();
        }
    }
    if (!fs::exists(user_themes_dir() / "default.lua")) found["default"] = fs::path();
    std::vector<ThemeInfo> out;
    for (const auto& [name, path] : found) out.push_back({name, path});
    return out;
}

void apply_theme(Settings& settings, const Theme& theme) {
    settings.styles = default_styles();
    for (const auto& [role, s] : theme.styles) settings.styles[role] = s;
    for (const auto& [role, s] : settings.style_overrides) settings.styles[role] = s.merged_over(settings.styles[role]);
    settings.theme = theme.name;
}

std::string theme_lua(const Theme& theme, const std::string& comment) {
    std::string out;
    size_t pos = 0;
    while (pos < comment.size()) {
        size_t nl = comment.find('\n', pos);
        out += "-- " + comment.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos) + "\n";
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    out += "return {\n  name = " + json(theme.name).dump() + ",\n  background = " + json(theme.background).dump() + ",\n  styles = {\n";
    for (const auto& [role, s] : theme.styles) {
        bool plain = std::all_of(role.begin(), role.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        std::vector<std::string> fields;
        if (s.fg) fields.push_back("fg = " + json(*s.fg).dump());
        if (s.bg) fields.push_back("bg = " + json(*s.bg).dump());
        if (s.bold) fields.push_back("bold = true");
        if (s.dim) fields.push_back("dim = true");
        if (s.italic) fields.push_back("italic = true");
        if (s.underline) fields.push_back("underline = true");
        if (s.inverted) fields.push_back("inverted = true");
        std::string body;
        for (const auto& f : fields) body += (body.empty() ? " " : ", ") + f;
        out += "    " + (plain ? role : "[" + json(role).dump() + "]") + " = {" + body + (body.empty() ? "}" : " }") + ",\n";
    }
    return out + "  },\n}\n";
}

ColorDepth color_depth(const std::string& setting) {
    if (setting == "truecolor") return ColorDepth::Truecolor;
    if (setting == "256") return ColorDepth::Xterm256;
    if (setting == "16") return ColorDepth::Ansi16;
    std::string colorterm = std::getenv("COLORTERM") ? std::getenv("COLORTERM") : "";
    std::string term = std::getenv("TERM") ? std::getenv("TERM") : "";
    if (colorterm == "truecolor" || colorterm == "24bit") return ColorDepth::Truecolor;
    if (term.find("256") != std::string::npos || colorterm.find("256") != std::string::npos) return ColorDepth::Xterm256;
    return ColorDepth::Ansi16;
}

namespace {

// xterm's defaults for the first 16; the rest are the cube and the grey ramp, which every xterm-like terminal shares.
const int kAnsi16[16][3] = {{0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},   {0, 0, 238},   {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
                            {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},   {92, 92, 255}, {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
const int kCube[6] = {0, 95, 135, 175, 215, 255};

void xterm_rgb(int i, int& r, int& g, int& b) {
    if (i < 16) {
        r = kAnsi16[i][0], g = kAnsi16[i][1], b = kAnsi16[i][2];
    } else if (i < 232) {
        r = kCube[(i - 16) / 36], g = kCube[(i - 16) / 6 % 6], b = kCube[(i - 16) % 6];
    } else {
        r = g = b = 8 + 10 * (i - 232);
    }
}

int nearest(int from, int to, uint8_t r, uint8_t g, uint8_t b) {
    int best = from;
    long best_d = -1;
    for (int i = from; i < to; ++i) {
        int cr, cg, cb;
        xterm_rgb(i, cr, cg, cb);
        long d = long(cr - r) * (cr - r) + long(cg - g) * (cg - g) + long(cb - b) * (cb - b);
        if (best_d < 0 || d < best_d) best = i, best_d = d;
    }
    return best;
}

}  // namespace

int nearest_xterm256(uint8_t r, uint8_t g, uint8_t b) {
    return nearest(16, 256, r, g, b);
}

int nearest_ansi16(uint8_t r, uint8_t g, uint8_t b) {
    return nearest(0, 16, r, g, b);
}

std::string xterm_hex(int index) {
    int r, g, b;
    xterm_rgb(std::clamp(index, 0, 255), r, g, b);
    char buf[8];
    snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

const std::vector<std::string>& nvim_theme_groups() {
    static const std::vector<std::string> groups = {
        "Normal", "NormalFloat", "Comment", "String", "Keyword", "Function", "Identifier", "Type", "Constant", "Title",
        "Error", "ErrorMsg", "WarningMsg", "MoreMsg", "Question", "Directory", "DiffAdd", "DiffDelete", "DiffChange", "DiffText",
        "Added", "Removed", "Changed", "Visual", "Search", "StatusLine", "StatusLineNC", "Pmenu", "PmenuSel", "CursorLine",
        "LineNr", "Underlined", "Todo", "Special", "@markup.heading", "@markup.raw", "@markup.link", "@markup.strong", "@markup.italic",
        "@markup.list", "@markup.quote"};
    return groups;
}

namespace {

// Where each role takes its colour from: the first group in the list that has one. A plain role takes the
// group's accent (its fg, else its bg) as fg and keeps the built-in role's attributes; a filled role (`fill`)
// takes the group's colours as nvim shows them (reverse applied), fg and bg. Roles not listed keep the default.
struct RoleSource {
    const char* role;
    std::vector<const char*> groups;
    bool fill = false;
};

const std::vector<RoleSource>& role_sources() {
    static const std::vector<RoleSource> table = {
        {"thinking", {"Comment"}},
        {"tool", {"Function", "Identifier"}},
        {"tool_ok", {"Comment"}},
        {"tool_err", {"ErrorMsg", "Error"}},
        {"notice", {"MoreMsg", "WarningMsg"}},
        {"error", {"ErrorMsg", "Error"}},
        {"shell", {"String"}},
        {"md_heading", {"@markup.heading", "Title"}},
        {"md_bold", {"@markup.strong"}},
        {"md_italic", {"@markup.italic"}},
        {"md_code", {"@markup.raw", "String"}},
        {"md_code_block", {"Pmenu", "NormalFloat"}, true},
        {"md_link", {"@markup.link", "Underlined", "Directory"}},
        {"md_url", {"Comment"}},
        {"md_quote", {"@markup.quote", "Comment"}},
        {"md_bullet", {"@markup.list", "Special"}},
        {"md_rule", {"LineNr", "Comment"}},
        {"hl_keyword", {"Keyword"}},
        {"hl_string", {"String"}},
        {"hl_comment", {"Comment"}},
        {"hl_heading", {"@markup.heading", "Title"}},
        {"hl_code", {"@markup.raw", "Constant"}},
        {"diff_added", {"Added", "DiffAdd"}},
        {"diff_removed", {"Removed", "DiffDelete"}},
        {"diff_hunk", {"Changed", "DiffText"}},
        {"input_prompt_insert", {"String"}},
        {"input_prompt_normal", {"Function", "Directory"}},
        {"separator", {"LineNr"}},
        {"focus", {"Special", "Function"}},
        {"visual", {"Visual"}, true},
        {"search", {"Search"}, true},
        {"cursor_line", {"CursorLine"}, true},
        {"status", {"StatusLine"}, true},
        {"status_insert", {"String"}},
        {"status_normal", {"Function", "Directory"}},
        {"status_visual", {"Constant", "Keyword"}},
        {"mode_manual", {"Function", "Directory"}},
        {"mode_auto-read", {"Special"}},
        {"mode_edit", {"Type", "WarningMsg"}},
        {"mode_auto", {"ErrorMsg", "Error"}},
        {"mode_plan", {"String"}},
        {"harness_armed", {"String"}},
        {"harness_tripped", {"ErrorMsg", "Error"}},
        {"remote", {"ErrorMsg", "Error"}},
        {"approval", {"Question", "MoreMsg"}},
    };
    return table;
}

std::optional<std::string> color_of(const json& g, const char* key) {
    if (g.contains(key) && g[key].is_string() && valid_color(g[key].get<std::string>())) return g[key].get<std::string>();
    return std::nullopt;
}

// A colorscheme that defines only cterm colours still gets a theme: its palette indexes become hex.
json gui_colors(json g) {
    if (!g.is_object() || g.contains("fg") || g.contains("bg")) return g;
    if (g.contains("ctermfg") && g["ctermfg"].is_number_integer()) g["fg"] = xterm_hex(g["ctermfg"].get<int>());
    if (g.contains("ctermbg") && g["ctermbg"].is_number_integer()) g["bg"] = xterm_hex(g["ctermbg"].get<int>());
    return g;
}

}  // namespace

Theme theme_from_nvim(const json& groups, const std::string& name, const std::string& background) {
    Theme t{name, background == "light" ? "light" : "dark"};
    if (!groups.is_object()) return t;
    for (const auto& src : role_sources()) {
        for (const char* gname : src.groups) {
            if (!groups.contains(gname)) continue;
            json g = gui_colors(groups[gname]);
            if (!g.is_object()) continue;
            auto fg = color_of(g, "fg"), bg = color_of(g, "bg");
            Style s;
            if (src.fill) {
                if (g.value("reverse", false)) std::swap(fg, bg);
                if (!fg && !bg) continue;
                s.fg = fg;
                s.bg = bg;
            } else {
                if (!fg && !bg) continue;
                s = default_styles().at(src.role);
                s.fg = fg ? fg : bg;
            }
            s.bold |= g.value("bold", false);
            s.italic |= g.value("italic", false);
            s.underline |= g.value("underline", false);
            t.styles[src.role] = s;
            break;
        }
    }
    return t;
}

namespace {

// Runs inside nvim after the user's configuration: applies the colorscheme and writes what MAIC needs as JSON
// to $MAIC_THEME_OUT, then quits. With no $MAIC_THEME_NAME it only lists the colorschemes. Groups come in as
// MAIC_THEME_GROUPS (JSON) so the list lives in one place.
const char* kImportLua = R"lua(
local out, name = os.getenv('MAIC_THEME_OUT'), os.getenv('MAIC_THEME_NAME') or ''
local result = {}
local ok, err = true, nil
if name ~= '' then
  vim.o.termguicolors = true
  ok, err = pcall(vim.cmd.colorscheme, name)
end
if name == '' or not ok then
  result.colors = vim.fn.getcompletion('', 'color')
  if not ok then result.error = tostring(err) end
else
  result.background = vim.o.background
  result.source = vim.api.nvim_get_runtime_file('colors/' .. name .. '.vim', false)[1] or vim.api.nvim_get_runtime_file('colors/' .. name .. '.lua', false)[1]
  result.groups = {}
  for _, g in ipairs(vim.json.decode(os.getenv('MAIC_THEME_GROUPS'))) do
    local entry = {}
    for k, v in pairs(vim.api.nvim_get_hl(0, { name = g, link = false })) do
      if k == 'fg' or k == 'bg' or k == 'sp' then entry[k] = string.format('#%06x', v)
      elseif type(v) == 'boolean' or type(v) == 'number' then entry[k] = v end
    end
    if next(entry) then result.groups[g] = entry end
  end
end
local f = assert(io.open(out, 'w'))
f:write(vim.json.encode(result))
f:close()
vim.cmd('qa!')
)lua";

// One headless nvim with the user's configuration, stdin from /dev/null so nothing waits on a prompt, in its
// own process group so a plugin manager's children go with it; killed at the timeout. Returns what it wrote.
json run_nvim(const std::string& nvim, const std::string& scheme, std::chrono::seconds timeout) {
    std::string tmpl = (fs::temp_directory_path() / "maic-theme-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) throw std::runtime_error("can't create a temporary directory for the nvim run");
    fs::path dir = tmpl;
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{dir};
    std::ofstream(dir / "import.lua") << kImportLua;
    fs::path out = dir / "out.json";

    // The environment is built before fork: the child of a threaded process may only exec.
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        if (kv.rfind("MAIC_THEME_", 0) != 0) env.push_back(kv);
    }
    env.push_back("MAIC_THEME_SCRIPT=" + (dir / "import.lua").string());
    env.push_back("MAIC_THEME_OUT=" + out.string());
    env.push_back("MAIC_THEME_NAME=" + scheme);
    env.push_back("MAIC_THEME_GROUPS=" + json(nvim_theme_groups()).dump());
    std::vector<char*> envp;
    for (auto& kv : env) envp.push_back(kv.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = {nvim, "--headless", "-i", "NONE", "-n", "--cmd", "let g:maic_theme_import = 1", "-c", "lua dofile(os.getenv('MAIC_THEME_SCRIPT'))"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    setpgid(pid, pid);
    int status = 0;
    bool done = false;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        done = waitpid(pid, &status, WNOHANG) == pid;
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-pid, SIGKILL);  // whatever it left behind, and nvim itself when it ran out of time
    if (!done) {
        waitpid(pid, &status, 0);
        throw std::runtime_error(nvim + " did not finish within " + std::to_string(timeout.count()) +
                                 " s and was stopped (a plugin manager installing in headless mode? g:maic_theme_import is set for a config that wants to skip plugins)");
    }
    std::error_code ec;
    if (!fs::is_regular_file(out, ec)) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) throw std::runtime_error("can't run " + nvim + " (is neovim installed and on PATH?)");
        throw std::runtime_error(nvim + " exited without an answer (status " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) + ")");
    }
    try {
        return json::parse(read_text(out));
    } catch (const json::exception& e) {
        throw std::runtime_error(nvim + " answered with something that is not JSON: " + e.what());
    }
}

std::string now_text() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

}  // namespace

std::vector<std::string> nvim_colorschemes(const std::string& nvim, std::chrono::seconds timeout) {
    json r = run_nvim(nvim, "", timeout);
    std::vector<std::string> out;
    for (const auto& c : r.value("colors", json::array())) {
        if (c.is_string()) out.push_back(c.get<std::string>());
    }
    return out;
}

Theme import_nvim_theme(const std::string& colorscheme, const std::string& as, const std::string& nvim, std::chrono::seconds timeout) {
    std::string name = as.empty() ? "nvim-" + colorscheme : as;
    if (!valid_theme_name(colorscheme)) throw std::runtime_error("a colorscheme name is letters, digits, _ - and ., not \"" + colorscheme + "\"");
    if (!valid_theme_name(name)) throw std::runtime_error("a theme name is letters, digits, _ - and ., not \"" + name + "\"");
    json r = run_nvim(nvim, colorscheme, timeout);
    if (r.contains("error")) throw std::runtime_error("nvim has no colorscheme " + colorscheme + "; :theme nvim: with Tab lists them");
    Theme t = theme_from_nvim(r.value("groups", json::object()), name, r.value("background", "dark"));
    std::string source = r.value("source", json()).is_string() ? r["source"].get<std::string>() : "";
    std::string comment = "Imported from the nvim colorscheme " + colorscheme + (source.empty() ? "" : " (" + source + ")") + " on " + now_text() +
                          ".\nRe-importing it overwrites this file; copy it under another name before editing.\n"
                          "Roles not listed keep MAIC's built-in default; the group each role comes from: docs/settings.md.";
    fs::create_directories(user_themes_dir());
    t.path = user_themes_dir() / (name + ".lua");
    std::ofstream(t.path) << theme_lua(t, comment);
    return t;
}

}  // namespace maic
