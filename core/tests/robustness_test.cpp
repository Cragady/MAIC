// Hostile and broken inputs must give an error result, never a crash or a hang.
// Add a case here for every crash found in the wild (the first one: std::regex overflowing the stack).
#include "check.hpp"

#include "maic/artifacts.hpp"
#include "maic/harness.hpp"
#include "maic/instructions.hpp"
#include "maic/sandbox.hpp"
#include "maic/session.hpp"
#include "maic/settings.hpp"
#include "maic/tools.hpp"
#include "maic/vendor.hpp"
#include "maic/lua.hpp"
#include "maic/paths.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace fs = std::filesystem;
using namespace maic;
using nlohmann::json;

namespace {

std::atomic<bool> no_cancel{false};

ToolResult tool(const Harness& h, const std::string& name, const json& args) {
    return run_tool(h, name, args, false, no_cancel);
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string read_whole_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    fs::path home = std::getenv("HOME");
    fs::path ws = home / ".cache" / "maic-robustness-test";  // under $HOME so artifact cleaning is allowed
    fs::remove_all(ws);
    fs::create_directories(ws);
    Harness h(ws);

    section("tool arguments of the wrong shape");
    expect(!tool(h, "read_file", json::object()).ok, "read_file with no path -> error");
    expect(!tool(h, "read_file", {{"path", 42}}).ok, "read_file with a numeric path -> error");
    expect(!tool(h, "read_file", {{"path", "x"}, {"offset", "abc"}}).ok, "read_file with a string offset -> error");
    expect(!tool(h, "write_file", {{"path", "a.txt"}}).ok, "write_file with no content -> error");
    expect(!tool(h, "run_shell", {{"command", json::array()}}).ok, "run_shell with an array command -> error");
    expect(!tool(h, "no_such_tool", json::object()).ok, "unknown tool -> error");
    bool threw = false;
    try {
        tool_action(h, "read_file", {{"path", nullptr}});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "tool_action with a null path throws (the agent reports it) instead of crashing");

    section("files that are not what the tool expects");
    static const char kBinary[] = "\x00\x01\xff\xfe binary \x80\x81";
    write_file(ws / "binary.bin", std::string(kBinary, sizeof(kBinary) - 1));
    expect(!tool(h, "read_file", {{"path", "binary.bin"}}).ok && tool(h, "read_file", {{"path", "binary.bin"}}).text.find("binary") != std::string::npos,
           "read_file refuses a binary file");
    expect(!tool(h, "read_file", {{"path", "missing.txt"}}).ok, "read_file on a missing file -> error");
    fs::create_directories(ws / "adir");
    expect(!tool(h, "list_dir", {{"path", "binary.bin"}}).ok, "list_dir on a file -> error");
    expect(!tool(h, "list_dir", {{"path", "nope"}}).ok, "list_dir on a missing directory -> error");
    write_file(ws / "small.txt", "one\ntwo\n");
    expect(tool(h, "read_file", {{"path", "small.txt"}, {"offset", 1000000}}).ok, "read_file far past the end is fine");
    expect(!tool(h, "edit_file", {{"path", "small.txt"}, {"old_string", ""}, {"new_string", "x"}}).ok, "edit_file with an empty old_string -> error");
    expect(tool(h, "write_file", {{"path", "deep/er/still/new.txt"}, {"content", "x"}}).ok, "write_file creates missing parent directories");

    section("tool results for small models");
    {
        std::string big;
        for (int i = 1; i <= 2500; ++i) big += "line " + std::to_string(i) + "\n";
        write_file(ws / "big.txt", big);
        auto r = tool(h, "read_file", {{"path", "big.txt"}});
        expect(r.ok && r.text.find("Showing lines 1-2000 of 2500. Use offset=2001") != std::string::npos, "a capped read says where it stopped and how to continue");
        r = tool(h, "read_file", {{"path", "big.txt"}, {"offset", 2400}});
        expect(r.ok && r.text.find("(End of file - total 2500 lines)") != std::string::npos, "a complete read says it reached the end");
        std::string wide;
        for (int i = 0; i < 100; ++i) wide += std::string(1000, 'w') + "\n";
        write_file(ws / "wide.txt", wide);
        r = tool(h, "read_file", {{"path", "wide.txt"}});
        expect(r.ok && r.text.find("Showing lines 1-") != std::string::npos && r.text.find("of 100. Use offset=") != std::string::npos, "a 50 KB byte cap applies even with few lines");
        write_file(ws / "oneline.txt", std::string(80 * 1024, 'x'));
        r = tool(h, "read_file", {{"path", "oneline.txt"}});
        expect(r.ok && r.text.find("End of file") != std::string::npos && r.text.size() < 3000, "one huge line is truncated, not skipped");
        r = tool(h, "read_file", {{"path", "big.txt"}, {"offset", 9000}});
        expect(r.ok && r.text.find("past the end") != std::string::npos, "an offset past the end says how long the file is");
        write_file(ws / "README.md", "x");
        r = tool(h, "read_file", {{"path", "readme.txt"}});
        expect(!r.ok && r.text.find("Did you mean") != std::string::npos && r.text.find("README.md") != std::string::npos, "a missing file suggests a sibling");
        expect(tool(h, "list_dir", {{"path", "."}}).text.find("entries)") != std::string::npos, "list_dir ends with a count");
        expect(!tool(h, "read_file", {{"path", "adir"}}).ok, "read_file on a directory says to use list_dir");
        write_file(ws / "e.txt", "alpha\n    beta\ngamma\n");
        r = tool(h, "edit_file", {{"path", "e.txt"}, {"old_string", "beta"}, {"new_string", "beta"}});
        expect(!r.ok && r.text.find("identical") != std::string::npos, "identical strings are rejected");
        r = tool(h, "edit_file", {{"path", "e.txt"}, {"old_string", ""}, {"new_string", "x"}});
        expect(!r.ok && r.text.find("write_file") != std::string::npos, "an empty old_string points at write_file");
        r = tool(h, "edit_file", {{"path", "e.txt"}, {"old_string", "beta"}, {"new_string", "delta\nepsilon"}});
        expect(r.ok && r.text.find("(+2 -1)") != std::string::npos, "an edit reports added and removed lines");
        r = tool(h, "edit_file", {{"path", "e.txt"}, {"old_string", "delta\nepsilon"}, {"new_string", "beta"}});
        expect(r.ok, "and back again");
        r = tool(h, "edit_file", {{"path", "e.txt"}, {"old_string", "beta"}, {"new_string", "BETA"}});
        expect(r.ok && read_whole_text(ws / "e.txt") == "alpha\n    BETA\ngamma\n", "an indentation-mismatched old_string matches by trimmed lines and the replacement is re-indented");
        write_file(ws / "r.txt", "a b a b\n");
        r = tool(h, "edit_file", {{"path", "r.txt"}, {"old_string", "a"}, {"new_string", "z"}});
        expect(!r.ok && r.text.find("replace_all") != std::string::npos, "an ambiguous old_string mentions replace_all");
        r = tool(h, "edit_file", {{"path", "r.txt"}, {"old_string", "a"}, {"new_string", "z"}, {"replace_all", true}});
        expect(r.ok && read_whole_text(ws / "r.txt") == "z b z b\n" && r.text.find("2 occurrences") != std::string::npos, "replace_all replaces every occurrence");
        write_file(ws / "crlf.txt", "one\r\ntwo\r\n");
        r = tool(h, "edit_file", {{"path", "crlf.txt"}, {"old_string", "two"}, {"new_string", "TWO"}});
        expect(r.ok && read_whole_text(ws / "crlf.txt") == "one\r\nTWO\r\n", "CRLF files keep their line endings");
        r = tool(h, "write_file", {{"path", "e.txt"}, {"content", "just one line\n"}});
        expect(r.ok && r.text.find("overwrote") != std::string::npos && r.text.find("(+1 -3)") != std::string::npos, "write_file reports the line delta");
        r = tool(h, "run_shell", {{"command", "true"}});
        expect(r.ok && r.text.find("(no output)") != std::string::npos, "a silent command says so");
        r = tool(h, "run_shell", {{"command", "sleep 5"}, {"timeout_seconds", 1}});
        expect(!r.ok && r.text.find("retry with a larger timeout_seconds") != std::string::npos, "a timeout suggests a larger timeout");
        expect(canonical_tool_name("Read_File") == "read_file" && canonical_tool_name("readFile") == "read_file" && canonical_tool_name("list-dir") == "list_dir" &&
               canonical_tool_name("nope").empty(), "tool names are repaired from common misspellings");
    }

    section("approval previews and workdir");
    {
        write_file(ws / "p.txt", "one\ntwo\nthree\n");
        std::string pv = tool_preview(h, "edit_file", {{"path", "p.txt"}, {"old_string", "two"}, {"new_string", "TWO\n2.5"}});
        expect(pv == "- two\n+ TWO\n+ 2.5\n", "edit_file preview shows removed and added lines: " + pv);
        pv = tool_preview(h, "write_file", {{"path", "p.txt"}, {"content", "one\nzwei\nthree\n"}});
        expect(pv.find("replaces 3 lines with 3") == 0 && pv.find("- two\n+ zwei") != std::string::npos, "write_file preview diffs against the existing file");
        pv = tool_preview(h, "write_file", {{"path", "new.txt"}, {"content", "a\nb\n"}});
        expect(pv.find("new file, 2 lines") == 0 && pv.find("+ a\n+ b") != std::string::npos, "a new file previews its first lines");
        expect(tool_preview(h, "run_shell", {{"command", "ls"}}).empty(), "commands have no preview");
        fs::create_directories(ws / "sub");
        write_file(ws / "sub" / "here.txt", "x");
        auto r = tool(h, "run_shell", {{"command", "ls"}, {"workdir", "sub"}});
        expect(r.ok && r.text.find("here.txt") != std::string::npos, "workdir runs the command in that directory");
        r = tool(h, "run_shell", {{"command", "ls"}, {"workdir", "nope"}});
        expect(!r.ok && r.text.find("not a directory") != std::string::npos, "a missing workdir is an error, not a silent fallback");
        auto act = tool_action(h, "run_shell", {{"command", "ls"}, {"workdir", "sub"}});
        expect(act.workdir == fs::weakly_canonical(ws / "sub"), "tool_action resolves the workdir for the harness");
    }

    section("search_files");
    write_file(ws / "minified.js", std::string(3 * 1024 * 1024, 'x') + "chat log\n");  // crashed MAIC via std::regex
    write_file(ws / "normal.txt", "a chat about a log\n");
    auto sr = tool(h, "search_files", {{"pattern", "chat.*log"}, {"path", "."}});
    expect(sr.ok && sr.text.find("normal.txt") != std::string::npos, "survives a 3 MB line and still finds matches");
    sr = tool(h, "search_files", {{"pattern", "(a|aa)+$"}, {"path", "."}});
    expect(true, "catastrophic-backtracking pattern returns");
    expect(!tool(h, "search_files", {{"pattern", "([unclosed"}}).ok, "invalid regex -> error");
    fs::create_directory_symlink(ws, ws / "loop");
    expect(tool(h, "search_files", {{"pattern", "chat"}}).ok, "a symlink loop doesn't hang the search");
    write_file(ws / "noperm" / "secret.txt", "chat log");
    fs::permissions(ws / "noperm", fs::perms::none);
    expect(tool(h, "search_files", {{"pattern", "chat"}}).ok, "an unreadable directory is skipped");
    fs::permissions(ws / "noperm", fs::perms::owner_all);
    write_file(ws / "ünïcödé 名前.txt", "chat log");
    expect(tool(h, "search_files", {{"pattern", "chat"}}).text.find("名前") != std::string::npos, "unicode file names come through");

    section("glob");
    for (const char* f : {"src/a.cpp", "src/a.h", "src/deep/b.cpp", "src/deep/er/c.cpp", "build/x.cpp", ".git/config", "node_modules/m.cpp", "README.md"}) {
        write_file(ws / "g" / f, "x");
    }
    auto g = tool(h, "glob", {{"pattern", "*.cpp"}, {"path", "g"}});
    expect(g.ok && g.text == "src/a.cpp\nsrc/deep/b.cpp\nsrc/deep/er/c.cpp\n(3 files)", "a pattern without / matches at any depth, sorted, skipping build and node_modules:\n" + g.text);
    g = tool(h, "glob", {{"pattern", "src/*.cpp"}, {"path", "g"}});
    expect(g.ok && g.text == "src/a.cpp\n(1 file)", "* stays inside one segment: " + g.text);
    g = tool(h, "glob", {{"pattern", "src/**/*.cpp"}, {"path", "g"}});
    expect(g.ok && g.text.find("src/a.cpp") != std::string::npos && g.text.find("src/deep/er/c.cpp") != std::string::npos && g.text.find("(3 files)") != std::string::npos,
           "** spans zero or more directories: " + g.text);
    g = tool(h, "glob", {{"pattern", "src/?.h"}, {"path", "g"}});
    expect(g.ok && g.text == "src/a.h\n(1 file)", "? matches one character: " + g.text);
    g = tool(h, "glob", {{"pattern", "**/config"}, {"path", "g"}});
    expect(g.ok && g.text.find("no files match") == 0, ".git is skipped: " + g.text);
    g = tool(h, "glob", {{"pattern", "*.[ch]pp"}, {"path", "g/src"}});
    expect(g.ok && g.text.find("a.cpp") != std::string::npos && g.text.find("a.h") == std::string::npos, "character classes work: " + g.text);
    expect(!tool(h, "glob", {{"pattern", "*"}, {"path", "g/nowhere"}}).ok, "a missing directory -> error");
    expect(!tool(h, "glob", {{"pattern", ""}, {"path", "g"}}).ok, "an empty pattern -> error");
    g = tool(h, "glob", {{"pattern", "R(EAD)ME.md"}, {"path", "g"}});
    expect(g.ok && g.text.find("no files match") == 0, "regex characters in a pattern are literal");
    if (fs::is_directory(home / ".ssh")) {
        g = tool(h, "glob", {{"pattern", "*"}, {"path", (home / ".ssh").string()}});
        expect(g.ok && g.text.find("no files match") == 0, "secret directories yield nothing, not even names");
    }
    for (int i = 0; i < 510; ++i) write_file(ws / "g" / "many" / ("f" + std::to_string(i) + ".txt"), "");
    g = tool(h, "glob", {{"pattern", "many/*.txt"}, {"path", "g"}});
    expect(g.ok && std::count(g.text.begin(), g.text.end(), '\n') == 501 && g.text.find("stopped at 500") != std::string::npos, "results are capped at 500 with a note");
    expect(tool_action(h, "glob", {{"pattern", "*.cpp"}}).kind == Action::Kind::Read && tool_action(h, "glob", {{"pattern", "*.cpp"}}).path == h.resolve("."),
           "the harness sees glob as a read of the directory");

    section("sandbox under stress");
    auto t0 = std::chrono::steady_clock::now();
    auto r = run_sandboxed("yes | head -c 50000000", ws, false, std::chrono::seconds(30), no_cancel);
    expect(r.exit_code == 0 && r.output.size() < 64 * 1024, "50 MB of output is capped, not buffered whole");
    static const char kNul[] = "echo a\0b";
    r = run_sandboxed(std::string(kNul, sizeof(kNul) - 1), ws, false, std::chrono::seconds(10), no_cancel);
    expect(true, "a command with an embedded NUL returns");
    r = run_sandboxed("sleep 5 & sleep 5 & wait", ws, false, std::chrono::seconds(1), no_cancel);
    expect(r.timed_out, "background children are killed at the timeout too");
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    t0 = std::chrono::steady_clock::now();
    r = run_sandboxed("sleep 30", ws, false, std::chrono::seconds(60), cancel);
    canceller.join();
    expect(r.cancelled && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3), "cancel stops a command within a second");

    section("harness on odd commands");
    expect(!is_read_only_command(""), "empty command is not read-only");
    expect(!is_read_only_command(" ; ; && "), "separators only is not read-only");
    expect(!is_read_only_command(std::string(100000, '|')), "100k pipes returns (not read-only)");
    auto d = h.check({Action::Kind::Shell, {}, std::string(1 << 20, 'a')}, Mode::Auto, Origin::Local);
    expect(d.verdict == Verdict::Deny, "a 1 MB command is denied as too long to vet");

    section("session log");
    {
        SessionLog log("robustness-test");
        log.write("tool", {{"result", std::string("bad utf8 \xff\xfe here")}});
        struct stat st{};
        stat(log.path().c_str(), &st);
        expect((st.st_mode & 0777) == 0600, "session log is created 0600");
        std::ifstream in(log.path());
        std::string line;
        std::getline(in, line);
        expect(json::parse(line, nullptr, false).is_object(), "invalid UTF-8 is written as valid JSON");
        fs::remove(log.path());
    }

    section("markdown export");
    {
        SessionInfo info;
        info.id = "20260101-000000-tui-1";
        info.title = "Fix the thing";
        info.workspace = "/w";
        info.started = "20260101-000000";
        LoadedSession ls;
        ls.model = "m";
        ls.transcript = {{"user", "hi"}, {"tool_call", "read_file a"}, {"tool_result", "1\tx"}, {"assistant", "**done**"}};
        std::string md = export_markdown(info, ls);
        expect(md.rfind("# Fix the thing\n", 0) == 0 && md.find("## User\n\nhi") != std::string::npos && md.find("## Assistant\n\n**done**") != std::string::npos &&
                   md.find("**Tool:** `read_file a`") != std::string::npos && md.find("```\n1\tx\n```") != std::string::npos,
               "the export has a title, sections and fenced tool output");
        expect(export_markdown(info, ls, false).find("**Tool:**") == std::string::npos, "tool details can be left out");
    }

    section("artifact cleaning");
    fs::path victim = home / ".cache" / "maic-robustness-victim";
    fs::remove_all(victim);
    write_file(victim / "keep.txt", "must survive");
    fs::path art = ws / "artifact";
    write_file(art / "old.txt", "x");
    write_file(art / "sub" / "new.txt", "y");
    fs::create_directory_symlink(victim, art / "link-to-victim");
    fs::last_write_time(art / "old.txt", fs::file_time_type::clock::now() - std::chrono::hours(24 * 40));
    Artifact a{"test", "a", "", art};
    auto removed = clean(a, std::chrono::hours(24 * 30));
    expect(removed.files == 1 && !fs::exists(art / "old.txt") && fs::exists(art / "sub" / "new.txt"), "--older-than only removes old files");
    removed = clean(a);
    expect(fs::exists(victim / "keep.txt"), "cleaning never follows a symlink out of the artifact");
    expect(fs::exists(art) && fs::is_empty(art), "the artifact directory itself stays, emptied");
    threw = false;
    try {
        clean({"test", "home", "", home});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "refuses to clean $HOME itself");
    threw = false;
    try {
        clean({"test", "etc", "", "/etc"});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "refuses to clean outside $HOME");
    fs::remove_all(victim);

    section("layered settings and session homes");
    {
        // ws is under $HOME (~/.cache/...), so project layers apply. Global file untouched: use XDG_CONFIG_HOME.
        fs::path cfg = ws / "xdg";
        write_file(cfg / "maic" / "settings.json", R"({"model": "global/model", "mode": "manual", "style": {"user": {"fg": "red"}}})");
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        fs::path proj = ws / "proj";
        write_file(proj / ".maic" / "settings.json", R"({"mode": "edit", "providers": {"lab": {"kind": "openai", "base_url": "http://127.0.0.1:9/v1"}}})");
        write_file(proj / ".maic" / "settings.local.json", R"({"model": "local/model", "style": {"user": {"bold": true}}})");
        Settings s = load_settings(proj);
        expect(s.sources.size() == 3, "three files read: global, project, project-local");
        expect(s.model == "local/model" && s.mode == "edit", "nearer files win for scalars");
        bool lab = false;
        for (const auto& p : s.providers) lab = lab || (p.name == "lab" && p.kind == "openai");
        expect(lab && s.providers.size() == default_providers().size() + 1, "providers merge by name");
        expect(s.style("user").fg == "red" && s.style("user").bold, "styles merge across layers");
        expect(resolve_sessions_home(s, proj).filename() == "general", "no MAIC.md: auto resolves to general");
        write_file(proj / "MAIC.md", "# proj\n");
        expect(resolve_sessions_home(s, proj).parent_path().filename() == "projects", "a MAIC.md moves auto to the project home");
        s.sessions_home = "general";
        expect(resolve_sessions_home(s, proj).filename() == "general", "an explicit general overrides the MAIC.md");
        fs::path sub = proj / "sub";
        fs::create_directories(sub);
        Settings inner = load_settings(sub);
        expect(inner.mode == "edit" && resolve_sessions_home(inner, sub).parent_path().filename() == "projects",
               "a subdirectory inherits the project's settings and home");
        write_file(proj / ".maic" / "settings.json", "{ this is not json");
        threw = false;
        try {
            load_settings(proj);
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a broken settings file throws instead of silently using defaults");
        // A settings.lua beside the json wins, and it is code.
        write_file(proj / ".maic" / "settings.lua", "return { mode = os.getenv('HOME') and 'plan' or 'manual', leader = ',', instruction_files = {'A.md','B.md'}, style = { user = { fg = 'blue' } } }");
        Settings ls = load_settings(proj);
        expect(ls.mode == "plan" && ls.leader == "," && ls.instruction_files.size() == 2 && ls.instruction_files[1] == "B.md", "settings.lua is evaluated as code, arrays included");
        expect(ls.style("user").fg == "blue" && ls.style("user").bold, "lua styles merge over earlier layers");
        bool lua_seen = false;
        for (const auto& src : ls.sources) lua_seen = lua_seen || src.extension() == ".lua";
        expect(lua_seen, ":settings lists the lua file that was read");
        write_file(proj / ".maic" / "settings.lua", "return 42");
        threw = false;
        try {
            load_settings(proj);
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a settings.lua that does not return a table is an error");
        fs::remove(proj / ".maic" / "settings.lua");
        Lua lt(ws);
        auto j = lt.eval_table("return { a = 1, b = 'x', c = { 1, 2, 3 }, d = { k = true }, e = 1.5 }");
        expect(j["a"] == 1 && j["b"] == "x" && j["c"].is_array() && j["c"].size() == 3 && j["d"]["k"] == true && j["e"] == 1.5, "eval_table converts scalars, arrays and nested tables");
        unsetenv("XDG_CONFIG_HOME");
    }

    section("lua");
    {
        std::vector<std::string> notices;
        Lua lua(ws, [&](const std::string& t) { notices.push_back(t); });
        auto r = lua.run("print('hi', 2+2) return 'ret'");
        expect(r.ok && r.output == "hi\t4\nret\n", "print is captured and returned values are shown: " + r.output);
        r = lua.run("x = 41");
        r = lua.run("return x + 1");
        expect(r.ok && r.output == "42\n", "globals persist between runs");
        r = lua.run("error('boom')");
        expect(!r.ok && r.output.find("boom") != std::string::npos, "errors come back as text");
        r = lua.run("this is not lua");
        expect(!r.ok, "a syntax error is reported, not fatal");
        write_file(ws / "lua.txt", "from file");
        r = lua.run("return maic.read('lua.txt')");
        expect(r.ok && r.output == "from file\n", "maic.read is relative to the workspace");
        r = lua.run("maic.write('out/made.txt', 'made by lua')");
        expect(r.ok && read_whole_text(ws / "out" / "made.txt") == "made by lua", "maic.write creates directories and files");
        r = lua.run("local out, rc = maic.shell('echo shell-ok; exit 3') return out, rc");
        expect(r.ok && r.output == "shell-ok\n\n3\n", "maic.shell returns output and exit code: " + r.output);
        r = lua.run("maic.notice('note') return maic.workspace");
        expect(r.ok && notices.size() == 1 && notices[0] == "note" && r.output == ws.string() + "\n", "maic.notice and maic.workspace");
        write_file(ws / "script.lua", "#!/usr/bin/env maic lua\nprint(#arg)\n");
        lua.run("arg = {'a','b'}");
        r = lua.run_file(ws / "script.lua");
        expect(r.ok && r.output == "2\n", "run_file skips a shebang and sees arg");
        r = lua.run("local t = {} for i = 1, 1e6 do t[i] = i end return #t");
        expect(r.ok && r.output == "1000000\n", "LuaJIT runs a real loop");
    }

    section("vendor manifest and adopt");
    {
        auto entries = load_vendor_manifest();
        expect(entries.size() >= 3, "the manifest lists the vendored services");
        auto comfy = find_vendor("comfyui");
        expect(comfy && comfy->kind == "submodule" && comfy->ref == "v0.38.0" && comfy->path == "vendor/ComfyUI", "comfyui is a submodule pinned to a release tag");
        auto oll = find_vendor("ollama");
        expect(oll && oll->kind == "release" && !oll->checksums.empty() && oll->url.find("${VERSION}") != std::string::npos, "ollama is a checksum-verified release");
        // Adopt into a throwaway state directory, never the real one.
        fs::path state = ws / "xdg-state";
        setenv("XDG_STATE_HOME", state.c_str(), 1);
        fs::path fake_ollama = ws / "fake-ollama" / "v9";
        write_file(fake_ollama / "bin" / "ollama", "#!/bin/sh\necho ollama version is 9\n");
        fs::permissions(fake_ollama / "bin" / "ollama", fs::perms::owner_all);
        VendorEntry e = *oll;
        e.install.clear();  // no script: adopt only links
        vendor_adopt(e, fake_ollama);
        auto st = vendor_status(e);
        expect(st.linked && st.installed && fs::path(st.target) == fs::weakly_canonical(fake_ollama), "adopt links current at the given install: " + st.target);
        expect(fs::is_symlink(vendor_link(e)) && vendor_link(e).parent_path() == state / "maic" / "vendor" / "ollama", "the link lives under <state>/vendor/ollama/current");
        threw = false;
        try {
            vendor_adopt(e, ws / "fake-ollama");  // the parent, without bin/ollama
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "adopting a directory without bin/ollama is refused");
        VendorEntry c = *comfy;
        c.install.clear();
        threw = false;
        try {
            vendor_adopt(c, ws / "fake-ollama");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "adopting a non-ComfyUI directory as comfyui is refused");
        vendor_unlink(e);
        expect(!vendor_status(e).linked, "unlink removes the link");
        expect(expand_vars("${MAIC_VENDOR}/x") == (state / "maic" / "vendor" / "x").string() && expand_vars("${MAIC_STATE}") == (state / "maic").string(),
               "service files can use ${MAIC_VENDOR} and ${MAIC_STATE}");
        unsetenv("XDG_STATE_HOME");
    }

    section("instructions");
    write_file(ws / "MAIC.md", std::string(100 * 1024, 'x'));
    auto files = load_instructions(ws);
    bool found = false;
    for (const auto& f : files) {
        if (f.path == ws / "MAIC.md") {
            found = true;
            expect(f.text.size() < 34 * 1024, "a 100 KB MAIC.md is truncated to 32 KB");
        }
    }
    expect(found, "the workspace MAIC.md is loaded");
    expect(load_instructions("/tmp").size() <= 1 + 2, "a workspace outside $HOME only reads its own directory");

    fs::permissions(ws / "noperm", fs::perms::owner_all);
    fs::remove_all(ws);
    return finish();
}
