#include "commands.hpp"
#include "doctor.hpp"
#include "headless.hpp"
#include "maic/artifacts.hpp"
#include "maic/import.hpp"
#include "maic/paths.hpp"
#include "maic/redact.hpp"
#include "maic/service.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/tripwire.hpp"
#include "maic/bans.hpp"
#include "maic/lua.hpp"
#include "maic/lua_tools.hpp"
#include "maic/vendor.hpp"
#include "server.hpp"
#include "tui.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
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
                 "       --system TEXT|@FILE, -S            operator instructions placed first in the system prompt (front-loads behaviour)\n"
                 "       --no-instructions                  load no MAIC.md / AGENTS.md anywhere; combines with --system\n"
                 "       --ban TEXT|@FILE                   a phrase the model must not say, or a file with one per line (repeatable; :ban)\n"
                 "       --ban-pattern REGEX|@FILE          a POSIX extended regex the reply must not match, or a file of them (maic help bans)\n"
                 "       --xtc P[,T]                        exclude top choices: probability and threshold (0.5,0.1); llama.cpp-style\n"
                 "                                          servers only, Ollama has no XTC (maic help sampling)\n"
                 "       --sampling KEY=VALUE               any sampler key for this run (temperature=0.7, min_p=0.05, seed=7); repeatable\n"
                 "       --harness smart|dumb               smart (default): a model reviews commands and writes the rules would let\n"
                 "                                          through without asking; dumb: the rule list alone (maic help harness)\n"
                 "       --accept-dumb-auto                 skip the warning when combining --harness dumb with --mode auto\n"
                 "       --record / --no-record             keep a transcript or not (interactive: yes by default, or \"record\" in\n"
                 "                                          settings; -p: none by default)\n"
                 "       --append / --no-append             with -c/-r: write into the old session file, or into a new one that\n"
                 "                                          points at it (default: interactive appends; -p records nothing\n"
                 "                                          unless --record, which forks, or --append)\n"
                 "       --fork-at N                        with -c/-r: continue from the old session's first N records only, in a\n"
                 "                                          new file that points at them (the old file is never changed)\n"
                 "\n"
                 "  vendor                     the services MAIC can install for itself (ComfyUI, Ollama, llama.cpp), pinned versions\n"
                 "  vendor add NAME            fetch, verify, build and link one (network; asks nothing else)\n"
                 "  vendor adopt NAME PATH     use an install you already have instead of fetching\n"
                 "  vendor use llamacpp PATH   the GGUF llama-server loads (a link; an upstream-format GGUF, not an Ollama blob)\n"
                 "  vendor model llamacpp URL SHA256 [--into DIR]   download a GGUF, verify it, link it as the model\n"
                 "  vendor unlink NAME         stop using it (nothing is deleted)\n"
                 "  lua [FILE [args...] | -e CODE]   Lua (vendored LuaJIT) here, with the maic table; no arguments: a REPL (maic help lua)\n"
                 "  tools                      the user-defined Lua tools this directory's sessions get (maic help tools)\n"
                 "  doctor                     what this machine has, what MAIC needs, a recommended setup\n"
                 "  status                     harness, services, where they run, quick actions\n"
                 "  up <service...|all>        start services\n"
                 "  down <service...|all>      stop services MAIC started\n"
                 "  logs <service> [lines]     the end of a service's log (default 40 lines)\n"
                 "  artifacts                  where MAIC and its services keep transcripts, logs and outputs\n"
                 "  artifacts clean OWNER/NAME [--older-than DAYS] [--yes]\n"
                 "  sessions                   list session transcripts (where started, where last opened)\n"
                 "  sessions rehome ID [project|general|NAME]   move a transcript to another home (default: project)\n"
                 "  sessions path ID           print a transcript's path\n"
                 "  sessions export ID [FILE]  the transcript as markdown (stdout without FILE)\n"
                 "  sessions import FILE [--as claude-ai|claude-code|auto] [--home general|project|NAME] [--conversation UUID]\n"
                 "                             a claude.ai export or a Claude Code transcript as a new session (prints its id)\n"
                 "  sessions redact ID|FILE [--in-place | -o FILE]   a copy with credential material replaced by [REDACTED:kind]\n"
                 "                             (default: ./<id>.redacted.jsonl, outside the sessions tree)\n"
                 "  settings init [--json]|path  write the global settings file (Lua; --json for JSON), or show where it goes\n"
                 "  init                       scaffold this project: MAIC.md and .maic/settings.lua (transcripts then\n"
                 "                             go under sessions/projects/); :init in a session also drafts the MAIC.md\n"
                 "  server start [--listen ADDR:PORT] [--model M] [--mode MODE]   the remote-access server and its web client\n"
                 "  server token new|list|revoke [NAME]   per-device bearer tokens for it\n"
                 "  server status              its configuration, and whether it is up (maic help server)\n"
                 "  trip [reason]              trip the harness lock now (blocks all actions until unlocked)\n"
                 "  unlock                     reset the harness lock (asks for your sudo password)\n"
                 "\n"
                 "  help [TOPIC]               this text, or one page: maic help help lists the topics; help headless,\n"
                 "                             sessions, modes, keys, vendor, lua, settings, ... (the same pages as :h)\n"
                 "\n"
                 "modes: manual, auto-read, edit, auto, plan\n";
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

int cmd_up(const std::vector<maic::ServiceDef>& services) {
    maic::require_armed("start services");
    int rc = 0;
    for (const auto& def : services) {
        try {
            if (std::string missing = maic::missing_requirement(def); !missing.empty()) throw std::runtime_error(missing);
            std::cout << def.name << ": starting..." << std::flush;
            bool ready = maic::start_service(def);
            std::cout << (ready ? " ready on port " + std::to_string(def.port) : " still starting, check `maic status`") << "\n";
        } catch (const std::exception& e) {
            std::cout << " failed\n";
            std::cerr << "maic: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_down(const std::vector<maic::ServiceDef>& services) {
    int rc = 0;
    for (const auto& def : services) {
        try {
            maic::stop_service(def);
            std::cout << def.name << ": stopped\n";
        } catch (const std::exception& e) {
            std::cerr << "maic: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_logs(const maic::ServiceDef& def, size_t lines) {
    std::ifstream in(maic::service_log_path(def));
    if (!in) {
        std::cerr << "maic: no log yet for " << def.name << "\n";
        return 1;
    }
    std::deque<std::string> tail;
    for (std::string line; std::getline(in, line);) {
        tail.push_back(std::move(line));
        if (tail.size() > lines) tail.pop_front();
    }
    for (const auto& line : tail) std::cout << line << "\n";
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

void print_sessions(const std::vector<maic::SessionInfo>& sessions) {
    char today[16];
    std::time_t now = std::time(nullptr);
    std::strftime(today, sizeof(today), "%Y%m%d", std::localtime(&now));
    std::string last_day;
    for (size_t i = 0; i < sessions.size(); ++i) {
        const auto& s = sessions[i];
        std::string day = s.started.substr(0, 8);
        std::string hm = s.started.size() >= 13 ? s.started.substr(9, 2) + ":" + s.started.substr(11, 2) : "";
        if (day != last_day) {
            std::cout << (day == today ? "Today" : day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2)) << "\n";
            last_day = day;
        }
        std::string where = std::filesystem::path(s.workspace).filename().string();
        std::cout << "  " << i + 1 << ". " << hm << "  " << (s.title.empty() ? (s.first_prompt.empty() ? "(no prompt yet)" : s.first_prompt) : s.title)
                  << "  [" << where << "]  " << s.turns << " turn" << (s.turns == 1 ? "" : "s") << "\n"
                  << "     " << s.id << "  [" << s.home << "]  started in " << s.workspace;
        if (s.opened_in != s.workspace) std::cout << ", last opened in " << s.opened_in;
        if (s.opens > 1) std::cout << " (" << s.opens << " opens)";
        if (!s.host.empty()) std::cout << " on " << s.host;
        std::cout << "\n";
        if (!s.parent.empty()) std::cout << "     resumed from " << s.parent << " (first " << s.parent_records << " records)\n";
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
    std::filesystem::path dest = in_place ? std::filesystem::path(s->path.string() + ".redacting")
                                 : out.empty() ? std::filesystem::current_path() / (s->id + ".redacted.jsonl")
                                               : std::filesystem::path(out);
    maic::RedactReport report = maic::redact_session(s->path, dest);
    if (in_place) {
        std::filesystem::rename(dest, s->path);
        dest = s->path;
    }
    if (report.total() == 0) std::cout << "nothing to redact in " << report.records << " records";
    else {
        std::cout << "redacted " << report.total() << " value" << (report.total() == 1 ? "" : "s") << " in " << report.records << " records:";
        for (const auto& [kind, n] : report.counts) std::cout << " " << kind << " " << n;
    }
    if (report.malformed) std::cout << " (" << report.malformed << " malformed lines redacted as text)";
    std::cout << "\nwrote " << dest.string() << "\n";
    return 0;
}

int cmd_settings(const std::vector<std::string>& args) {
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
            else if (a == "--harness") tui.harness = headless.harness = value("--harness");
            else if (a == "--accept-dumb-auto") tui.accept_dumb_auto = headless.accept_dumb_auto = true;
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
            else if (a == "--context" || a == "-C") {
                std::string f = value("--context");
                tui.context.push_back(f);
                headless.context.push_back(f);
            } else if (a == "--json") headless.json = true;
            else if (a == "--think") headless.think = true;
            else if (a == "-h" || a == "--help" || a == "help") {
                if (i + 1 < args.size()) {
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
            if (!maic::tripwire_state()) {
                std::cout << "harness: not tripped\n";
                return 0;
            }
            // Drop any cached sudo login first so unlocking always needs the password.
            return std::system("sudo -k && sudo /usr/local/sbin/maic-lock reset") == 0 ? 0 : 1;
        }
        if (cmd == "settings") return cmd_settings(cargs);
        if (cmd == "server") {
            // --model and --mode were taken by the agent options above; the server wants them too.
            if (tui.model) cargs.insert(cargs.end(), {"--model", *tui.model});
            if (tui.mode) cargs.insert(cargs.end(), {"--mode", *tui.mode});
            return maic::server::run_server_command(cargs);
        }
        if (cmd == "doctor") return maic::run_doctor();
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
        if (cmd == "tools") {
            auto set = maic::load_lua_tools(std::filesystem::current_path());
            if (set.tools.empty()) {
                std::cout << "no user-defined tools. Put a <name>.lua in .maic/tools/ here or in " << maic::global_tools_dir().string() << " (maic help tools)\n";
            }
            for (const auto& t : set.tools) std::cout << t.name << "  " << t.file.string() << "\n    " << t.description << "\n";
            for (const auto& n : set.notices) std::cout << n << "\n";
            return 0;
        }
        if (cmd == "vendor") {
            auto entries = maic::load_vendor_manifest();
            if (cargs.empty() || cargs[0] == "list") {
                std::cout << "vendored services (" << maic::vendor_dir().string() << "):\n";
                for (const auto& e : entries) {
                    auto st = maic::vendor_status(e);
                    std::cout << "  " << e.name << "  " << (e.kind == "submodule" ? e.ref : e.version) << "  "
                              << (st.installed ? "installed" : st.linked ? "linked" : "not installed") << (st.target.empty() ? "" : "  -> " + st.target) << "\n"
                              << "    " << e.description << (st.note.empty() ? "" : "\n    " + st.note) << "\n";
                    if (e.name == "llamacpp") std::cout << "    model: " << (st.model.empty() ? "none (maic vendor use llamacpp PATH)" : st.model) << "\n";
                }
                return 0;
            }
            if (cargs.size() < 2) throw std::runtime_error("maic vendor add|adopt|use|unlink NAME [PATH]");
            auto e = maic::find_vendor(cargs[1]);
            if (!e) throw std::runtime_error("no vendored service named " + cargs[1] + " (maic vendor)");
            if (cargs[0] == "add") maic::vendor_add(*e);
            else if (cargs[0] == "adopt" && cargs.size() == 3) maic::vendor_adopt(*e, cargs[2]);
            else if (cargs[0] == "use" && cargs.size() == 3) maic::vendor_use(*e, cargs[2]);
            else if (cargs[0] == "model" && cargs.size() >= 4) {
                std::filesystem::path into;
                for (size_t i = 4; i + 1 < cargs.size(); ++i) {
                    if (cargs[i] == "--into") into = cargs[i + 1];
                }
                maic::vendor_model(*e, cargs[2], cargs[3], into);
            }
            else if (cargs[0] == "unlink") maic::vendor_unlink(*e);
            else throw std::runtime_error("maic vendor add|adopt|use|model|unlink NAME [PATH | URL SHA256]");
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
            return 0;
        }
        if (cmd == "sessions") {
            if (!cargs.empty() && cargs[0] == "import") return cmd_sessions_import(cargs);
            if (!cargs.empty() && cargs[0] == "redact") return cmd_sessions_redact(cargs);
            if (cargs.size() >= 2 && (cargs[0] == "rehome" || cargs[0] == "path" || cargs[0] == "export")) {
                auto s = maic::find_session(cargs[1]);
                if (!s) throw std::runtime_error("no session matching '" + cargs[1] + "' (maic sessions)");
                if (cargs[0] == "path") {
                    std::cout << s->path.string() << "\n";
                    return 0;
                }
                if (cargs[0] == "export") {
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
                std::string home = cargs.size() >= 3 ? cargs[2] : "project";
                auto target = maic::rehome_session(*s, home);
                std::cout << "moved to " << target.string() << "\n";
                return 0;
            }
            std::cout << maic::sessions_dir().string() << "\n";
            print_sessions(maic::list_sessions());
            std::cout << "resume: maic -r ID or maic -r PATH (maic -c: newest from this directory) · move: maic sessions rehome ID [project|general|NAME]\n";
            return 0;
        }
        if (cmd == "artifacts") return cmd_artifacts(cargs);

        auto services = maic::load_services(maic::root_dir() / "services");
        if (cmd == "status") {
            std::cout << maic::format_status(maic::status_report(services));
            return 0;
        }
        if ((cmd == "up" || cmd == "down") && !cargs.empty()) {
            auto selected = select(services, cargs);
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
