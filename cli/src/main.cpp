#include "commands.hpp"
#include "doctor.hpp"
#include "headless.hpp"
#include "maic/artifacts.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/tripwire.hpp"
#include "maic/lua.hpp"
#include "maic/vendor.hpp"
#include "tui.hpp"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(std::ostream& out = std::cerr) {
    out << "usage: maic [--model M] [--mode MODE]      the agent, in this directory\n"
                 "       maic -c                            continue the last session started in this directory\n"
                 "       maic -r [ID]                       resume a session by id (or pick from a list)\n"
                 "       maic -p \"prompt\" [--json] [--think] one turn without the UI (prompt \"-\" reads stdin; -c/-r work here too)\n"
                 "       maic -p \"prompt\" --interactive     an interactive session that opens with that prompt sent (-i)\n"
                 "       --context FILE, -C FILE            attach a text file to the conversation before the prompt; repeatable;\n"
                 "                                          FILE \"-\" reads stdin (then the prompt can't also be stdin)\n"
                 "       --record / --no-record             keep a transcript or not (interactive: yes by default, or \"record\" in\n"
                 "                                          settings; -p: none by default)\n"
                 "       --append / --no-append             with -c/-r: write into the old session file, or into a new one that\n"
                 "                                          points at it (default: interactive appends; -p records nothing\n"
                 "                                          unless --record, which forks, or --append)\n"
                 "\n"
                 "  vendor                     the services MAIC can install for itself (ComfyUI, Ollama), pinned versions\n"
                 "  vendor add NAME            fetch, verify, build and link one (network; asks nothing else)\n"
                 "  vendor adopt NAME PATH     use an install you already have instead of fetching\n"
                 "  vendor unlink NAME         stop using it (nothing is deleted)\n"
                 "  lua [FILE [args...] | -e CODE]   Lua (vendored LuaJIT) here, with the maic table; no arguments: a REPL (maic help lua)\n"
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
                 "  settings init [--json]|path  write the global settings file (Lua; --json for JSON), or show where it goes\n"
                 "  init                       scaffold this project: MAIC.md and .maic/settings.lua (transcripts then\n"
                 "                             go under sessions/projects/); :init in a session also drafts the MAIC.md\n"
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
    for (size_t i = 0; i < sessions.size(); ++i) {
        const auto& s = sessions[i];
        std::cout << "  " << i + 1 << ". " << s.id << "  [" << s.home << "]  " << s.turns << " turn" << (s.turns == 1 ? "" : "s") << "\n"
                  << "     " << (s.first_prompt.empty() ? "(no prompt yet)" : s.first_prompt) << "\n"
                  << "     started in " << s.workspace;
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
            else if (a == "--interactive" || a == "-i") interactive = true;
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
        if (cmd == "vendor") {
            auto entries = maic::load_vendor_manifest();
            if (cargs.empty() || cargs[0] == "list") {
                std::cout << "vendored services (" << maic::vendor_dir().string() << "):\n";
                for (const auto& e : entries) {
                    auto st = maic::vendor_status(e);
                    std::cout << "  " << e.name << "  " << (e.kind == "submodule" ? e.ref : e.version) << "  "
                              << (st.installed ? "installed" : st.linked ? "linked" : "not installed") << (st.target.empty() ? "" : "  -> " + st.target) << "\n"
                              << "    " << e.description << (st.note.empty() ? "" : "\n    " + st.note) << "\n";
                }
                return 0;
            }
            if (cargs.size() < 2) throw std::runtime_error("maic vendor add|adopt|unlink NAME [PATH]");
            auto e = maic::find_vendor(cargs[1]);
            if (!e) throw std::runtime_error("no vendored service named " + cargs[1] + " (maic vendor)");
            if (cargs[0] == "add") maic::vendor_add(*e);
            else if (cargs[0] == "adopt" && cargs.size() == 3) maic::vendor_adopt(*e, cargs[2]);
            else if (cargs[0] == "unlink") maic::vendor_unlink(*e);
            else throw std::runtime_error("maic vendor add|adopt|unlink NAME [PATH]");
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
            if (cargs.size() >= 2 && (cargs[0] == "rehome" || cargs[0] == "path")) {
                auto s = maic::find_session(cargs[1]);
                if (!s) throw std::runtime_error("no session matching '" + cargs[1] + "' (maic sessions)");
                if (cargs[0] == "path") {
                    std::cout << s->path.string() << "\n";
                    return 0;
                }
                std::string home = cargs.size() >= 3 ? cargs[2] : "project";
                auto target = maic::rehome_session(*s, home);
                std::cout << "moved to " << target.string() << "\n";
                return 0;
            }
            std::cout << maic::sessions_dir().string() << "\n";
            print_sessions(maic::list_sessions());
            std::cout << "resume: maic -r ID (maic -c: newest from this directory) · move: maic sessions rehome ID [project|general|NAME]\n";
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
