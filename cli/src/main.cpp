#include "commands.hpp"
#include "doctor.hpp"
#include "headless.hpp"
#include "setup.hpp"
#include "maic/artifacts.hpp"
#include "maic/harness.hpp"
#include "maic/import.hpp"
#include "maic/lazy_lock.hpp"
#include "maic/nvim_keymaps.hpp"
#include "maic/nvim_setup.hpp"
#include "maic/paths.hpp"
#include "maic/redact.hpp"
#include "maic/service.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/theme.hpp"
#include "maic/status.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"
#include "maic/bans.hpp"
#include "maic/clipboard.hpp"
#include "maic/lua.hpp"
#include "maic/places.hpp"
#include "maic/lua_tools.hpp"
#include "maic/models.hpp"
#include "maic/script_tools.hpp"
#include "maic/vendor.hpp"
#include "server.hpp"
#include "tui.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

void usage(std::ostream& out = std::cerr) {
    out << "usage: maic [--model M] [--mode MODE]      the agent, in this directory\n"
                 "       maic -c                            continue the last session started in this directory\n"
                 "       maic -r [ID|PATH]                  resume a session by id, or by the path of any transcript file (a temporary\n"
                 "                                          one from --no-record too); no argument: pick from a list\n"
                 "       maic -p \"prompt\" [--json] [--think] one turn without the UI (prompt \"-\" reads stdin; -c/-r work here too)\n"
                 "       maic -p \"prompt\" --interactive     an interactive session that opens with that prompt sent (-i)\n"
                 "       --context FILE, -C FILE            attach a text file to the conversation before the prompt; repeatable;\n"
                 "                                          FILE \"-\" reads stdin (then the prompt can't also be stdin)\n"
                 "       --image FILE, -I                   a picture sent with the first (or only) prompt; repeatable; the model must\n"
                 "                                          be a vision one (both Qwen3.5 GGUFs here are)\n"
                 "       --system TEXT|@FILE, -S            operator instructions placed first in the system prompt (front-loads behaviour)\n"
                 "       --no-instructions                  load no instruction file anywhere; combines with --system\n"
                 "       --ctx N                            context window in tokens: starts (or restarts) the local llama.cpp server\n"
                 "                                          with --ctx-size N and sizes the readout; also `maic up llamacpp --ctx N`\n"
                 "       --ctx2 N                           the same for the side server llamacpp-2 (port 8082; default 8192)\n"
                 "       --prefix TEXT|@FILE                every reply starts with these literal words (also --prefill)\n"
                 "       --rule TEXT                        a standing instruction the model is reminded of every turn (repeatable)\n"
                 "       --ban TEXT|@FILE                   a phrase the model must not say, or a file with one per line (repeatable; :ban)\n"
                 "       --ban-pattern REGEX|@FILE          a POSIX extended regex the reply must not match, or a file of them (maic help bans)\n"
                 "       --xtc P[,T]                        exclude top choices: probability and threshold (0.5,0.1); llama.cpp-style\n"
                 "                                          servers only (maic help sampling)\n"
                 "       --sampling KEY=VALUE               any sampler key for this run (temperature=0.7, min_p=0.05, seed=7); repeatable\n"
                 "       --harness smart|dumb               smart (default): a model reviews commands and writes the rules would let\n"
                 "                                          through without asking; dumb: the rule list alone (maic help harness)\n"
                 "       --accept-dumb-auto                 skip the warning when combining --harness dumb with --mode auto\n"
                 "       --bare                             nothing from nvim: no $NVIM host, no nvim highlighter or theme, no lazy-lock\n"
                 "                                          notice or keymap check (also MAIC_BARE=1, bare = true; maic help bare)\n"
                 "       --record / --no-record             keep a transcript or not (interactive: yes by default, or \"record\" in\n"
                 "                                          settings; -p: none by default)\n"
                 "       --append / --no-append             with -c/-r: write into the old session file, or into a new one that\n"
                 "                                          points at it (default: interactive appends; -p records nothing\n"
                 "                                          unless --record, which forks, or --append)\n"
                 "       --fork-at N                        with -c/-r: continue from the old session's first N records only, in a\n"
                 "                                          new file that points at them (the old file is never changed)\n"
                 "       --trust[=sandbox]                  trust this directory's project files for this run only, fully or with\n"
                 "                                          their Lua in the sandbox (headless runs and runs off a terminal use\n"
                 "                                          untrusted ones otherwise; maic help trust)\n"
                 "\n"
                 "  vendor                     the services MAIC can install for itself (ComfyUI, llama.cpp), pinned versions\n"
                 "  vendor add NAME            fetch, verify, build and link one (network; asks nothing else)\n"
                 "  vendor adopt NAME PATH     use an install you already have instead of fetching\n"
                 "  vendor use llamacpp PATH   the GGUF that llamacpp/current means (a file under the models directory)\n"
                 "  vendor use whisper FILE    the ggml model whisper-server loads (<models_dir>/whisper/current.bin)\n"
                 "  vendor model llamacpp|whisper URL SHA256 [--into DIR]   download a model, verify it, link it as current\n"
                 "  vendor wire NAME           redo the links and, for comfyui, the maic: block of extra_model_paths.yaml\n"
                 "                             from models_dir (no network; add and adopt do this too)\n"
                 "  vendor unlink NAME         stop using it (nothing is deleted)\n"
                 "  model resolve NAME         a preset name or provider/model as JSON: provider, kind, base_url, model, context\n"
                 "  models                     the model catalog: id, role, size, installed, current, presets (maic help models)\n"
                 "  models info ID             what it is good and bad at, its license, files, hashes and VRAM estimate\n"
                 "  models install ID [--link] download its files checked by SHA-256 (present ones are kept), --link: make it\n"
                 "                             the current model of its server (llamacpp, whisper, or llamacpp-fim)\n"
                 "  models verify ID | remove ID [--yes] | check   hash what is there; delete it (never shared weights);\n"
                 "                             validate the catalog offline\n"
                 "  diction [ARGS...]          narrate out loud into a markdown document: mic, whisper-server, a local scribe\n"
                 "                             (maic help diction is its own --help; docs/diction.md)\n"
                 "  lua [FILE [args...] | -e CODE]   Lua (vendored LuaJIT) here, with the maic table; no arguments: a REPL (maic help lua)\n"
                 "  tools                      every tool the model can call: built-ins, the helpers beside maic, this\n"
                 "                             directory's Lua and script tools with their language and declared reads/writes\n"
                 "  tools check                validate every tool manifest here and in ~/.config/maic/tools (exit 1 on a problem)\n"
                 "  tools new NAME --lang python|sh|perl|node [--global]   scaffold .maic/tools/NAME/ with a manifest and a stub\n"
                 "  themes                     the themes there are (yours in ~/.config/maic/themes, then the shipped ones), the\n"
                 "                             active one marked, with where each comes from (maic help theme)\n"
                 "  themes import NAME [--as FILE]   a neovim colorscheme as a theme file, from a headless nvim with your config\n"
                 "  doctor                     what this machine has, what MAIC needs, a recommended setup\n"
                 "  nvim keymaps [--all] [-u FILE]   maic.nvim's keymap check (:checkhealth maic) in a headless nvim with your\n"
                 "                             config: the keys maic.nvim, MAIC's terminal input and llama.vim need against\n"
                 "                             your mappings; exit 0 none collide, 1 collisions, 2 nvim could not run\n"
                 "  nvim setup llama-vim [--dry-run] [--remove] [--yes]   llama.vim's spec (docs/models.md) as one file MAIC\n"
                 "                             owns, maic-llama-vim.lua in the directory your lazy.nvim spec imports; asks\n"
                 "                             first, writes nothing else; without lazy.nvim or an import directory it explains\n"
                 "                             and stops (exit 0); exit 1 refused (a file or spec of yours in the way)\n"
                 "  lazy-lock [record|diff]    is nvim's lazy-lock.json as recorded? record its hash (a file to commit with your\n"
                 "                             dotfiles), or list what changed per plugin; exit 0 in sync, 1 changed, 2 no file\n"
                 "  setup                      a guided first run: prerequisites, settings, llama.cpp, ComfyUI, a model, the\n"
                 "                             tripwire; every step is a yes/no question, nothing runs without a yes\n"
                 "  status                     harness, services (host process or docker container), what each holds, quick actions\n"
                 "  up <service...|all>        start services\n"
                 "  down <service...|all>      stop services MAIC started\n"
                 "  logs <service> [lines]     the end of a service's log (default 40 lines; docker logs for a container)\n"
                 "  gpu [free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui]]   who holds the card (each llama server's resident model,\n"
                 "                             whisper's, ComfyUI's VRAM) and whether they fit; free unloads models without stopping anything\n"
                 "  gpu load llamacpp-fim      load the completion server's coder (it never loads by itself; docs/models.md)\n"
                 "  path [NAME] [--copy]       every place maic knows (workspace, sessions, models, workflows, ...) or one path;\n"
                 "                             --copy puts it on the clipboard; a unique prefix is enough\n"
                 "  cd NAME [--subshell]       print a place's directory (cd \"$(maic cd NAME)\"; a file's parent); --subshell (-s)\n"
                 "                             opens a shell there instead, exit returns; mcd from shell-init changes this shell\n"
                 "  open [NAME|SERVICE] [--browser default|firefox|chrome] [--folder]\n"
                 "                             --folder (or --file-manager, -f) opens the containing folder instead\n"
                 "                             a service (comfyui, llamacpp, server) in the browser, a place in the file manager; no\n"
                 "                             name: a menu. With `remote` in settings and that maic-server up, its copy of the service\n"
                 "  shell-init [zsh|bash|fish] shell functions: mcd NAME (cd there), mpath NAME, mcp NAME; eval \"$(maic shell-init)\"\n"
                 "  artifacts                  where MAIC and its services keep transcripts, logs and outputs\n"
                 "  artifacts clean OWNER/NAME [--older-than DAYS] [--yes]\n"
                 "  sessions                   list session transcripts (where started, where last opened)\n"
                 "  sessions rehome ID [ID...] [project|general|NAME] [--subagent | --subagent-only] [--children-of ID] [-n]\n"
                 "                             move transcripts to another home (default: project); their subagent sessions\n"
                 "                             stay unless --subagent (them too) or --subagent-only (only them); -n: the plan\n"
                 "  sessions path ID           print a transcript's path\n"
                 "  sessions export ID [FILE]  the transcript as markdown (stdout without FILE)\n"
                 "  sessions import FILE [--as claude-ai|claude-code|auto] [--home general|project|NAME] [--conversation UUID]\n"
                 "                             a claude.ai export or a Claude Code transcript as a new session (prints its id)\n"
                 "  sessions redact ID|FILE [--in-place | -o FILE]   a copy with credential material replaced by [REDACTED:kind]\n"
                 "                             (default: ./<id>.redacted.jsonl, outside the sessions tree)\n"
                 "  sessions read ID [--range A-B] [--tools]   the conversation as plain text (user turns A to B), for piping\n"
                 "  sessions state ID          one screen: turns, tool calls per tool, files touched, tokens and budget,\n"
                 "                             compactions, forks and subagents\n"
                 "  sessions time ID [--slowest N]   how long each turn took, and the slowest tool calls\n"
                 "  sessions name ID [--model M]   title it with the small model (small_model in settings), as the auto-title does\n"
                 "  sessions inject ID (--text T | --file F|-) [--at N] [--role user|system] [--home general|project|NAME]\n"
                 "                             a new session: ID's first N records (a pointer) plus one note, marked as injected\n"
                 "  sessions graft ID --onto TARGET [--at N] [--home ...]   a new session: TARGET's first N records (a pointer),\n"
                 "                             then ID's conversation copied in after a note saying where it came from\n"
                 "  sessions compose ID --from N [--root FILE|-] [--home ...]   a new session: ID's records from N on, copied,\n"
                 "                             after an optional root text and a note that the earlier part is missing\n"
                 "                             (none of these three changes an existing transcript)\n"
                 "  cai [TOOL [args...]]       cai-tools (docs/cai.md): the dispatcher's listing, or one of its tools with the\n"
                 "                             arguments untouched; the same as `cai TOOL ...` (and `maic-cai`) on PATH\n"
                 "  trans-fairy [args...]      maic cai trans-fairy ...: cut, compose, graft and install transcripts\n"
                 "  trans-fairy-write [args...]   maic cai trans-fairy-write ...: overwrite one, with a backup first\n"
                 "                             (both take a MAIC session or a Claude Code transcript, told apart by content)\n"
                 "  settings init [--json]|path  write the global settings file (Lua; --json for JSON), or show where it goes\n"
                 "  settings read diction      diction.lua beside settings.lua, evaluated in a restricted Lua state, as JSON\n"
                 "                             ({} when it does not exist); diction reads its config through this\n"
                 "  init                       scaffold this project: MAIC.md and .maic/settings.lua (transcripts then\n"
                 "                             go under sessions/projects/); :init in a session also drafts the MAIC.md\n"
                 "  server start [--listen ADDR:PORT] [--model M] [--mode MODE]   the remote-access server and its web client\n"
                 "  server token new|list|revoke [NAME]   per-device bearer tokens for it\n"
                 "  server pair | pairs | unpair NAME     a phone's pairing for the relay (server.relay in settings)\n"
                 "  server status              its configuration, the relay link, and whether it is up (maic help server)\n"
                 "  trust [PATH] [--lua full|sandbox|restricted] [--level strict|standard|relaxed]   trust a project directory\n"
                 "                             (default: every untrusted one on the chain down to here): how its settings Lua\n"
                 "                             runs (default full) and how often to ask again; trust --list shows what is\n"
                 "                             remembered; untrust [PATH] forgets it (maic help trust)\n"
                 "  trip [reason]              trip the harness lock now (blocks all actions until unlocked)\n"
                 "  unlock [machine|session ID|all-sessions|all]\n"
                 "                             what is locked, with a menu: the machine lock (sudo) and each session's own lock\n"
                 "                             (removed without sudo; the session shown with its task, dir, model, running or not)\n"
                 "\n"
                 "  help [TOPIC]               this text, or one page: maic help help lists the topics; help headless,\n"
                 "                             sessions, modes, keys, vendor, lua, settings, ... (the same pages as :h);\n"
                 "                             help cai [TOOL], help trans-fairy and help trans-fairy-write print cai's help\n"
                 "\n"
                 "modes: manual, auto-read, edit, auto, plan\n";
}

// cai-tools (docs/cai.md). The `cai` wrapper is the one entry point: `maic cai TOOL ...`, `maic trans-fairy ...`,
// `maic trans-fairy-write ...` and `maic help cai|TOOL` exec it with the arguments untouched, before any option of
// maic's own is read, so a cai flag (--json, --model, -h) is never taken for one of maic's. The exec keeps stdin,
// stdout, stderr and the exit code. The wrapper installed beside this binary comes first, then the source tree's.
const std::vector<std::string>& cai_tools() {
    static const std::vector<std::string> tools = {"trans-fairy", "redact", "trans-fairy-write", "notation", "grant", "commit", "enroll", "hook",
                                                   "edit", "time", "document", "name", "fabricate", "sync", "flow", "read", "reflow"};
    return tools;
}

[[noreturn]] void exec_cai(std::vector<std::string> args) {
    std::error_code ec;
    std::vector<std::filesystem::path> where = {maic::root_dir() / "tools" / "cai" / "bin" / "cai"};
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) where.insert(where.begin(), exe.parent_path() / "cai");
    for (const auto& script : where) {
        if (!std::filesystem::is_regular_file(script, ec)) continue;
        std::string path = script.string();
        std::vector<char*> argv = {path.data()};
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        std::cerr << "maic: cannot run " << path << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }
    std::cerr << "maic: the cai wrapper is not installed beside maic or in " << where.back().parent_path().string() << " (maic help cai)\n";
    _exit(127);
}

std::vector<maic::ServiceDef> select(const std::vector<maic::ServiceDef>& all, const std::vector<std::string>& names) {
    if (names.size() == 1 && names[0] == "all") return all;
    std::vector<maic::ServiceDef> out;
    for (const auto& name : names) {
        auto it = std::find_if(all.begin(), all.end(), [&](const auto& d) { return d.name == name; });
        if (it == all.end()) throw std::runtime_error("unknown service: " + name);
        out.push_back(*it);
    }
    return out;
}

// A numbered pick on a terminal: "1", "1 3", "1,3", "all"; empty cancels. Off a terminal: nothing chosen.
std::vector<size_t> menu_pick(const std::string& what, const std::vector<std::string>& rows, bool multi) {
    if (!isatty(STDIN_FILENO)) return {};
    for (size_t i = 0; i < rows.size(); ++i) std::cout << "  " << i + 1 << ". " << rows[i] << "\n";
    std::cout << what << (multi ? " (numbers, or all; Enter cancels): " : " (a number; Enter cancels): ") << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return {};
    std::vector<size_t> out;
    if (line == "all" || line == "a") {
        for (size_t i = 0; i < rows.size(); ++i) out.push_back(i);
        return out;
    }
    for (auto& c : line) if (c == ',') c = ' ';
    std::istringstream in(line);
    for (std::string tok; in >> tok;) {
        int n = std::atoi(tok.c_str());
        if (n >= 1 && static_cast<size_t>(n) <= rows.size()) out.push_back(static_cast<size_t>(n - 1));
        if (!multi) break;
    }
    return out;
}

int cmd_up(const std::vector<maic::ServiceDef>& services) {
    maic::require_armed("start services");
    auto all = maic::load_services(maic::root_dir() / "services");
    int rc = 0;
    for (const auto& def : services) {
        try {
            if (std::string missing = maic::missing_requirement(def); !missing.empty()) throw std::runtime_error(missing);
            if (std::string freed = maic::free_gpu_for(def, all); !freed.empty()) std::cout << freed << "\n";
            std::cout << def.name << ": starting..." << std::flush;
            bool ready = maic::start_service(def);
            std::cout << (ready ? " ready on port " + std::to_string(def.port) : " still starting, check `maic status`") << "\n";
            if (maic::is_fim_server(def.name) && !ready) std::cout << "once it is up, maic gpu load " << def.name << " loads its coder\n";
            if (maic::is_fim_server(def.name) && ready) {
                try {
                    std::cout << maic::load_fim(def, all) << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "maic: " << e.what() << "\n";
                    rc = 1;
                }
            }
        } catch (const std::exception& e) {
            std::cout << " failed\n";
            std::cerr << "maic: " << e.what() << "\n";
            if (std::string why = maic::explain_exit(def, all); !why.empty()) std::cerr << "      " << why << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_gpu(const std::vector<std::string>& args) {
    auto services = maic::load_services(maic::root_dir() / "services");
    if (args.empty() || args[0] == "show") {
        maic::GpuReport report = maic::gpu_report(services);
        std::cout << report.text();
        if (std::string fit = maic::gpu_budget(report, maic::load_settings()); !fit.empty()) std::cout << fit << "\n";
        std::cout << "maic gpu free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui] releases memory without stopping anything; maic gpu load llamacpp-fim brings the coder back\n";
        return 0;
    }
    if (args[0] == "free") {
        std::cout << maic::gpu_free(services, args.size() > 1 ? args[1] : "all");
        return 0;
    }
    if (args[0] == "load") {
        maic::require_armed("load a model");
        std::cout << maic::gpu_load(services, args.size() > 1 ? args[1] : "llamacpp-fim");
        return 0;
    }
    throw std::runtime_error("maic gpu [show | free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui] | load llamacpp-fim]");
}

std::string human_bytes(uintmax_t b);

// `maic models`: the catalog (models/catalog.json plus ~/.config/maic/models.json), what is installed, and
// installing, verifying and removing by id. docs/models.md
int cmd_models(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "list" : args[0];
    if (sub == "check") {
        std::ifstream in(maic::catalog_path());
        if (!in) throw std::runtime_error("no model catalog at " + maic::catalog_path().string());
        nlohmann::json shipped = nlohmann::json::parse(in, nullptr, false, true), user = nlohmann::json::object();
        if (shipped.is_discarded()) throw std::runtime_error(maic::catalog_path().string() + " is not valid JSON");
        std::error_code ec;
        if (std::filesystem::exists(maic::user_catalog_path(), ec)) {
            std::ifstream uin(maic::user_catalog_path());
            user = nlohmann::json::parse(uin, nullptr, false, true);
            if (user.is_discarded()) throw std::runtime_error(maic::user_catalog_path().string() + " is not valid JSON");
        }
        auto problems = maic::check_catalog(shipped, user);
        for (const auto& p : problems) std::cout << "FAIL  " << p << "\n";
        size_t n = maic::parse_catalog(maic::merge_catalog(shipped, user)).size();
        std::cout << n << " entries (" << maic::catalog_path().string() << (user.empty() ? "" : " and " + maic::user_catalog_path().string()) << "), "
                  << problems.size() << " problem" << (problems.size() == 1 ? "" : "s") << "\n";
        return problems.empty() ? 0 : 1;
    }
    auto all = maic::load_catalog();
    if (sub == "list") {
        std::vector<std::pair<std::string, std::string>> presets;
        for (const auto& p : maic::load_settings().presets) presets.emplace_back(p.name, p.model);
        std::printf("%-28s %-10s %-9s %-10s %-9s %s\n", "id", "role", "size", "installed", "current", "presets");
        for (const auto& e : all) {
            long bytes = 0;
            for (const auto& f : e.files) bytes += f.size;
            std::string size = e.shares_entry.empty() ? human_bytes(static_cast<uintmax_t>(bytes)) : "link";
            bool installed = maic::entry_installed(e, all);
            bool some = false;
            for (const auto& f : e.files) some = some || std::filesystem::exists(maic::entry_dir(e) / f.name);
            std::string names;
            for (const auto& p : maic::entry_presets(e, presets)) names += (names.empty() ? "" : ", ") + p;
            std::printf("%-28s %-10s %-9s %-10s %-9s %s\n", e.id.c_str(), e.role.c_str(), size.c_str(), installed ? "yes" : some ? "partial" : "no",
                        maic::entry_current(e) ? e.root.c_str() : "", names.c_str());
        }
        std::cout << "models_dir: " << maic::models_root("llamacpp").parent_path().string() << " (llamacpp/, whisper/, fim/)\n"
                  << "maic models info ID · install ID [--link] · verify ID · remove ID · check (docs/models.md)\n";
        return 0;
    }
    if (args.size() < 2) throw std::runtime_error("usage: maic models [list | info ID | install ID [--link] | verify ID | remove ID [--yes] | check]");
    const maic::CatalogEntry* e = maic::find_entry(all, args[1]);
    if (!e) throw std::runtime_error("no model '" + args[1] + "' in the catalog (maic models lists them)");
    bool link = false, yes = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--link") link = true;
        else if (args[i] == "--yes" || args[i] == "-y") yes = true;
        else throw std::runtime_error("unknown option " + args[i]);
    }
    if (sub == "info") {
        std::vector<std::pair<std::string, std::string>> presets;
        for (const auto& p : maic::load_settings().presets) presets.emplace_back(p.name, p.model);
        std::cout << e->id << ": " << e->name << "\n\n" << e->brief << "\n\n";
        std::cout << "role:      " << e->role << "\n";
        std::cout << "license:   " << e->license << (e->license_url.empty() ? "" : "  " + e->license_url) << "\n";
        std::cout << "source:    " << e->repo << " at " << e->revision << "\n";
        std::cout << "installs:  " << maic::entry_dir(*e).string() << (maic::entry_installed(*e, all) ? "  (installed" : "  (not installed") << (maic::entry_current(*e) ? ", current " + e->root + " model)" : ")") << "\n";
        if (!e->shares_entry.empty()) std::cout << "shares:    " << e->shares_entry << "'s " << e->shares_file << " through a relative link; nothing is copied\n";
        for (const auto& f : e->files) {
            std::cout << "file:      " << f.name << "  " << f.kind << "  " << human_bytes(static_cast<uintmax_t>(f.size)) << "  sha256 " << f.sha256 << "\n"
                      << "           " << f.url << "\n";
        }
        if (std::string v = maic::vram_line(*e); !v.empty()) std::cout << "vram:      " << v << "\n";
        if (e->context) std::cout << "context:   " << e->context << " tokens recommended\n";
        std::string names;
        for (const auto& p : maic::entry_presets(*e, presets)) names += (names.empty() ? "" : ", ") + p;
        std::cout << "presets:   " << (names.empty() ? "none" : names) << "\n";
        auto deps = maic::dependents(*e, all);
        for (const auto& d : deps) std::cout << "needed by: " << d << " (installed; it links this entry's weights)\n";
        if (!e->notes.empty()) std::cout << "notes:     " << e->notes << "\n";
        return 0;
    }
    if (sub == "install") {
        maic::install_entry(*e, all, link, std::cout);
        return 0;
    }
    if (sub == "verify") return maic::verify_entry(*e, std::cout) ? 0 : 1;
    if (sub == "remove") {
        if (std::string why = maic::remove_blocker(*e, all); !why.empty()) throw std::runtime_error("refusing to remove " + e->id + ": " + why);
        if (!yes) {
            if (!isatty(STDIN_FILENO)) {
                std::cerr << "maic: removing deletes files; off a terminal it needs --yes (maic models remove " << e->id << " --yes)\n";
                return 2;
            }
            std::cout << "Remove " << e->id << " from " << maic::entry_dir(*e).string() << (e->shares_entry.empty() ? "" : " (its link only)") << "? [y/N] " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line) || (line != "y" && line != "Y" && line != "yes")) {
                std::cout << "nothing removed\n";
                return 0;
            }
        }
        maic::remove_entry(*e, all, std::cout);
        return 0;
    }
    throw std::runtime_error("usage: maic models [list | info ID | install ID [--link] | verify ID | remove ID [--yes] | check]");
}

// `maic nvim setup llama-vim [--dry-run] [--remove] [--yes] [-u FILE]`: llama.vim's spec as one file MAIC owns in
// the directory the user's lazy.nvim spec imports. Asked on a terminal, --yes off one. docs/nvim.md
int cmd_nvim_setup(const std::vector<std::string>& args) {
    bool dry = false, remove = false, yes = false;
    std::string config;
    bool ok = args.size() >= 2 && args[1] == "llama-vim";
    for (size_t i = 2; ok && i < args.size(); ++i) {
        if (args[i] == "--dry-run") dry = true;
        else if (args[i] == "--remove") remove = true;
        else if (args[i] == "--yes") yes = true;
        else if (args[i] == "-u" && i + 1 < args.size()) config = args[++i];
        else ok = false;
    }
    if (!ok) {
        std::cerr << "usage: maic nvim setup llama-vim [--dry-run] [--remove] [--yes] [-u FILE]\n";
        return 2;
    }
    using Kind = maic::LlamaVimPlan::Kind;
    maic::LlamaVimPlan plan = maic::plan_llama_vim(remove, config);
    (plan.kind == Kind::Refuse || plan.kind == Kind::Error ? std::cerr : std::cout) << plan.text << "\n";
    if (plan.kind != Kind::Write && plan.kind != Kind::Remove) return plan.kind == Kind::Refuse ? 1 : plan.kind == Kind::Error ? 2 : 0;
    if (dry) {
        std::cout << "\n--dry-run: nothing was written.\n";
        return 0;
    }
    if (!yes) {
        if (!isatty(STDIN_FILENO)) {
            std::cerr << "maic: this writes into your nvim config; off a terminal it needs --yes (maic nvim setup llama-vim" << (remove ? " --remove" : "")
                      << " --yes). Nothing was written.\n";
            return 2;
        }
        std::cout << "\n" << (remove ? "Remove it" : plan.update ? "Update it" : "Write it") << "? [y/N] " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || (line != "y" && line != "Y" && line != "yes")) {
            std::cout << "nothing written\n";
            return 0;
        }
    }
    maic::apply_llama_vim(plan);
    if (remove) {
        std::cout << "removed " << plan.file.string() << "\n"
                  << "next: nvim's next start no longer loads llama.vim; :Lazy clean deletes its files, which changes lazy-lock.json\n"
                     "(maic lazy-lock record once you have checked it)\n";
        return 0;
    }
    std::cout << (plan.update ? "updated " : "wrote ") << plan.file.string() << "\nnext:\n";
    if (plan.update) std::cout << "  * restart nvim for the new settings\n";
    else {
        std::cout << "  * open nvim: lazy.nvim installs llama.vim on its next start (or run :Lazy sync)\n"
                     "  * that changes lazy-lock.json, which maic lazy-lock will report; run maic lazy-lock record once you have\n"
                     "    checked it. MAIC re-runs its keymap check after that lock change (maic nvim keymaps any time)\n";
    }
    std::error_code ec;
    if (!std::filesystem::exists(maic::fim_model_link(), ec)) std::cout << "  * maic models install qwen2.5-coder-7b --link: no coder is linked for completion yet\n";
    auto services = maic::load_services(maic::root_dir() / "services");
    auto fim = std::find_if(services.begin(), services.end(), [](const auto& d) { return d.name == "llamacpp-fim"; });
    if (fim != services.end() && maic::service_status(*fim).state == maic::ServiceState::Stopped) {
        std::cout << "  * maic up llamacpp-fim: the completion server on 127.0.0.1:8084 is not running\n";
    }
    return 0;
}

int cmd_down(const std::vector<maic::ServiceDef>& services) {
    auto all = maic::load_services(maic::root_dir() / "services");
    int rc = 0;
    for (const auto& def : services) {
        try {
            maic::stop_service(def);
            std::cout << def.name << ": stopped\n";
            if (std::string back = maic::restore_gpu_after(def, all); !back.empty()) std::cout << back << "\n";
        } catch (const std::exception& e) {
            std::cerr << "maic: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_logs(const maic::ServiceDef& def, size_t lines) {
    std::string tail = maic::log_tail(def, lines);
    if (tail.empty()) {
        std::cerr << "maic: no log yet for " << def.name << "\n";
        return 1;
    }
    std::cout << tail;
    return 0;
}

std::string human_bytes(uintmax_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 4) v /= 1024, ++u;
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

int cmd_path(const std::vector<std::string>& args) {
    maic::Settings settings = maic::load_settings();
    auto services = maic::load_services(maic::root_dir() / "services");
    auto places = maic::known_places(settings, std::filesystem::current_path(), services);
    bool copy = false, names_only = false;
    std::string query;
    for (const auto& a : args) {
        if (a == "--copy" || a == "-c") copy = true;
        else if (a == "--names") names_only = true;
        else if (query.empty()) query = a;
        else throw std::runtime_error("maic path [NAME] [--copy]");
    }
    if (names_only) {
        for (const auto& p : places) std::cout << p.name << "\n";
        return 0;
    }
    if (query.empty()) {
        std::cout << "PLACE                 PATH\n";
        for (const auto& p : places) {
            std::error_code ec;
            std::string mark = !std::filesystem::exists(p.path, ec) ? "  (missing)" : p.is_file ? "" : "/";
            char line[400];
            snprintf(line, sizeof(line), "%-21s %s%s\n", p.name.c_str(), p.path.c_str(), mark.c_str());
            std::cout << line << "    " << p.description << "\n";
        }
        std::cout << "maic path NAME prints one (a unique prefix is enough); --copy puts it on the clipboard; maic open NAME opens it;\n"
                     "eval \"$(maic shell-init)\" gives your shell mcd NAME, mpath NAME and mcp NAME.\n";
        return 0;
    }
    const auto& p = maic::find_place(places, query);
    std::cout << p.path.string() << "\n";
    if (copy) std::cerr << "copied (" << maic::copy_to_clipboard(p.path.string()) << ")\n";
    return 0;
}

int cmd_cd(const std::vector<std::string>& args) {
    std::string name;
    bool subshell = false;
    for (const auto& a : args) {
        if (a == "--subshell" || a == "-s") subshell = true;
        else if (name.empty()) name = a;
        else throw std::runtime_error("maic cd NAME [--subshell]");
    }
    if (name.empty()) throw std::runtime_error("maic cd NAME [--subshell]  (maic path lists the names; mcd from `maic shell-init` changes this shell)");
    maic::Settings settings = maic::load_settings();
    auto services = maic::load_services(maic::root_dir() / "services");
    auto places = maic::known_places(settings, std::filesystem::current_path(), services);
    const auto& p = maic::find_place(places, name);
    std::filesystem::path dir = p.is_file ? p.path.parent_path() : p.path;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) throw std::runtime_error(dir.string() + " does not exist yet");
    if (!subshell) {
        std::cout << dir.string() << "\n";  // cd "$(maic cd NAME)"; a process cannot change its parent's directory
        return 0;
    }
    // --subshell: a shell there; `exit` returns here.
    const char* shell = std::getenv("SHELL");
    std::string sh = shell && *shell ? shell : "/bin/sh";
    std::cerr << "entering " << dir.string() << " in " << sh << "  (exit returns; mcd from `maic shell-init` changes this shell instead)\n";
    if (chdir(dir.c_str()) != 0) throw std::runtime_error("cannot enter " + dir.string());
    setenv("MAIC_PLACE", p.name.c_str(), 1);
    execl(sh.c_str(), sh.c_str(), static_cast<char*>(nullptr));
    throw std::runtime_error("could not start " + sh);
}

int cmd_open(const std::vector<std::string>& args) {
    std::string name, browser;
    bool folder = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--browser" && i + 1 < args.size()) browser = args[++i];
        else if (args[i] == "--folder" || args[i] == "--file-manager" || args[i] == "-f") folder = true;
        else if (name.empty()) name = args[i];
    }
    maic::Settings settings = maic::load_settings();
    auto services = maic::load_services(maic::root_dir() / "services");
    if (name.empty()) {
        // A menu: services first (what runs, where), then the places.
        std::vector<std::string> rows, names;
        for (const auto& def : services) {
            auto st = maic::service_status(def);
            rows.push_back(def.name + "  " + (st.state == maic::ServiceState::Running ? "running" : "stopped") + "  http://127.0.0.1:" + std::to_string(def.port) + "  (browser)");
            names.push_back(def.name);
        }
        rows.push_back("server  the maic web client  (browser)");
        names.push_back("server");
        for (const auto& p : maic::known_places(settings, std::filesystem::current_path(), services)) {
            rows.push_back(p.name + "  " + p.path.string() + "  (file manager)");
            names.push_back(p.name);
        }
        auto picks = menu_pick("open which", rows, false);
        if (picks.empty()) throw std::runtime_error("maic open NAME [--browser default|firefox|chrome]; maic path lists the names");
        name = names[picks.front()];
    }
    if (!browser.empty() && browser != "default" && browser != "firefox" && browser != "chrome") throw std::runtime_error("--browser takes default, firefox or chrome");
    auto [cmd, what] = maic::open_command(name, settings, std::filesystem::current_path(), services, std::nullopt, browser, folder);
    if (std::system(cmd.c_str()) != 0) throw std::runtime_error("could not open " + what);
    std::cout << "opened " << what << "\n";
    return 0;
}

int cmd_artifacts(const std::vector<std::string>& args) {
    auto services = maic::load_services(maic::root_dir() / "services");
    auto artifacts = maic::list_artifacts(services);
    if (args.empty() || args[0] == "list") {
        std::cout << "OWNER/NAME            SIZE       FILES  PATH\n";
        for (const auto& a : artifacts) {
            auto u = maic::measure(a);
            char line[512];
            snprintf(line, sizeof(line), "%-21s %-10s %-6zu %s", (a.owner + "/" + a.name).c_str(), human_bytes(u.bytes).c_str(), u.files, a.path.c_str());
            std::cout << line << (a.resolved.empty() ? "" : " -> " + a.resolved) << "\n    " << a.description << "\n";
        }
        std::cout << "\nclean with: maic artifacts clean OWNER/NAME [--older-than DAYS] [--yes]\n";
        return 0;
    }
    if (args[0] == "clean" && args.size() >= 2) {
        std::optional<std::chrono::hours> older;
        bool yes = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--older-than" && i + 1 < args.size()) older = std::chrono::hours(24 * std::stoi(args[++i]));
            else if (args[i] == "--yes" || args[i] == "-y") yes = true;
            else throw std::runtime_error("unknown option " + args[i]);
        }
        auto it = std::find_if(artifacts.begin(), artifacts.end(), [&](const auto& a) { return a.owner + "/" + a.name == args[1]; });
        if (it == artifacts.end()) throw std::runtime_error("unknown artifact " + args[1] + " (see `maic artifacts`)");
        auto u = maic::measure(*it, older);
        if (u.files == 0) {
            std::cout << "nothing to remove\n";
            return 0;
        }
        std::cout << "remove " << u.files << " files (" << human_bytes(u.bytes) << ") from " << it->path.string() << "? [y/N] ";
        if (!yes) {
            std::string line;
            std::getline(std::cin, line);
            if (line != "y" && line != "Y") {
                std::cout << "kept\n";
                return 0;
            }
        } else {
            std::cout << "yes\n";
        }
        auto removed = maic::clean(*it, older);
        std::cout << "removed " << removed.files << " files (" << human_bytes(removed.bytes) << ")\n";
        return 0;
    }
    usage();
    return 2;
}

// Numbered by position in `sessions` (the pick list uses the numbers); a subagent's transcript is listed
// under the session that delegated it when that one is in the list.
void print_sessions(const std::vector<maic::SessionInfo>& sessions) {
    char today[16];
    std::time_t now = std::time(nullptr);
    std::strftime(today, sizeof(today), "%Y%m%d", std::localtime(&now));
    std::string last_day;
    std::vector<bool> shown(sessions.size(), false);
    auto print_one = [&](size_t i, const std::string& indent) {
        shown[i] = true;
        const auto& s = sessions[i];
        std::string day = s.started.substr(0, 8);
        std::string hm = s.started.size() >= 13 ? s.started.substr(9, 2) + ":" + s.started.substr(11, 2) : "";
        if (day != last_day && indent.empty()) {
            std::cout << (day == today ? "Today" : day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2)) << "\n";
            last_day = day;
        }
        std::string where = std::filesystem::path(s.workspace).filename().string();
        std::cout << indent << "  " << (indent.empty() ? "" : "↳ ") << i + 1 << ". " << hm << "  " << (s.agent.empty() ? "" : s.agent + ": ")
                  << (s.title.empty() ? (s.first_prompt.empty() ? "(no prompt yet)" : s.first_prompt) : s.title)
                  << "  [" << where << "]  " << s.turns << " turn" << (s.turns == 1 ? "" : "s") << (maic::session_running(s) ? "  RUNNING" : "")
                  << (maic::session_lock_reason(s) ? "  LOCKED" : "") << "\n"
                  << indent << "     " << s.id << "  [" << s.home << "]  started in " << s.workspace;
        if (s.opened_in != s.workspace) std::cout << ", last opened in " << s.opened_in;
        if (s.opens > 1) std::cout << " (" << s.opens << " opens)";
        if (!s.host.empty()) std::cout << " on " << s.host;
        std::cout << "\n";
        if (!s.parent.empty()) std::cout << indent << "     resumed from " << s.parent << " (first " << s.parent_records << " records)\n";
    };
    auto listed = [&](const std::string& id) {
        for (const auto& s : sessions) {
            if (s.id == id) return true;
        }
        return false;
    };
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (shown[i] || (!sessions[i].delegated_from.empty() && listed(sessions[i].delegated_from))) continue;
        print_one(i, "");
        for (size_t j = 0; j < sessions.size(); ++j) {
            if (!shown[j] && sessions[j].delegated_from == sessions[i].id) print_one(j, "    ");
        }
    }
}

// -c: the newest session from this directory. -r ID: that session. -r alone: choose from a numbered list.
std::filesystem::path pick_session(bool continue_last, const std::optional<std::string>& id) {
    if (continue_last) {
        auto here = maic::list_sessions(std::filesystem::current_path());
        if (here.empty()) throw std::runtime_error("no earlier session in " + std::filesystem::current_path().string());
        return here.front().path;
    }
    if (id) {
        auto s = maic::find_session(*id);
        if (!s) throw std::runtime_error("no session matching '" + *id + "' (maic sessions)");
        return s->path;
    }
    auto all = maic::list_sessions();
    if (all.empty()) throw std::runtime_error("no sessions yet");
    if (all.size() > 15) all.resize(15);
    std::cerr << "sessions (newest first):\n";
    print_sessions(all);
    std::cerr << "resume which? [1-" << all.size() << "] ";
    std::string line;
    std::getline(std::cin, line);
    size_t n = line.empty() ? 0 : std::stoul(line);
    if (n < 1 || n > all.size()) throw std::runtime_error("cancelled");
    return all[n - 1].path;
}

// maic sessions import FILE [--as FORMAT] [--home general|project|NAME] [--conversation UUID]
int cmd_sessions_import(const std::vector<std::string>& args) {
    std::string file, format = "auto", home = "general", conversation;
    for (size_t i = 1; i < args.size(); ++i) {
        auto value = [&] {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " needs a value");
            return args[++i];
        };
        if (args[i] == "--as") format = value();
        else if (args[i] == "--home") home = value();
        else if (args[i] == "--conversation") conversation = value();
        else if (file.empty() && args[i][0] != '-') file = args[i];
        else throw std::runtime_error("maic sessions import FILE [--as claude-ai|claude-code|auto] [--home general|project|NAME] [--conversation UUID]");
    }
    if (file.empty()) throw std::runtime_error("maic sessions import FILE [--as claude-ai|claude-code|auto] [--home general|project|NAME]");
    maic::ImportedSession s = maic::read_import(file, format, conversation);
    std::filesystem::path dir = home == "project" ? maic::sessions_home("project:" + s.workspace) : maic::sessions_home(home);
    std::filesystem::path path = maic::write_import(s, file, dir);
    std::cerr << "imported " << s.messages << " messages from " << file << " (" << s.format << ")";
    if (s.skipped) std::cerr << ", " << s.skipped << " records or blocks skipped";
    if (s.malformed) std::cerr << ", " << s.malformed << " malformed lines";
    std::cerr << "\n" << path.string() << "\n";
    std::cout << path.stem().string() << "\n";
    return 0;
}

// maic sessions redact ID|FILE [--in-place | -o FILE]
int cmd_sessions_redact(const std::vector<std::string>& args) {
    std::string target, out;
    bool in_place = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--in-place") in_place = true;
        else if ((args[i] == "-o" || args[i] == "--output") && i + 1 < args.size()) out = args[++i];
        else if (target.empty() && args[i][0] != '-') target = args[i];
        else throw std::runtime_error("maic sessions redact ID|FILE [--in-place | -o FILE]");
    }
    if (target.empty() || (in_place && !out.empty())) throw std::runtime_error("maic sessions redact ID|FILE [--in-place | -o FILE]");
    auto s = maic::find_session(target);
    if (!s) throw std::runtime_error("no session matching '" + target + "' (maic sessions)");
    // The copy lands in the current directory by default, so it is never listed as a session itself.
    std::filesystem::path dest = in_place ? s->path : out.empty() ? std::filesystem::current_path() / (s->id + ".redacted.jsonl") : std::filesystem::path(out);
    maic::RedactReport report;
    std::filesystem::path backup;
    if (in_place) backup = maic::redact_session_in_place(s->path, "maic sessions redact " + target + " --in-place", report);
    else report = maic::redact_session(s->path, dest);
    if (report.total() == 0) std::cout << "nothing to redact in " << report.records << " records";
    else {
        std::cout << "redacted " << report.total() << " value" << (report.total() == 1 ? "" : "s") << " in " << report.records << " records:";
        for (const auto& [kind, n] : report.counts) std::cout << " " << kind << " " << n;
    }
    if (report.malformed) std::cout << " (" << report.malformed << " malformed lines redacted as text)";
    std::cout << "\nwrote " << dest.string() << "\n";
    if (!backup.empty()) std::cout << "the original is kept at " << backup.string() << " (cai trans-fairy-write restore " << s->id << " puts it back)\n";
    return 0;
}

maic::SessionInfo need_session(const std::string& id) {
    auto s = maic::find_session(id);
    if (!s) throw std::runtime_error("no session matching '" + id + "' (maic sessions)");
    return *s;
}

// The ID first, then options: a `valued` one takes the next argument, a `bare` one stands alone. Anything else
// is the usage error.
std::pair<std::string, std::map<std::string, std::string>> session_args(const std::vector<std::string>& args, const std::vector<std::string>& valued,
                                                                        const std::vector<std::string>& bare, const std::string& use) {
    std::string id;
    std::map<std::string, std::string> opts;
    for (size_t i = 1; i < args.size(); ++i) {
        if (std::find(valued.begin(), valued.end(), args[i]) != valued.end()) {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " needs a value");
            const std::string& name = args[i];
            opts[name] = args[++i];
        } else if (std::find(bare.begin(), bare.end(), args[i]) != bare.end()) opts[args[i]] = "yes";
        else if (id.empty() && args[i][0] != '-') id = args[i];
        else throw std::runtime_error(use);
    }
    if (id.empty()) throw std::runtime_error(use);
    return {id, opts};
}

// --home general|project|NAME; project is the session's own workspace.
std::filesystem::path home_for(std::map<std::string, std::string>& opts, const maic::SessionInfo& s) {
    std::string home = opts.count("--home") ? opts["--home"] : "general";
    return home == "project" ? maic::sessions_home("project:" + s.workspace) : maic::sessions_home(home);
}

// A text argument given as a file, or - for stdin.
std::string text_from(const std::string& file) {
    if (file == "-") return std::string(std::istreambuf_iterator<char>(std::cin), {});
    std::ifstream in(file);
    if (!in) throw std::runtime_error("can't read " + file);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int created(const std::filesystem::path& path, const std::string& what) {
    std::cerr << what << "\n" << path.string() << "\n";
    std::cout << path.stem().string() << "\n";
    return 0;
}

// maic sessions compose ID --from N [--root FILE|-] [--home general|project|NAME]
int cmd_sessions_compose(const std::vector<std::string>& args) {
    std::string use = "maic sessions compose ID --from N [--root FILE|-] [--home general|project|NAME]";
    auto [id, o] = session_args(args, {"--from", "--root", "--home"}, {}, use);
    if (!o.count("--from")) throw std::runtime_error(use);
    auto s = need_session(id);
    std::string root = o.count("--root") ? text_from(o["--root"]) : "";
    size_t from = std::stoul(o["--from"]);
    auto path = maic::compose_session(s.path, from, root, home_for(o, s));
    return created(path, "composed: records " + std::to_string(from) + " to " + std::to_string(maic::count_records(s.path)) + " of " + s.id +
                             (root.empty() ? "" : " after the root") + "; " + s.id + " is unchanged");
}

// maic sessions graft ID --onto TARGET [--at N] [--home general|project|NAME]
int cmd_sessions_graft(const std::vector<std::string>& args) {
    std::string use = "maic sessions graft ID --onto TARGET [--at N] [--home general|project|NAME]";
    auto [id, o] = session_args(args, {"--onto", "--at", "--home"}, {}, use);
    if (!o.count("--onto")) throw std::runtime_error(use);
    auto s = need_session(id);
    auto target = need_session(o["--onto"]);
    size_t at = o.count("--at") ? std::stoul(o["--at"]) : maic::count_records(target.path);
    auto path = maic::graft_session(target.path, at, s.path, home_for(o, target));
    return created(path, "grafted " + s.id + " after record " + std::to_string(at) + " of " + target.id + "; both are unchanged");
}

// maic sessions inject ID (--text TEXT | --file FILE|-) [--at N] [--role user|system] [--home general|project|NAME]
int cmd_sessions_inject(const std::vector<std::string>& args) {
    std::string use = "maic sessions inject ID (--text TEXT | --file FILE|-) [--at N] [--role user|system] [--home general|project|NAME]";
    auto [id, o] = session_args(args, {"--text", "--file", "--at", "--role", "--home"}, {}, use);
    if (o.count("--text") == o.count("--file")) throw std::runtime_error(use);
    auto s = need_session(id);
    std::string text = o.count("--text") ? o["--text"] : text_from(o["--file"]);
    size_t at = o.count("--at") ? std::stoul(o["--at"]) : maic::count_records(s.path);
    std::string role = o.count("--role") ? o["--role"] : "user";
    auto path = maic::inject_note(s.path, at, role, text, home_for(o, s));
    return created(path, "injected a " + role + " note after record " + std::to_string(at) + " of " + s.id + "; " + s.id + " is unchanged");
}

std::string hms(long seconds) {
    char buf[32];
    if (seconds >= 3600) snprintf(buf, sizeof buf, "%ldh%02ldm", seconds / 3600, seconds % 3600 / 60);
    else if (seconds >= 60) snprintf(buf, sizeof buf, "%ldm%02lds", seconds / 60, seconds % 60);
    else snprintf(buf, sizeof buf, "%lds", seconds);
    return buf;
}

// maic sessions state ID
int cmd_sessions_state(const std::vector<std::string>& args) {
    auto [id, o] = session_args(args, {}, {}, "maic sessions state ID");
    auto s = need_session(id);
    maic::SessionStats st = maic::session_stats(s.path);
    std::cout << (s.title.empty() ? (s.first_prompt.empty() ? "(no prompt yet)" : s.first_prompt) : s.title) << "\n"
              << s.id << "  [" << s.home << "]  " << s.kind << ", started " << s.started << " in " << s.workspace;
    if (s.opened_in != s.workspace) std::cout << ", last opened in " << s.opened_in;
    if (s.opens > 1) std::cout << " (" << s.opens << " opens)";
    if (!s.model.empty()) std::cout << ", model " << s.model;
    if (maic::session_running(s)) std::cout << "  RUNNING";
    if (maic::session_lock_reason(s)) std::cout << "  LOCKED";
    std::cout << "\n";
    if (!s.parent.empty()) std::cout << "resumed from " << s.parent << " (first " << s.parent_records << " records)\n";
    if (!s.delegated_from.empty()) std::cout << "subagent of " << s.delegated_from << " as the " << s.agent << " agent\n";
    std::cout << st.records << " records in this file, " << st.first_time << " to " << st.last_time << "\n"
              << st.turns << " turn" << (st.turns == 1 ? "" : "s") << ", " << st.replies << " repl" << (st.replies == 1 ? "y" : "ies") << ", "
              << st.tool_calls << " tool call" << (st.tool_calls == 1 ? "" : "s");
    if (st.tool_errors) std::cout << " (" << st.tool_errors << " failed)";
    std::vector<std::pair<std::string, size_t>> tools(st.tools.begin(), st.tools.end());
    std::sort(tools.begin(), tools.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < tools.size(); ++i) std::cout << (i ? ", " : ": ") << tools[i].first << " " << tools[i].second;
    std::cout << "\n";
    if (!st.files.empty()) {
        std::cout << "files touched (" << st.files.size() << "):\n";
        for (const auto& f : st.files) std::cout << "  " << f << "\n";
    }
    std::cout << "tokens: " << st.input_tokens << " in, " << st.output_tokens << " out";
    if (long budget = maic::load_settings().budget_tokens; budget > 0) {
        long used = st.input_tokens + st.output_tokens;
        std::cout << "; budget " << used << " of " << budget << (used < budget ? ", " + std::to_string(budget - used) + " left" : ", spent");
    }
    if (st.context) std::cout << "; context window " << st.context;
    std::cout << "\n";
    if (!st.compactions.empty() || st.clears || st.undos) {
        std::cout << "compactions:";
        for (const auto& [stage, n] : st.compactions) std::cout << " " << stage << " " << n;
        if (st.compactions.empty()) std::cout << " none";
        if (st.clears) std::cout << "; clears " << st.clears;
        if (st.undos) std::cout << "; undos " << st.undos;
        std::cout << "\n";
    }
    std::string forks, children;
    for (const auto& other : maic::list_sessions()) {
        if (other.parent == s.id) forks += (forks.empty() ? "" : ", ") + other.id + " (at " + std::to_string(other.parent_records) + ")";
        if (other.delegated_from == s.id) children += (children.empty() ? "" : ", ") + other.id + " (" + other.agent + ")";
    }
    if (!forks.empty()) std::cout << "forks: " << forks << "\n";
    if (!children.empty()) std::cout << "subagents: " << children << "\n";
    return 0;
}

// maic sessions time ID [--slowest N]
int cmd_sessions_time(const std::vector<std::string>& args) {
    auto [id, o] = session_args(args, {"--slowest"}, {}, "maic sessions time ID [--slowest N]");
    auto s = need_session(id);
    maic::SessionTiming t = maic::session_timing(s.path);
    if (t.turns.empty()) {
        std::cout << "no turns yet\n";
        return 0;
    }
    long total = 0;
    std::cout << "turn      time  tools  prompt\n";
    for (size_t i = 0; i < t.turns.size(); ++i) {
        const auto& turn = t.turns[i];
        total += turn.seconds;
        printf("%4zu  %8s  %5zu  %s\n", i + 1, hms(turn.seconds).c_str(), turn.tool_calls, turn.prompt.c_str());
    }
    std::cout << "total " << hms(total) << " over " << t.turns.size() << " turn" << (t.turns.size() == 1 ? "" : "s") << ", " << t.tools.size() << " tool call"
              << (t.tools.size() == 1 ? "" : "s") << "\n";
    std::sort(t.tools.begin(), t.tools.end(), [](const auto& a, const auto& b) { return a.seconds > b.seconds; });
    size_t show = std::min(t.tools.size(), o.count("--slowest") ? std::stoul(o["--slowest"]) : size_t(5));
    if (show && t.tools[0].seconds > 0) std::cout << "slowest tool calls:\n";
    for (size_t i = 0; i < show && t.tools[i].seconds > 0; ++i) {
        printf("  %8s  %s%s\n", hms(t.tools[i].seconds).c_str(), t.tools[i].summary.c_str(), t.tools[i].ok ? "" : "  (failed)");
    }
    return 0;
}

// maic sessions rehome ID [ID...] [HOME] [--subagent | --subagent-only] [--children-of ID] [--dry-run]
int cmd_sessions_rehome(const std::vector<std::string>& args) {
    std::string use = "maic sessions rehome ID [ID...] [project|general|NAME] [--subagent | --subagent-only] [--children-of ID] [--dry-run]";
    std::vector<std::string> words, parents;
    maic::Subagents subagents = maic::Subagents::Stay;
    bool dry = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--subagent" || args[i] == "--subagent-only") {
            maic::Subagents want = args[i] == "--subagent" ? maic::Subagents::Too : maic::Subagents::Only;
            if (subagents != maic::Subagents::Stay && subagents != want) throw std::runtime_error("--subagent and --subagent-only exclude each other");
            subagents = want;
        } else if (args[i] == "--children-of") {
            if (i + 1 >= args.size()) throw std::runtime_error("--children-of needs a session id");
            parents.push_back(args[++i]);
        } else if (args[i] == "--dry-run" || args[i] == "-n") dry = true;
        else if (args[i][0] != '-') words.push_back(args[i]);
        else throw std::runtime_error(use);
    }
    // HOME is the last word; one ID alone goes to its project, as it always has.
    std::string home = "project";
    if (words.size() > 1 || (words.size() == 1 && !parents.empty())) {
        home = words.back();
        words.pop_back();
    }
    if (words.empty() && parents.empty()) throw std::runtime_error(use);
    std::vector<maic::RehomeTarget> targets;
    for (const auto& w : words) targets.push_back({w, subagents});
    for (const auto& p : parents) targets.push_back({p, maic::Subagents::Only});
    size_t moving = 0;
    for (const auto& m : maic::plan_rehome(targets, home)) {
        if (m.to == m.session.path) {
            std::cout << m.session.id << " is already in " << m.session.home << "/\n";
            continue;
        }
        if (!dry) maic::rehome_session(m);
        std::cout << m.session.path.string() << " -> " << m.to.string() << "\n";
        ++moving;
    }
    if (dry) std::cout << "dry run: nothing moved (" << moving << " file" << (moving == 1 ? "" : "s") << " would move)\n";
    else if (moving == 0) std::cout << "nothing to move\n";
    return 0;
}

// maic sessions name ID [--model MODEL]
int cmd_sessions_name(const std::vector<std::string>& args) {
    auto [id, o] = session_args(args, {"--model"}, {}, "maic sessions name ID [--model MODEL]");
    auto s = need_session(id);
    if (maic::session_running(s)) throw std::runtime_error(s.id + " is running: :rename inside it, or wait until it ends");
    maic::Settings settings = maic::load_settings();
    std::string model = o.count("--model") ? o["--model"] : settings.small_model;
    if (model.empty()) throw std::runtime_error("no small_model in settings; pass --model MODEL (maic help settings)");
    auto [provider, name] = maic::resolve_model(settings.providers, model);
    bool local_session = s.model.empty() || !maic::resolve_model(settings.providers, s.model).first.remote();
    if (provider.remote() && local_session) {
        throw std::runtime_error("a remote title model is never used for a local session (" + s.id + " ran on " + (s.model.empty() ? "no named model" : s.model) + ")");
    }
    std::string first;
    for (const auto& t : maic::load_session(s.path).transcript) {
        if (t.type != "user") continue;
        first = t.text;
        break;
    }
    if (first.empty()) throw std::runtime_error(s.id + " has no user turn to name it by");
    std::string title = maic::generate_title(provider, name, first);
    if (title.empty()) throw std::runtime_error(model + " gave no usable title");
    maic::SessionLog::reopen(s.path).write("title", {{"text", title}});
    std::cout << title << "\n";
    return 0;
}

// maic sessions read ID [--range A-B] [--tools]
int cmd_sessions_read(const std::vector<std::string>& args) {
    auto [id, o] = session_args(args, {"--range"}, {"--tools"}, "maic sessions read ID [--range A-B] [--tools]");
    auto s = need_session(id);
    size_t from = 0, to = 0;
    if (o.count("--range")) {
        std::string r = o["--range"];
        size_t dash = r.find('-');
        if (dash == std::string::npos) from = to = std::stoul(r);
        else {
            if (dash) from = std::stoul(r.substr(0, dash));
            if (dash + 1 < r.size()) to = std::stoul(r.substr(dash + 1));
        }
    }
    std::cout << maic::render_text(maic::load_session(s.path), from, to, o.count("--tools"));
    return 0;
}

// diction's launcher: the installed one beside this binary (a release), else the repository's (a dev build).
std::filesystem::path diction_launcher() {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && std::filesystem::exists(exe.parent_path() / "maic-diction", ec)) return exe.parent_path() / "maic-diction";
    return maic::root_dir() / "diction" / "maic-diction";
}

// Replaces this process with diction, its arguments untouched, so its exit code is the one the shell sees.
int exec_diction(const std::vector<std::string>& args) {
    std::filesystem::path launcher = diction_launcher();
    std::error_code ec;
    if (std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) setenv("MAIC_BIN", exe.c_str(), 1);
    std::vector<char*> argv;
    std::string name = launcher.string();
    argv.push_back(name.data());
    std::vector<std::string> copy = args;
    for (auto& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(name.c_str(), argv.data());
    throw std::runtime_error("could not start diction at " + name);
}

// maic model resolve NAME: what a preset name or provider/model means here, for helpers outside the binary.
int cmd_model(const std::vector<std::string>& args) {
    if (args.size() != 2 || args[0] != "resolve") throw std::runtime_error("maic model resolve NAME   (a preset like qwen-4b, or provider/model)");
    maic::Settings s = maic::load_settings();
    std::string model = args[1];
    int context = 0;
    if (auto p = maic::find_preset(s.presets, model)) {
        model = p->model;
        context = p->context;
    }
    model = maic::resolve_model_alias(model);
    auto [provider, name] = maic::resolve_model(s.providers, model);
    // A local server's window is the one it was started with; a preset never claims more than that.
    int served = provider.name == "llamacpp" ? s.context : provider.name == "llamacpp-2" ? s.context_2 : provider.options.value("context_window", 0);
    if (!context || (served && maic::is_llama_server(provider.name) && served < context)) context = served;
    nlohmann::json out = {{"provider", provider.name}, {"kind", provider.kind}, {"base_url", provider.base_url}, {"model", name}, {"context", context},
                          {"remote", provider.remote()}, {"api_key_env", provider.api_key_env}, {"api_key_command", provider.api_key_command}};
    std::cout << out.dump() << "\n";
    return 0;
}

// `maic trust imports --approve`: asks at the terminal about each import of your own instruction files that waits
// for approval (docs/instructions.md, Imports from your own files).
int cmd_approve_imports() {
    maic::Settings settings = maic::load_settings();
    std::vector<maic::PendingImport> pending;
    maic::load_instructions(std::filesystem::current_path(), settings.instructions, {}, &pending);
    if (pending.empty()) {
        std::cout << "no import of your own instruction files waits for approval\n";
        return 0;
    }
    if (!isatty(STDIN_FILENO)) {
        std::cerr << "maic: approving an import asks at a terminal; run maic trust imports --approve in one. Nothing was approved.\n";
        return 2;
    }
    for (const auto& p : pending) {
        std::cout << "\nyour instruction file imports a file from outside your trusted directories:\n";
        for (const auto& l : maic::import_prompt(p.importer, p.target, maic::import_exception_status(p.importer, p.target))) std::cout << "  " << l << "\n";
        std::cout << "approve it? [y/N] " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line == "y" || line == "Y" || line == "yes") {
            maic::approve_import(p.importer, p.target, maic::Origin::Local);
            std::cout << "approved: it is read from the next turn of every session\n";
        } else {
            std::cout << "not approved\n";
        }
    }
    return 0;
}

int cmd_settings(const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "read") {
        if (args.size() != 2 || args[1] != "diction") throw std::runtime_error("maic settings read diction   (diction.lua beside settings.lua, as JSON)");
        // diction.lua is the user's own file: it runs at global_lua (full by default), so the settings come first.
        std::filesystem::path file = maic::settings_path().parent_path() / "diction.lua";
        maic::load_settings();
        nlohmann::json table = nlohmann::json::object();
        if (std::filesystem::exists(file)) {
            // Whatever it prints goes to stderr: stdout is the JSON diction reads.
            fflush(stdout);
            int out = dup(STDOUT_FILENO);
            dup2(STDERR_FILENO, STDOUT_FILENO);
            struct Restore {
                int fd;
                ~Restore() {
                    fflush(stdout);
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
            } restore{out};
            maic::LuaDataLimits limits = maic::lua_data_limits();
            table = maic::eval_lua_data_file(file, std::filesystem::current_path(), limits.tier, limits.memory_mb);
        }
        std::cout << table.dump() << "\n";
        return 0;
    }
    if (!args.empty() && args[0] == "init") {
        bool as_json = args.size() > 1 && args[1] == "--json";
        maic::write_default_settings(as_json);
        std::filesystem::path p = maic::settings_path();
        if (!as_json) p.replace_extension(".lua");
        std::cout << "wrote " << p.string() << "\n";
        return 0;
    }
    std::filesystem::path lua = maic::settings_path();
    lua.replace_extension(".lua");
    bool have = std::filesystem::exists(lua) || std::filesystem::exists(maic::settings_path());
    std::cout << (std::filesystem::exists(lua) ? lua : maic::settings_path()).string() << (have ? "" : "  (not created yet: maic settings init, or init --json)") << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // --trust first: it decides which project settings the loads below may apply. The global settings are read
    // before it, for where the chain of project directories ends (instructions.bound).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a != "--trust" && a.rfind("--trust=", 0) != 0) continue;
        auto lua = maic::parse_lua_tier(a == "--trust" ? "full" : a.substr(8));
        if (!lua) {
            std::cerr << "maic: --trust=LEVEL takes full, sandbox or restricted\n";
            return 2;
        }
        try {
            maic::load_settings();
        } catch (const std::exception&) {
            // reported by whichever command loads the settings properly
        }
        for (const auto& p : maic::project_dirs(std::filesystem::current_path())) maic::trust_for_session(p.dir, *lua);
    }
    // Service files reach the models directory as ${MAIC_MODELS}; it comes from settings.
    try {
        maic::Settings early = maic::load_settings();
        if (!early.models_dir.empty()) setenv("MAIC_MODELS_DIR", early.models_dir.c_str(), 0);
        setenv("MAIC_CONTEXT", std::to_string(early.context).c_str(), 1);
        setenv("MAIC_CONTEXT_2", std::to_string(early.context_2).c_str(), 1);
    } catch (const std::exception&) {
        // a broken settings file is reported by whichever command loads it properly
    }
    if (argc >= 2) {
        // cai first, on the raw argv: its arguments are its own (docs/cai.md).
        std::string first = argv[1];
        std::vector<std::string> pass(argv + 2, argv + argc);
        if (first == "cai") exec_cai(pass);
        if (first == "trans-fairy" || first == "trans-fairy-write") {
            pass.insert(pass.begin(), first);
            exec_cai(pass);
        }
        // maic help cai [TOOL], maic help trans-fairy[-write]: the tools' own help. Other cai names are left to
        // maic's own pages (`edit` is one), so `maic help cai edit` is how to reach that tool's.
        if ((first == "help" || first == "-h" || first == "--help") && argc >= 3) {
            std::string topic = argv[2];
            if (topic == "cai" && argc >= 4) exec_cai({argv[3], "--help"});
            if (topic == "cai") exec_cai({"--help"});
            if (topic == "trans-fairy" || topic == "trans-fairy-write") exec_cai({topic, "--help"});
        }
    }
    // diction keeps its own flags (-m, --mode, -h, ...), so it is handed off before any of them is read here.
    if (argc >= 2 && std::string(argv[1]) == "diction") {
        try {
            return exec_diction(std::vector<std::string>(argv + 2, argv + argc));
        } catch (const std::exception& e) {
            std::cerr << "maic: " << e.what() << "\n";
            return 1;
        }
    }
    std::vector<std::string> args;
    // Clustered short flags: -pi is -p -i. A flag that takes a value (-m, -C) must come last in a cluster.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool cluster = a.size() > 2 && a[0] == '-' && a[1] != '-' &&
                       std::all_of(a.begin() + 1, a.end(), [](unsigned char c) { return std::isalpha(c); });
        if (!cluster) {
            args.push_back(a);
            continue;
        }
        for (size_t j = 1; j < a.size(); ++j) args.push_back(std::string("-") + a[j]);
    }
    try {
        // Options that apply to the agent (TUI or headless).
        maic::TuiOptions tui;
        maic::HeadlessOptions headless;
        bool print = false;
        bool interactive = false;
        std::optional<bool> append;
        bool continue_last = false;
        std::optional<std::string> resume_id;
        bool resume = false;
        std::vector<std::string> rest;
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto value = [&](const char* flag) {
                if (i + 1 >= args.size()) throw std::runtime_error(std::string(flag) + " needs a value");
                return args[++i];
            };
            if (a == "--model" || a == "-m") tui.model = headless.model = value("--model");
            else if (a == "--mode") tui.mode = headless.mode = value("--mode");
            else if (a == "-p" || a == "--print") {
                print = true;
                // The prompt is the next token unless that is another flag; a bare "-" means stdin.
                if (i + 1 < args.size() && (args[i + 1] == "-" || args[i + 1][0] != '-')) headless.prompt = args[++i];
            } else if (a == "-" && print && headless.prompt.empty()) {
                headless.prompt = "-";  // `maic -pi -`: the stdin marker after other flags
            } else if (a == "-c" || a == "--continue") continue_last = true;
            else if (a == "-r" || a == "--resume") {
                resume = true;
                if (i + 1 < args.size() && args[i + 1][0] != '-') resume_id = args[++i];
            } else if (a == "--record" || a == "--transcript") headless.record = true, tui.record = true;
            else if (a == "--no-record") headless.record = false, tui.record = false;
            else if (a == "--append") append = true;
            else if (a == "--no-append") append = false;
            else if (a == "--fork-at") tui.fork_at = headless.fork_at = std::stoul(value("--fork-at"));
            else if (a == "--interactive" || a == "-i") interactive = true;
            else if (a == "--system" || a == "-S") tui.system = headless.system = value("--system");
            else if (a == "--no-instructions") tui.load_instructions = headless.load_instructions = false;
            else if (a == "--trust" || a.rfind("--trust=", 0) == 0) continue;  // read before anything else, at the top of main
            else if (a == "--prefill" || a == "--prefix") tui.prefill = headless.prefill = value(a.c_str());
            else if (a == "--rule") {
                std::string r = value("--rule");
                tui.rules.push_back(r);
                headless.rules.push_back(r);
            }
            else if (a == "--harness") tui.harness = headless.harness = value("--harness");
            else if (a == "--accept-dumb-auto") tui.accept_dumb_auto = headless.accept_dumb_auto = true;
            else if (a == "--bare") tui.bare = true;
            else if (a == "--xtc") {
                // --xtc P or --xtc P,T (threshold defaults to 0.1)
                std::string v = value("--xtc");
                std::string p = v, t = "0.1";
                if (auto c = v.find(','); c != std::string::npos) p = v.substr(0, c), t = v.substr(c + 1);
                nlohmann::json pj = nlohmann::json::parse(p, nullptr, false), tj = nlohmann::json::parse(t, nullptr, false);
                if (!pj.is_number() || !tj.is_number()) throw std::runtime_error("--xtc takes a probability, or probability,threshold (e.g. 0.5 or 0.5,0.1)");
                tui.sampling["xtc_probability"] = headless.sampling["xtc_probability"] = pj;
                tui.sampling["xtc_threshold"] = headless.sampling["xtc_threshold"] = tj;
            } else if (a == "--sampling") {
                std::string kv = value("--sampling");
                auto eq = kv.find('=');
                if (eq == std::string::npos || eq == 0) throw std::runtime_error("--sampling takes KEY=VALUE (e.g. temperature=0.7, min_p=0.05)");
                nlohmann::json v = nlohmann::json::parse(kv.substr(eq + 1), nullptr, false);
                if (v.is_discarded()) v = kv.substr(eq + 1);
                tui.sampling[kv.substr(0, eq)] = headless.sampling[kv.substr(0, eq)] = v;
            } else if (a == "--ban-pattern") {
                for (const auto& b : maic::expand_ban_entry(value("--ban-pattern"))) {
                    tui.ban_patterns.push_back(b);
                    headless.ban_patterns.push_back(b);
                }
            } else if (a == "--ban") {
                for (const auto& b : maic::expand_ban_entry(value("--ban"))) {
                    tui.bans.push_back(b);
                    headless.bans.push_back(b);
                }
            }
            else if (a == "--ctx") {
                int n = std::atoi(value("--ctx").c_str());
                if (n < 1024) throw std::runtime_error("--ctx takes the context window in tokens (16384, 32768, ...)");
                tui.ctx = headless.ctx = n;
                setenv("MAIC_CONTEXT", std::to_string(n).c_str(), 1);  // service files read it at load
            } else if (a == "--ctx2") {
                int n = std::atoi(value("--ctx2").c_str());
                if (n < 1024) throw std::runtime_error("--ctx2 takes the side server's context window in tokens (8192, 16384, ...)");
                tui.ctx2 = headless.ctx2 = n;
                setenv("MAIC_CONTEXT_2", std::to_string(n).c_str(), 1);
            } else if (a == "--image" || a == "-I") {
                std::filesystem::path f = value(a.c_str());
                tui.images.push_back(f);
                headless.images.push_back(f);
            } else if (a == "--context" || a == "-C") {
                std::string f = value("--context");
                tui.context.push_back(f);
                headless.context.push_back(f);
            } else if (a == "--json") headless.json = true;
            else if (a == "--think") headless.think = true;
            else if (a == "-h" || a == "--help" || a == "help") {
                if (i + 1 < args.size()) {
                    if (args[i + 1] == "diction") return exec_diction({"--help"});
                    std::cout << maic::help_text(args[i + 1]) << "\n";
                    return 0;
                }
                usage(std::cout);  // asked for: stdout, so it pipes
                return 0;
            } else if (a == "-V" || a == "--version" || a == "version") {
                std::cout << "maic " MAIC_VERSION "\n";
                return 0;
            } else rest.push_back(a);
        }
        // `maic -pi "text"` or `maic -pC file "text"`: with -p given and no prompt yet, one leftover word is the prompt.
        if (print && headless.prompt.empty() && rest.size() == 1) {
            headless.prompt = rest[0];
            rest.clear();
        }
        if (continue_last || resume) tui.resume = headless.resume = pick_session(continue_last, resume_id);
        if (append) tui.append = headless.append = *append;
        if (tui.fork_at) {
            if (!tui.resume) throw std::runtime_error("--fork-at needs -c or -r");
            size_t have = maic::count_records(*tui.resume);
            if (*tui.fork_at < 1 || *tui.fork_at > have) throw std::runtime_error("--fork-at: that session has " + std::to_string(have) + " records");
            if (append && *append) throw std::runtime_error("--fork-at writes a new file; it can't --append");
            tui.append = headless.append = false;
        }
        if (headless.append) headless.record = true;
        if (print && interactive) {
            // An interactive session that starts with the prompt: interactive transcript rules apply, whatever
            // the order of the flags. --json has no meaning here.
            if (headless.json) std::cerr << "maic: --json applies to headless runs only; ignoring it\n";
            tui.initial_prompt = headless.prompt.empty() ? "-" : headless.prompt;
            return maic::run_tui(tui);
        }
        if (print) return maic::run_headless(headless);
        if (rest.empty()) return maic::run_tui(tui);

        const std::string& cmd = rest[0];
        std::vector<std::string> cargs(rest.begin() + 1, rest.end());

        // The lock commands come first so a broken service file can never block them.
        if (cmd == "trip") {
            std::string reason = "manual trip";
            for (size_t i = 0; i < cargs.size(); ++i) reason += (i == 0 ? ": " : " ") + cargs[i];
            maic::trip_tripwire(reason);
            std::cout << "harness: TRIPPED. Nothing will run until `maic unlock`.\n";
            return 0;
        }
        if (cmd == "unlock") {
            // What is locked: the machine (global) lock, and every session's own lock.
            auto machine = maic::tripwire_state();
            struct Row { std::string text; std::filesystem::path lock; };
            std::vector<Row> rows;
            if (machine) rows.push_back({"MACHINE LOCK (global; outranks every session)\n     " + *machine, {}});
            auto sessions = maic::list_sessions();
            for (const auto& s : sessions) {
                auto reason = maic::session_lock_reason(s);
                if (!reason) continue;
                std::string what = s.title.empty() ? s.first_prompt : s.title;
                rows.push_back({"session " + s.id + " [" + s.kind + "]  " + (what.empty() ? "(no prompt yet)" : what.substr(0, 60)) + "\n     dir " + s.workspace +
                                    "  model " + s.model + "  " + (maic::session_running(s) ? "RUNNING pid " + std::to_string(s.pid) : "not running") + "\n     " + *reason,
                                s.path.string() + ".tripped"});
            }
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(maic::runtime_sessions_dir(), ec)) {
                if (e.path().extension() != ".tripped") continue;
                std::ifstream in(e.path());
                std::string reason((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                rows.push_back({"temporary session " + e.path().stem().string() + "\n     " + reason, e.path()});
            }
            if (rows.empty()) {
                std::cout << "nothing is locked\n";
                return 0;
            }
            std::vector<size_t> picks;
            if (!cargs.empty()) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    bool m = cargs[0] == "machine" || cargs[0] == "global" ? rows[i].lock.empty()
                             : cargs[0] == "all-sessions" || cargs[0] == "sessions"  ? !rows[i].lock.empty()
                             : cargs[0] == "all"                                    ? true
                                                                                      : rows[i].text.find(cargs[0]) != std::string::npos;
                    if (m) picks.push_back(i);
                }
                if (picks.empty()) throw std::runtime_error("nothing locked matches '" + cargs[0] + "' (maic unlock lists what is)");
            } else {
                std::vector<std::string> texts;
                for (const auto& r : rows) texts.push_back(r.text);
                picks = menu_pick("unlock which", texts, true);
                if (picks.empty()) {
                    std::cout << "nothing chosen (maic unlock machine | session ID | all-sessions | all)\n";
                    return isatty(STDIN_FILENO) ? 0 : 2;
                }
            }
            int rc = 0;
            for (size_t i : picks) {
                if (rows[i].lock.empty()) {
                    // Drop any cached sudo login first so unlocking the machine always needs the password.
                    std::cout << "machine lock: unlocking (sudo)\n";
                    if (std::system("sudo -k && sudo /usr/local/sbin/maic-lock reset") != 0) rc = 1;
                } else {
                    std::filesystem::remove(rows[i].lock, ec);
                    std::cout << "removed " << rows[i].lock.string() << "\n";
                }
            }
            return rc;
        }
        if (cmd == "settings") return cmd_settings(cargs);
        if (cmd == "trust" && cargs == std::vector<std::string>{"imports", "--approve"}) return cmd_approve_imports();
        if (cmd == "trust" || cmd == "untrust") {
            std::cout << maic::trust_command(cmd, cargs, std::filesystem::current_path()) << "\n";
            return 0;
        }
        if (cmd == "model") return cmd_model(cargs);
        if (cmd == "server") {
            // --model and --mode were taken by the agent options above; the server wants them too.
            if (tui.model) cargs.insert(cargs.end(), {"--model", *tui.model});
            if (tui.mode) cargs.insert(cargs.end(), {"--mode", *tui.mode});
            return maic::server::run_server_command(cargs);
        }
        if (cmd == "doctor") return maic::run_doctor();
        if (cmd == "setup") return maic::run_setup();
        if (cmd == "lua") {
            maic::Lua lua(std::filesystem::current_path());
            maic::Lua::Result r;
            if (cargs.empty() || cargs[0] == "-i") {
                // A REPL, like `luajit -i`: `=expr` shows a value, unfinished lines continue, Ctrl-D leaves.
                bool tty = isatty(STDIN_FILENO);
                if (tty) std::cout << "maic lua (" << lua.run("return jit.version").output << "  Ctrl-D leaves; =expr shows a value; the maic table is loaded)\n";
                std::string chunk;
                for (std::string line;;) {
                    if (tty) std::cout << (chunk.empty() ? "lua> " : "...> ") << std::flush;
                    if (!std::getline(std::cin, line)) break;
                    if (chunk.empty() && !line.empty() && line[0] == '=') line = "return " + line.substr(1);
                    chunk += (chunk.empty() ? "" : "\n") + line;
                    if (lua.incomplete(chunk)) continue;
                    // Statements print nothing; an expression is run as `return expr` so `1+1` shows 2.
                    maic::Lua::Result res = lua.compiles("return " + chunk) ? lua.run("return " + chunk) : lua.run(chunk);
                    if (!res.ok) std::cout << "error: " << res.output << (res.output.empty() || res.output.back() != '\n' ? "\n" : "");
                    else if (!res.output.empty()) std::cout << res.output;
                    chunk.clear();
                }
                if (tty) std::cout << "\n";
                return 0;
            }
            if (cargs[0] == "-e") {
                if (cargs.size() < 2) throw std::runtime_error("maic lua -e CODE");
                r = lua.run(cargs[1]);
            } else {
                std::string argv_lua = "arg = {";
                for (size_t i = 1; i < cargs.size(); ++i) argv_lua += "[" + std::to_string(i) + "]=" + nlohmann::json(cargs[i]).dump() + ",";
                lua.run(argv_lua + "}");
                r = lua.run_file(cargs[0]);
            }
            if (r.ok) std::cout << r.output;
            else std::cerr << "maic lua: " << r.output << (r.output.empty() || r.output.back() != '\n' ? "\n" : "");
            return r.ok ? 0 : 1;
        }
        if (cmd == "tools" && !cargs.empty() && cargs[0] == "check") {
            // Every manifest, good or bad, with the reason: the loader's notices plus the ones that loaded.
            int problems = 0, seen = 0;
            for (const auto& dir : {std::filesystem::current_path() / ".maic" / "tools", maic::global_tools_dir()}) {
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec)) continue;
                std::vector<std::filesystem::path> manifests;
                for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (e.is_directory(ec) && std::filesystem::is_regular_file(e.path() / "tool.json", ec)) manifests.push_back(e.path() / "tool.json");
                }
                std::sort(manifests.begin(), manifests.end());
                for (const auto& m : manifests) {
                    ++seen;
                    try {
                        maic::ScriptTool t = maic::read_script_tool(m);
                        std::cout << "ok    " << m.string() << ": " << t.name << " (" << maic::script_tool_language(t) << ")\n";
                    } catch (const std::exception& e) {
                        ++problems;
                        std::cout << "FAIL  " << m.string() << ": " << e.what() << "\n";
                    }
                }
            }
            auto set = maic::load_script_tools(std::filesystem::current_path());
            for (const auto& n : set.notices) {
                if (n.find("already defined") != std::string::npos) {
                    ++problems;
                    std::cout << "FAIL  " << n.substr(n.find(": ") + 2) << "\n";
                }
            }
            if (seen == 0) std::cout << "no tool manifests here (.maic/tools/*/tool.json) or in " << maic::global_tools_dir().string() << "\n";
            else std::cout << seen << " manifest" << (seen == 1 ? "" : "s") << ", " << problems << " problem" << (problems == 1 ? "" : "s") << "\n";
            return problems ? 1 : 0;
        }
        if (cmd == "tools" && !cargs.empty() && cargs[0] == "new") {
            std::string name, lang = "python";
            bool global = false;
            for (size_t i = 1; i < cargs.size(); ++i) {
                if (cargs[i] == "--lang" && i + 1 < cargs.size()) lang = cargs[++i];
                else if (cargs[i] == "--global") global = true;
                else if (name.empty()) name = cargs[i];
                else throw std::runtime_error("usage: maic tools new NAME --lang python|sh|perl|node [--global]");
            }
            if (name.empty()) throw std::runtime_error("usage: maic tools new NAME --lang python|sh|perl|node [--global]");
            auto dir = (global ? maic::global_tools_dir() : std::filesystem::current_path() / ".maic" / "tools") / name;
            for (const auto& f : maic::scaffold_script_tool(dir, name, lang)) std::cout << "wrote " << f.string() << "\n";
            std::cout << "Edit the description, the parameters and the script; `maic tools check` validates it (maic help tools).\n";
            return 0;
        }
        if (cmd == "tools") {
            std::cout << "built-in tools (what the model can call; every one goes through the harness):\n";
            for (const auto& t : maic::tool_schemas()) {
                std::string desc = t["function"].value("description", "");
                if (auto nl = desc.find('\n'); nl != std::string::npos) desc = desc.substr(0, nl);
                if (desc.size() > 110) desc = desc.substr(0, 107) + "...";
                char line[200];
                snprintf(line, sizeof(line), "  %-14s %s\n", t["function"].value("name", "").c_str(), desc.c_str());
                std::cout << line;
            }
            std::cout << "\nhelpers beside maic (run through run_shell; on the allow list):\n"
                         "  maic-workflow-edit    edit a ComfyUI workflow's fields without touching its wiring\n"
                         "  maic-storyboard       a story JSON into a manga workflow, one panel per turn\n"
                         "  maic-danbooru-tags    check prompt tags against a local copy of Danbooru's vocabulary\n"
                         "  maic-panel-check      one manga panel's prompt, negative, sampler and captions, with the usual mistakes flagged\n";
            std::cout << "\ncai-tools (docs/cai.md; `cai TOOL ...` and `maic cai TOOL ...` run the same, `maic-cai` is `cai`; on the\n"
                         "allow list: read, time, trans-fairy state, the listing and trans-fairy's help; maic help cai TOOL):\n";
            for (const auto& t : cai_tools()) {
                std::string maic_form = t == "trans-fairy" || t == "trans-fairy-write" ? "maic " + t : "maic cai " + t;
                char line[200];
                snprintf(line, sizeof(line), "  %-22s %s\n", ("cai " + t).c_str(), maic_form.c_str());
                std::cout << line;
            }
            auto set = maic::load_lua_tools(std::filesystem::current_path());
            if (std::filesystem::is_directory(std::filesystem::current_path() / ".maic" / "tools") && !maic::trusted(std::filesystem::current_path())) {
                std::cout << "\nthis directory is untrusted: its .maic/tools/ are not loaded (maic trust, or --trust for one run; maic help trust)\n";
            }
            std::cout << "\nuser-defined Lua tools for this directory:\n";
            if (set.tools.empty()) {
                std::cout << "  none. Put a <name>.lua in .maic/tools/ here or in " << maic::global_tools_dir().string() << " (maic help tools)\n";
            }
            for (const auto& t : set.tools) std::cout << "  " << t.name << "  " << t.file.string() << "\n    " << t.description << "\n";
            for (const auto& n : set.notices) std::cout << n << "\n";
            std::vector<std::string> taken;
            for (const auto& t : set.tools) taken.push_back(t.name);
            auto scripts = maic::load_script_tools(std::filesystem::current_path(), taken);
            std::cout << "\nscript tools for this directory (a manifest and a script per directory):\n";
            if (scripts.tools.empty()) {
                std::cout << "  none. maic tools new NAME --lang python|sh|perl|node scaffolds one in .maic/tools/NAME/ (maic help tools)\n";
            }
            auto globs = [](const std::vector<std::string>& g) {
                std::string out;
                for (const auto& x : g) out += (out.empty() ? "" : ", ") + x;
                return out.empty() ? "nothing" : out;
            };
            for (const auto& t : scripts.tools) {
                std::cout << "  " << t.name << "  (" << maic::script_tool_language(t) << ")  " << (t.dir / "tool.json").string() << "\n    " << t.description
                          << "\n    reads " << globs(t.reads) << "; writes " << globs(t.writes) << "; timeout " << t.timeout_s << " s\n";
            }
            for (const auto& n : scripts.notices) std::cout << n << "\n";
            return 0;
        }
        if (cmd == "themes" && !cargs.empty() && cargs[0] == "import") {
            std::string scheme, as;
            for (size_t i = 1; i < cargs.size(); ++i) {
                if (cargs[i] == "--as" && i + 1 < cargs.size()) as = cargs[++i];
                else if (scheme.empty()) scheme = cargs[i];
                else throw std::runtime_error("usage: maic themes import COLORSCHEME [--as FILE_NAME]");
            }
            if (scheme.empty()) throw std::runtime_error("usage: maic themes import COLORSCHEME [--as FILE_NAME]");
            try {
                maic::Theme t = maic::import_nvim_theme(scheme, as);
                std::cout << "wrote " << t.path.string() << " (" << t.styles.size() << " roles from nvim; the rest keep the default)\n"
                          << "use it with theme = \"" << t.name << "\" in settings, or :theme " << t.name << " in a session\n";
            } catch (const std::exception& e) {
                std::string msg = e.what();
                if (msg.rfind("nvim has no colorscheme", 0) == 0) {
                    std::string list;
                    for (const auto& c : maic::nvim_colorschemes()) list += (list.empty() ? "" : ", ") + c;
                    msg = "nvim has no colorscheme " + scheme + "; it has: " + list;
                }
                throw std::runtime_error(msg);
            }
            return 0;
        }
        if (cmd == "themes") {
            maic::Settings settings = maic::load_settings();
            for (const auto& t : maic::list_themes()) {
                std::cout << (t.name == settings.theme ? "* " : "  ") << t.name << "  " << (t.path.empty() ? "built in" : t.path.string()) << "\n";
            }
            if (!settings.theme_error.empty()) std::cout << "theme " << settings.theme << ": " << settings.theme_error << "\n";
            return 0;
        }
        if (cmd == "vendor") {
            auto entries = maic::load_vendor_manifest();
            if (cargs.empty() || cargs[0] == "list") {
                std::cout << "vendored services (" << maic::vendor_dir().string() << "):\n";
                for (const auto& e : entries) {
                    auto st = maic::vendor_status(e);
                    std::cout << "  " << e.name << "  " << e.ref << "  "
                              << (st.installed ? "installed" : st.linked ? "linked" : "not installed") << (st.target.empty() ? "" : "  -> " + st.target) << "\n"
                              << "    " << e.description << (st.note.empty() ? "" : "\n    " + st.note) << "\n";
                    if (e.name == "llamacpp" || e.name == "whisper") std::cout << "    model: " << (st.model.empty() ? "none (maic vendor use " + e.name + " PATH)" : st.model) << "\n";
                }
                return 0;
            }
            if (cargs.size() < 2) throw std::runtime_error("maic vendor add|adopt|use|wire|unlink NAME [PATH]");
            auto e = maic::find_vendor(cargs[1]);
            if (!e) throw std::runtime_error("no vendored service named " + cargs[1] + " (maic vendor)");
            if (cargs[0] == "add") maic::vendor_add(*e);
            else if (cargs[0] == "adopt" && cargs.size() == 3) maic::vendor_adopt(*e, cargs[2]);
            else if (cargs[0] == "use" && cargs.size() == 3) {
                std::string coder = maic::fim_current_id();
                maic::vendor_use(*e, cargs[2]);
                if (maic::fim_current_id() != coder) std::cout << maic::reload_fim(maic::load_services(maic::root_dir() / "services"));
            }
            else if (cargs[0] == "model" && cargs.size() >= 4) {
                std::filesystem::path into;
                for (size_t i = 4; i + 1 < cargs.size(); ++i) {
                    if (cargs[i] == "--into") into = cargs[i + 1];
                }
                maic::vendor_model(*e, cargs[2], cargs[3], into);
            }
            else if (cargs[0] == "wire") maic::vendor_wire(*e);
            else if (cargs[0] == "unlink") maic::vendor_unlink(*e);
            else throw std::runtime_error("maic vendor add|adopt|use|model|wire|unlink NAME [PATH | URL SHA256]");
            return 0;
        }
        if (cmd == "init") {
            auto ws = std::filesystem::current_path();
            std::filesystem::create_directories(ws / ".maic");
            bool any = false;
            if (!std::filesystem::exists(ws / ".maic" / "settings.lua") && !std::filesystem::exists(ws / ".maic" / "settings.json")) {
                std::ofstream(ws / ".maic" / "settings.lua") << "-- Project settings for MAIC, committed with the code. Personal overrides go in settings.local.lua\n"
                                                                  "-- (add it to .gitignore). Keys: docs/settings.md\n"
                                                                  "return {\n}\n";
                std::cout << "created .maic/settings.lua\n";
                any = true;
            }
            if (!std::filesystem::exists(ws / "MAIC.md")) {
                std::ofstream(ws / "MAIC.md") << "# " << ws.filename().string() << "\n\nStanding instructions for agents working in this project.\n";
                std::cout << "created MAIC.md: transcripts for this project now go under sessions/projects/. Run `maic` and `:init` to have the agent draft it.\n";
                any = true;
            }
            if (!any) std::cout << "already initialised\n";
            if (!maic::trusted(ws)) {
                std::cout << ws.string() << " is not trusted, so its MAIC.md and .maic/settings.lua are not read until it is: maic trust "
                             "(fully: its Lua runs as you) or maic trust --lua sandbox (its Lua in a child process that cannot reach the system)\n";
            }
            return 0;
        }
        if (cmd == "sessions") {
            if (!cargs.empty() && cargs[0] == "import") return cmd_sessions_import(cargs);
            if (!cargs.empty() && cargs[0] == "redact") return cmd_sessions_redact(cargs);
            if (!cargs.empty() && cargs[0] == "compose") return cmd_sessions_compose(cargs);
            if (!cargs.empty() && cargs[0] == "graft") return cmd_sessions_graft(cargs);
            if (!cargs.empty() && cargs[0] == "inject") return cmd_sessions_inject(cargs);
            if (!cargs.empty() && cargs[0] == "state") return cmd_sessions_state(cargs);
            if (!cargs.empty() && cargs[0] == "time") return cmd_sessions_time(cargs);
            if (!cargs.empty() && cargs[0] == "name") return cmd_sessions_name(cargs);
            if (!cargs.empty() && cargs[0] == "read") return cmd_sessions_read(cargs);
            if (!cargs.empty() && cargs[0] == "rehome") return cmd_sessions_rehome(cargs);
            if (cargs.size() >= 2 && (cargs[0] == "path" || cargs[0] == "export")) {
                auto s = maic::find_session(cargs[1]);
                if (!s) throw std::runtime_error("no session matching '" + cargs[1] + "' (maic sessions)");
                if (cargs[0] == "path") {
                    std::cout << s->path.string() << "\n";
                    return 0;
                }
                std::string md = maic::export_markdown(*s, maic::load_session(s->path));
                if (cargs.size() >= 3 && cargs[2] != "-") {
                    std::ofstream out(cargs[2]);
                    out << md;
                    std::cout << "wrote " << cargs[2] << "\n";
                } else {
                    std::cout << md;
                }
                return 0;
            }
            std::cout << maic::sessions_dir().string() << "\n";
            print_sessions(maic::list_sessions());
            std::cout << "resume: maic -r ID or maic -r PATH (maic -c: newest from this directory) · move: maic sessions rehome ID [ID...] [project|general|NAME]\n";
            return 0;
        }
        if (cmd == "artifacts") return cmd_artifacts(cargs);
        if (cmd == "path" || cmd == "paths" || cmd == "places") return cmd_path(cargs);
        if (cmd == "open") return cmd_open(cargs);
        if (cmd == "cd") return cmd_cd(cargs);
        if (cmd == "gpu" || cmd == "vram") return cmd_gpu(cargs);
        if (cmd == "models") return cmd_models(cargs);
        if (cmd == "nvim" && !cargs.empty() && cargs[0] == "setup") return cmd_nvim_setup(cargs);
        if (cmd == "nvim") {
            // maic nvim keymaps [--all] [-u FILE]: maic.nvim's keymap check, headless, against the user's nvim config.
            bool all = false;
            std::string config;
            bool ok = !cargs.empty() && cargs[0] == "keymaps";
            for (size_t i = 1; ok && i < cargs.size(); ++i) {
                if (cargs[i] == "--all") all = true;
                else if (cargs[i] == "-u" && i + 1 < cargs.size()) config = cargs[++i];
                else ok = false;
            }
            if (!ok) {
                std::cerr << "usage: maic nvim keymaps [--all] [-u FILE]\n       maic nvim setup llama-vim [--dry-run] [--remove] [--yes]\n";
                return 2;
            }
            maic::KeymapReport r = maic::run_keymap_check(config);
            if (config.empty() && r.error.empty()) maic::save_keymap_record(r, maic::lazy_lock_state(maic::lazy_lock_path(maic::load_settings().lazy_lock)).hash);
            (r.error.empty() ? std::cout : std::cerr) << maic::format_keymap_report(r, all);
            return maic::keymap_exit_code(r);
        }
        if (cmd == "lazy-lock" || cmd == "lazylock") {
            std::string out;
            int rc = maic::lazy_lock_command(cargs.empty() ? "" : cargs[0], maic::lazy_lock_path(maic::load_settings().lazy_lock), out);
            std::cout << out;
            return rc;
        }
        if (cmd == "shell-init") {
            std::string shell = cargs.empty() ? "" : cargs[0];
            if (shell.empty()) {
                const char* sh = std::getenv("SHELL");
                shell = sh && std::string(sh).find("bash") != std::string::npos ? "bash" : sh && std::string(sh).find("fish") != std::string::npos ? "fish" : "zsh";
            }
            std::cout << maic::shell_init(shell);
            return 0;
        }

        auto services = maic::load_services(maic::root_dir() / "services");
        if (cmd == "status") {
            maic::StatusReport report = maic::status_report(services);
            try {
                report.lazy_lock = maic::lazy_lock_summary(maic::lazy_lock_state(maic::lazy_lock_path(maic::load_settings().lazy_lock)));
            } catch (const std::exception&) {
                // a broken settings file must not hide the services; the commands that need settings report it
            }
            std::cout << maic::format_status(report);
            return 0;
        }
        if (cmd == "up" || cmd == "down") {
            if (!cargs.empty()) {
                auto selected = select(services, cargs);
                return cmd == "up" ? cmd_up(selected) : cmd_down(selected);
            }
            // No names: a menu of the services with their state.
            std::vector<std::string> rows;
            std::vector<maic::ServiceDef> candidates;
            for (const auto& def : services) {
                auto st = maic::service_status(def);
                bool running = st.state == maic::ServiceState::Running;
                if ((cmd == "up") == running) continue;  // up lists what is stopped, down what runs
                std::string missing = cmd == "up" ? maic::missing_requirement(def) : "";
                rows.push_back(def.name + "  " + (running ? "running, " + st.who() : "stopped") + "  port " + std::to_string(def.port) +
                               (missing.empty() ? "" : "  (" + missing + ")") + "\n     " + def.description);
                candidates.push_back(def);
            }
            if (rows.empty()) {
                std::cout << (cmd == "up" ? "every service is already running" : "no service MAIC started is running") << "\n";
                return 0;
            }
            auto picks = menu_pick(cmd == "up" ? "start which" : "stop which", rows, true);
            if (picks.empty()) {
                std::cout << "nothing chosen (maic " << cmd << " NAME... or all)\n";
                return isatty(STDIN_FILENO) ? 0 : 2;
            }
            std::vector<maic::ServiceDef> selected;
            for (size_t i : picks) selected.push_back(candidates[i]);
            return cmd == "up" ? cmd_up(selected) : cmd_down(selected);
        }
        if (cmd == "logs" && (cargs.size() == 1 || cargs.size() == 2)) {
            size_t lines = cargs.size() == 2 ? std::stoul(cargs[1]) : 40;
            return cmd_logs(select(services, {cargs[0]}).front(), lines);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "maic: " << e.what() << "\n";
        return 1;
    }
}
