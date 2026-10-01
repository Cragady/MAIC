// Hostile and broken inputs must give an error result, never a crash or a hang.
// Add a case here for every crash found in the wild (the first one: std::regex overflowing the stack).
#include "check.hpp"

#include "maic/artifacts.hpp"
#include "maic/harness.hpp"
#include "maic/instructions.hpp"
#include "maic/sandbox.hpp"
#include "maic/session.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/tools.hpp"
#include "maic/vendor.hpp"

#include "maic/http.hpp"
#include "maic/bans.hpp"
#include "maic/lua.hpp"
#include "maic/places.hpp"
#include "maic/tripwire.hpp"
#include "maic/paths.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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

// A loopback port nothing listens on.
int closed_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    close(fd);
    return ntohs(a.sin_port);
}

std::string read_whole_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    setenv("MAIC_TRIPWIRE_FILE", ("/tmp/maic-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);  // never the machine's lock
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
        tool_actions(h, "read_file", {{"path", nullptr}});
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "tool_actions with a null path throws (the agent reports it) instead of crashing");

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
        auto act = tool_actions(h, "run_shell", {{"command", "ls"}, {"workdir", "sub"}});
        expect(act.size() == 1 && act[0].workdir == fs::weakly_canonical(ws / "sub"), "tool_actions resolves the workdir for the harness");
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
    expect(tool_actions(h, "glob", {{"pattern", "*.cpp"}})[0].kind == Action::Kind::Read && tool_actions(h, "glob", {{"pattern", "*.cpp"}})[0].path == h.resolve("."),
           "the harness sees glob as a read of the directory");

    section("read_file grep");
    {
        write_file(ws / "grep.txt", "alpha one\nbeta two\nalpha three\ngamma\n");
        auto r = tool(h, "read_file", {{"path", "grep.txt"}, {"grep", "^alpha"}});
        expect(r.ok && r.text == "1\talpha one\n3\talpha three\n(2 matching lines; the file has 4 lines)\n", "grep returns only matching lines with their numbers:\n" + r.text);
        r = tool(h, "read_file", {{"path", "grep.txt"}, {"grep", "zeta"}});
        expect(r.ok && r.text.find("no lines match /zeta/") == 1 && r.text.find("4 lines") != std::string::npos, "no match says so and how long the file is: " + r.text);
        r = tool(h, "read_file", {{"path", "grep.txt"}, {"grep", "alpha"}, {"offset", 2}});
        expect(r.ok && r.text.find("1\t") == std::string::npos && r.text.find("3\talpha three") == 0, "offset applies before matching");
        r = tool(h, "read_file", {{"path", "grep.txt"}, {"grep", "([bad"}});
        expect(!r.ok && r.text.find("bad grep regex") == 0, "an invalid grep regex is an error");
        r = tool(h, "read_file", {{"path", "big.txt"}, {"grep", "line"}, {"limit", 3}});
        expect(r.ok && r.text.find("Showing 3 matching lines up to line 3 of 2500. Use offset=4") != std::string::npos, "a capped grep says where to continue: " + r.text);
    }

    section("list_dir depth");
    {
        for (const char* f : {"one/a.txt", "one/two/b.txt", "one/two/three/c.txt", "one/two/three/four/d.txt", "one/build/x.o", "top.txt"}) write_file(ws / "tree" / f, "x");
        auto r = tool(h, "list_dir", {{"path", "tree"}});
        expect(r.ok && r.text == "one/ (3 entries)\ntop.txt\n(2 entries)", "depth 1 shows entries with directory counts:\n" + r.text);
        r = tool(h, "list_dir", {{"path", "tree"}, {"depth", 2}});
        expect(r.ok && r.text.find("one/ (3 entries)\n  a.txt\n  build/ (1 entry)\n  two/ (2 entries)\ntop.txt\n(2 entries, 5 to depth 2)") == 0 && r.text.find("b.txt") == std::string::npos, "depth 2 indents one level and stops there:\n" + r.text);
        r = tool(h, "list_dir", {{"path", "tree"}, {"depth", 9}});
        expect(r.ok && r.text.find("    three/ (2 entries)\n      c.txt\n      four/ (1 entry)\n") != std::string::npos && r.text.find("d.txt") == std::string::npos && r.text.find("x.o") == std::string::npos &&
                   r.text.find("to depth 4)") != std::string::npos,
               "depth is capped at 4 and build/ is not expanded:\n" + r.text);
        expect(tool_actions(h, "list_dir", {{"path", "tree"}, {"depth", 3}})[0].kind == Action::Kind::Read, "a deep listing is still one read");
    }

    section("multi_edit");
    {
        write_file(ws / "m.txt", "one\ntwo\nthree\n");
        auto r = tool(h, "multi_edit", {{"path", "m.txt"}, {"edits", {{{"old_string", "one"}, {"new_string", "1\n1.5"}}, {{"old_string", "three"}, {"new_string", "3"}}}}});
        expect(r.ok && read_whole_text(ws / "m.txt") == "1\n1.5\ntwo\n3\n" && r.text.find("2 edits: 1 (+2 -1), 2 (+1 -1)") != std::string::npos, "edits apply in order and the result names each delta: " + r.text);
        r = tool(h, "multi_edit", {{"path", "m.txt"}, {"edits", {{{"old_string", "two"}, {"new_string", "2"}}, {{"old_string", "nope"}, {"new_string", "x"}}}}});
        expect(!r.ok && read_whole_text(ws / "m.txt") == "1\n1.5\ntwo\n3\n" && r.text.find("edit 2 of 2: old_string not found") == 0 && r.text.find("Nothing was written") != std::string::npos,
               "one failing edit means nothing is written and the result names it: " + r.text);
        r = tool(h, "multi_edit", {{"path", "m.txt"}, {"edits", {{{"old_string", "1"}, {"new_string", "one"}, {"replace_all", true}}, {{"old_string", "one\none.5"}, {"new_string", "uno"}}}}});
        expect(r.ok && read_whole_text(ws / "m.txt") == "uno\ntwo\n3\n" && r.text.find("1 (2 occurrences, +2 -2)") != std::string::npos, "a later edit sees the earlier one's result: " + r.text);
        r = tool(h, "multi_edit", {{"path", "m.txt"}, {"edits", json::array()}});
        expect(!r.ok && r.text.find("non-empty array") != std::string::npos, "an empty edit list is an error");
        r = tool(h, "multi_edit", {{"path", "m.txt"}, {"edits", {{{"old_string", "two"}}}}});
        expect(!r.ok && r.text.find("edit 1 of 1 needs old_string and new_string") == 0, "a malformed edit is named");
        write_file(ws / "mc.txt", "a\r\nb\r\n");
        r = tool(h, "multi_edit", {{"path", "mc.txt"}, {"edits", {{{"old_string", "a"}, {"new_string", "A"}}, {{"old_string", "b"}, {"new_string", "B"}}}}});
        expect(r.ok && read_whole_text(ws / "mc.txt") == "A\r\nB\r\n", "CRLF is kept through a multi_edit");
        std::string pv = tool_preview(h, "multi_edit", {{"path", "m.txt"}, {"edits", {{{"old_string", "uno"}, {"new_string", "one"}}, {{"old_string", "two"}, {"new_string", "2"}}}}});
        expect(pv == "edit 1 of 2:\n- uno\n+ one\nedit 2 of 2:\n- two\n+ 2\n", "the preview shows every edit: " + pv);
        expect(tool_summary("multi_edit", {{"path", "m.txt"}, {"edits", {1, 2, 3}}}) == "multi_edit m.txt (3 edits)", "the summary counts the edits");
    }

    section("apply_patch");
    {
        write_file(ws / "pa.txt", "one\ntwo\nthree\nfour\n");
        write_file(ws / "pb.txt", "x\ny\n");
        std::string patch =
            "--- a/pa.txt\n+++ b/pa.txt\n@@ -1,4 +1,4 @@\n one\n-two\n+TWO\n three\n four\n"
            "--- pb.txt\n+++ pb.txt\n@@ -1,2 +1,3 @@\n x\n+between\n y\n";
        auto acts = tool_actions(h, "apply_patch", {{"patch", patch}});
        expect(acts.size() == 2 && acts[0].kind == Action::Kind::Write && acts[0].path == h.resolve("pa.txt") && acts[1].path == h.resolve("pb.txt"), "one write action per file in the patch");
        auto r = tool(h, "apply_patch", {{"patch", patch}});
        expect(r.ok && read_whole_text(ws / "pa.txt") == "one\nTWO\nthree\nfour\n" && read_whole_text(ws / "pb.txt") == "x\nbetween\ny\n", "both files are patched");
        expect(r.text == "applied the patch: " + h.resolve("pa.txt").string() + " (1 hunk, +1 -1), " + h.resolve("pb.txt").string() + " (1 hunk, +1 -0)", "the result lists files and hunks: " + r.text);
        // A hunk that does not match applies nothing, in any file.
        std::string bad = "--- pb.txt\n+++ pb.txt\n@@ -1,1 +1,1 @@\n-x\n+X\n--- pa.txt\n+++ pa.txt\n@@ -2,1 +2,1 @@\n-nope\n+yes\n";
        r = tool(h, "apply_patch", {{"patch", bad}});
        expect(!r.ok && read_whole_text(ws / "pb.txt") == "x\nbetween\ny\n" && r.text.find("hunk 1 of " + h.resolve("pa.txt").string() + " (patch line 8) does not match: at line 2 the file has \"TWO\" but the patch expects \"nope\"") == 0 &&
                   r.text.find("Nothing was applied") != std::string::npos,
               "a failed hunk names the file and line and nothing is written: " + r.text);
        // Wrong line numbers with exact context still land; loose context does not.
        r = tool(h, "apply_patch", {{"patch", "--- pa.txt\n+++ pa.txt\n@@ -30,2 +30,2 @@\n three\n-four\n+FOUR\n"}});
        expect(r.ok && read_whole_text(ws / "pa.txt") == "one\nTWO\nthree\nFOUR\n", "a hunk with a wrong line number is placed by its exact context");
        r = tool(h, "apply_patch", {{"patch", "--- pa.txt\n+++ pa.txt\n@@ -3,2 +3,2 @@\n  three\n-FOUR\n+4\n"}});
        expect(!r.ok && read_whole_text(ws / "pa.txt") == "one\nTWO\nthree\nFOUR\n", "context with different whitespace does not match (fuzz 0)");
        // Create and delete, CRLF, the no-newline marker, and git's extra header lines.
        r = tool(h, "apply_patch", {{"patch", "diff --git a/new.txt b/new.txt\nnew file mode 100644\nindex 0000000..1111111\n--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1,2 @@\n+hello\n+world\n"}});
        expect(r.ok && read_whole_text(ws / "new.txt") == "hello\nworld\n" && r.text.find("(created, +2 -0)") != std::string::npos, "--- /dev/null creates a file: " + r.text);
        r = tool(h, "apply_patch", {{"patch", "--- /dev/null\n+++ new.txt\n@@ -0,0 +1 @@\n+again\n"}});
        expect(!r.ok && r.text.find("already exists") != std::string::npos, "creating an existing file is refused");
        r = tool(h, "apply_patch", {{"patch", "--- new.txt\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-hello\n-world\n"}});
        expect(r.ok && !fs::exists(ws / "new.txt") && r.text.find("(deleted)") != std::string::npos, "+++ /dev/null deletes it");
        write_file(ws / "pc.txt", "a\r\nb\r\n");
        r = tool(h, "apply_patch", {{"patch", "--- pc.txt\r\n+++ pc.txt\r\n@@ -1,2 +1,2 @@\r\n a\r\n-b\r\n+B\r\n"}});
        expect(r.ok && read_whole_text(ws / "pc.txt") == "a\r\nB\r\n", "CRLF files stay CRLF, and a CRLF patch is fine");
        r = tool(h, "apply_patch", {{"patch", "--- pc.txt\n+++ pc.txt\n@@ -2,1 +2,1 @@\n-B\n+end\n\\ No newline at end of file\n"}});
        expect(r.ok && read_whole_text(ws / "pc.txt") == "a\r\nend", "the no-newline marker is honoured");
        r = tool(h, "apply_patch", {{"patch", "--- pa.txt\n+++ pa.txt\n@@ -9,1 +9,1 @@\n-nine\n+9\n"}});
        expect(!r.ok && r.text.find("does not match: the file has only 4 lines") != std::string::npos, "a hunk past the end says how long the file is: " + r.text);
        r = tool(h, "apply_patch", {{"patch", "just change two to TWO please"}});
        expect(!r.ok && r.text.find("no '--- old' / '+++ new' file headers") != std::string::npos, "prose instead of a diff is explained");
        bool threw2 = false;
        try {
            tool_actions(h, "apply_patch", {{"patch", "@@ -1 +1 @@\n-x\n+y\n"}});
        } catch (const std::exception& e) {
            threw2 = std::string(e.what()).find("patch line 1: a hunk before any") == 0;
        }
        expect(threw2, "a hunk without a file header fails at tool_actions, before anything is judged or run");
        r = tool(h, "apply_patch", {{"patch", "--- old.txt\n+++ renamed.txt\n@@ -1 +1 @@\n-x\n+y\n"}});
        expect(!r.ok && r.text.find("does not rename") != std::string::npos && r.text.find("move_file") != std::string::npos, "a rename diff points at move_file");
        expect(tool_summary("apply_patch", {{"patch", patch}}) == "apply_patch pa.txt, pb.txt", "the summary names the files");
        expect(tool_preview(h, "apply_patch", {{"patch", patch}}).find("--- a/pa.txt\n+++ b/pa.txt\n@@ -1,4 +1,4 @@\n one\n-two\n+TWO\n") == 0, "the preview is the patch");
    }

    section("move, copy, delete, make_dir");
    {
        write_file(ws / "mv" / "a.txt", "content\n");
        auto r = tool(h, "move_file", {{"from", "mv/a.txt"}, {"to", "mv/sub/b.txt"}});
        expect(r.ok && !fs::exists(ws / "mv" / "a.txt") && read_whole_text(ws / "mv" / "sub" / "b.txt") == "content\n" && r.text == "moved " + h.resolve("mv/a.txt").string() + " to " + h.resolve("mv/sub/b.txt").string(),
               "move_file renames and creates the parent: " + r.text);
        r = tool(h, "move_file", {{"from", "mv/nope.txt"}, {"to", "mv/x.txt"}});
        expect(!r.ok && r.text.find("no such file or directory") == 0, "moving a missing file is an error");
        write_file(ws / "mv" / "c.txt", "c\n");
        r = tool(h, "move_file", {{"from", "mv/c.txt"}, {"to", "mv/sub/b.txt"}});
        expect(!r.ok && r.text.find("already exists") != std::string::npos && read_whole_text(ws / "mv" / "sub" / "b.txt") == "content\n", "a move never overwrites");
        r = tool(h, "move_file", {{"from", "mv/c.txt"}, {"to", "mv/sub"}});
        expect(!r.ok && r.text.find("give the full new path, like " + h.resolve("mv/sub/name").string()) != std::string::npos, "moving onto a directory explains the full-path rule: " + r.text);
        r = tool(h, "move_file", {{"from", "mv/sub"}, {"to", "mv/moved"}});
        expect(r.ok && read_whole_text(ws / "mv" / "moved" / "b.txt") == "content\n", "directories move too");
        auto acts = tool_actions(h, "move_file", {{"from", "mv/c.txt"}, {"to", "mv/d.txt"}});
        expect(acts.size() == 2 && acts[0].kind == Action::Kind::Write && acts[0].path == h.resolve("mv/c.txt") && acts[1].kind == Action::Kind::Write && acts[1].path == h.resolve("mv/d.txt"), "a move is a write at both ends");
        expect(tool_summary("move_file", {{"from", "a"}, {"to", "b"}}) == "move_file a -> b", "the move summary shows both paths");

        r = tool(h, "copy_file", {{"from", "mv/c.txt"}, {"to", "mv/copy/c2.txt"}});
        expect(r.ok && read_whole_text(ws / "mv" / "c.txt") == "c\n" && read_whole_text(ws / "mv" / "copy" / "c2.txt") == "c\n" && r.text.rfind("copied ", 0) == 0, "copy_file copies and keeps the source: " + r.text);
        r = tool(h, "copy_file", {{"from", "mv/moved"}, {"to", "mv/moved2"}});
        expect(r.ok && read_whole_text(ws / "mv" / "moved2" / "b.txt") == "content\n" && r.text.find("(1 files)") != std::string::npos, "directories copy recursively with a file count: " + r.text);
        r = tool(h, "copy_file", {{"from", "mv/c.txt"}, {"to", "mv/copy/c2.txt"}});
        expect(!r.ok && r.text.find("already exists") != std::string::npos, "a copy never overwrites");
        acts = tool_actions(h, "copy_file", {{"from", "mv/c.txt"}, {"to", "mv/e.txt"}});
        expect(acts.size() == 2 && acts[0].kind == Action::Kind::Read && acts[1].kind == Action::Kind::Write && acts[1].path == h.resolve("mv/e.txt"), "a copy is a read of the source and a write of the destination");

        r = tool(h, "delete_file", {{"path", "mv/copy/c2.txt"}});
        expect(r.ok && !fs::exists(ws / "mv" / "copy" / "c2.txt") && r.text == "deleted " + h.resolve("mv/copy/c2.txt").string(), "delete_file removes a file");
        r = tool(h, "delete_file", {{"path", "mv/copy"}});
        expect(r.ok && !fs::exists(ws / "mv" / "copy") && r.text.find("(empty directory)") != std::string::npos, "and an empty directory");
        r = tool(h, "delete_file", {{"path", "mv/moved2"}});
        expect(!r.ok && fs::exists(ws / "mv" / "moved2" / "b.txt") && r.text.find("is a directory with 1 files and 0 directories inside") != std::string::npos && r.text.find("recursive set to true") != std::string::npos,
               "a directory with contents needs recursive: " + r.text);
        std::string pv = tool_preview(h, "delete_file", {{"path", "mv/moved2"}});
        expect(pv == "deletes a directory with 1 files and 0 directories inside\n", "the preview counts what would go: " + pv);
        pv = tool_preview(h, "delete_file", {{"path", "mv/c.txt"}});
        expect(pv == "deletes 1 lines:\n- c\n", "a file's preview shows its lines: " + pv);
        r = tool(h, "delete_file", {{"path", "mv/moved2"}, {"recursive", true}});
        expect(r.ok && !fs::exists(ws / "mv" / "moved2") && r.text.find("(1 files, 0 directories)") != std::string::npos, "recursive deletes the tree and says how much went: " + r.text);
        r = tool(h, "delete_file", {{"path", "mv/moved2"}});
        expect(!r.ok && r.text.find("no such file or directory") == 0, "deleting a missing path is an error");
        r = tool(h, "delete_file", {{"path", "."}, {"recursive", true}});
        expect(!r.ok && r.text.find("workspace itself") != std::string::npos && fs::exists(ws / "mv" / "c.txt"), "the workspace itself is never deleted");
        expect(tool_summary("delete_file", {{"path", "x"}, {"recursive", true}}) == "delete_file x (recursive)", "the summary shows recursive");

        r = tool(h, "make_dir", {{"path", "mk/deep/er"}});
        expect(r.ok && fs::is_directory(ws / "mk" / "deep" / "er") && r.text.rfind("created ", 0) == 0, "make_dir creates parents");
        r = tool(h, "make_dir", {{"path", "mk/deep/er"}});
        expect(r.ok && r.text.find("already exists") != std::string::npos, "an existing directory is fine");
        r = tool(h, "make_dir", {{"path", "mv/c.txt"}});
        expect(!r.ok && r.text.find("is a file") != std::string::npos, "a file in the way is an error");
        expect(tool_actions(h, "make_dir", {{"path", "mk/x"}})[0].kind == Action::Kind::Write && tool_actions(h, "delete_file", {{"path", "mk"}})[0].kind == Action::Kind::Write, "make_dir and delete_file are writes");
        expect(canonical_tool_name("moveFile") == "move_file" && canonical_tool_name("Apply-Patch") == "apply_patch" && canonical_tool_name("MultiEdit") == "multi_edit", "the new names are repaired too");
    }

    section("the harness judges the new tools like writes");
    {
        auto verdict = [&](const std::string& name, const json& args, Mode mode) {
            std::vector<Verdict> out;
            for (const auto& a : tool_actions(h, name, args)) out.push_back(h.check(a, mode, Origin::Local).verdict);
            return out;
        };
        using V = Verdict;
        expect(verdict("move_file", {{"from", "mv/c.txt"}, {"to", "/tmp/maic-robustness-out.txt"}}, Mode::Auto) == std::vector<V>{V::Allow, V::Ask}, "a move out of the workspace asks on the destination");
        expect(verdict("move_file", {{"from", "/tmp/maic-robustness-out.txt"}, {"to", "mv/in.txt"}}, Mode::Auto) == std::vector<V>{V::Ask, V::Allow}, "a move in from outside asks on the source");
        expect(verdict("copy_file", {{"from", "mv/c.txt"}, {"to", "/tmp/maic-robustness-out.txt"}}, Mode::Auto) == std::vector<V>{V::Allow, V::Ask}, "a copy out of the workspace asks");
        expect(verdict("copy_file", {{"from", "~/.ssh/id_ed25519"}, {"to", "mv/key"}}, Mode::Auto)[0] == V::Deny, "copying a secret is denied as a read");
        expect(verdict("delete_file", {{"path", "/tmp/maic-robustness-out.txt"}}, Mode::Auto) == std::vector<V>{V::Ask}, "a delete outside the workspace asks");
        expect(verdict("delete_file", {{"path", "~/.ssh/known_hosts"}}, Mode::Auto) == std::vector<V>{V::Trip}, "deleting a secret trips");
        expect(verdict("delete_file", {{"path", "/etc/hosts"}}, Mode::Auto) == std::vector<V>{V::Trip}, "deleting a system file trips");
        expect(verdict("make_dir", {{"path", "~/bin/evil"}}, Mode::Auto) == std::vector<V>{V::Ask}, "a directory under a sensitive path asks");
        expect(verdict("multi_edit", {{"path", "mv/c.txt"}, {"edits", json::array()}}, Mode::Plan) == std::vector<V>{V::Deny}, "plan mode denies multi_edit");
        expect(verdict("apply_patch", {{"patch", "--- mv/c.txt\n+++ mv/c.txt\n@@ -1 +1 @@\n-c\n+C\n--- /tmp/maic-robustness-out.txt\n+++ /tmp/maic-robustness-out.txt\n@@ -1 +1 @@\n-a\n+b\n"}}, Mode::Auto) == std::vector<V>{V::Allow, V::Ask},
               "a patch touching a file outside the workspace asks for that file");
        expect(verdict("apply_patch", {{"patch", "--- /etc/passwd\n+++ /etc/passwd\n@@ -1 +1 @@\n-a\n+b\n"}}, Mode::Auto) == std::vector<V>{V::Trip}, "a patch under /etc trips");
        expect(!h.harmless(tool_actions(h, "delete_file", {{"path", "mv/c.txt"}})[0]) && !h.harmless(tool_actions(h, "move_file", {{"from", "a"}, {"to", "b"}})[1]) && h.harmless(tool_actions(h, "copy_file", {{"from", "a"}, {"to", "b"}})[0]),
               "repeats of the new writes are not harmless; the read half of a copy is");
    }

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
        expect(s.context_2 == 8192, "context_2 defaults to 8192");
        write_file(proj / ".maic" / "settings.local.json", R"({"model": "local/model", "context_2": 16384, "style": {"user": {"bold": true}}})");
        expect(load_settings(proj).context_2 == 16384, "context_2 is read from a layer");
        write_file(proj / ".maic" / "settings.local.json", R"({"model": "local/model", "style": {"user": {"bold": true}}})");
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
        expect(entries.size() >= 2, "the manifest lists the vendored services");
        auto comfy = find_vendor("comfyui");
        expect(comfy && comfy->ref == "v0.38.0" && comfy->path == "vendor/ComfyUI", "comfyui is a submodule pinned to a release tag");
        expect(!find_vendor("ollama"), "ollama is no longer vendored");
        auto lc = find_vendor("llamacpp");
        auto release_tag = [](const std::string& ref) {  // b<number>, llama.cpp's release tags
            return ref.size() > 1 && ref[0] == 'b' && ref.find_first_not_of("0123456789", 1) == std::string::npos;
        };
        expect(lc && lc->path == "vendor/llama.cpp" && release_tag(lc->ref) && lc->install == "vendor/llamacpp.sh",
               "llamacpp is a submodule pinned to a release tag: " + (lc ? lc->ref : std::string("missing")));
        bool loopback = false;
        for (const auto& p : default_providers()) {
            if (p.name == "llamacpp") loopback = p.kind == "openai" && !p.remote() && p.base_url.rfind("http://127.0.0.1:", 0) == 0;
        }
        expect(loopback, "default providers have llamacpp: OpenAI-compatible on loopback");
        // Adopt into a throwaway state directory, never the real one.
        fs::path state = ws / "xdg-state";
        setenv("XDG_STATE_HOME", state.c_str(), 1);
        fs::path fake_lc = ws / "fake-llama.cpp";
        write_file(fake_lc / "ggml" / "CMakeLists.txt", "# ggml\n");
        (void)!std::system(("git -C '" + fake_lc.string() + "' init -q && git -C '" + fake_lc.string() + "' remote add origin https://example.com/someone/not-the-one").c_str());
        std::stringstream adopt_out;
        auto* real_cout = std::cout.rdbuf(adopt_out.rdbuf());
        VendorEntry e = *lc;
        e.install.clear();  // no script: adopt only links
        vendor_adopt(e, fake_lc);
        std::cout.rdbuf(real_cout);
        expect(adopt_out.str().find("note: its origin is https://example.com/someone/not-the-one, not a llama.cpp repository") != std::string::npos, "adopt reads the git remote and says when it is not the upstream: " + adopt_out.str());
        auto st = vendor_status(e);
        expect(st.linked && st.installed && fs::path(st.target) == fs::weakly_canonical(fake_lc), "adopt links the checkout the user gave: " + st.target);
        expect(fs::is_symlink(vendor_link(e)) && vendor_link(e) == state / "maic" / "vendor" / "llama.cpp", "the link lives at <state>/vendor/llama.cpp");
        threw = false;
        try {
            vendor_adopt(e, ws);  // no ggml/
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "adopting a directory that is not a llama.cpp checkout is refused");
        VendorEntry c = *comfy;
        c.install.clear();
        threw = false;
        try {
            vendor_adopt(c, fake_lc);
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "adopting a non-ComfyUI directory as comfyui is refused");
        vendor_unlink(e);
        expect(!vendor_status(e).linked, "unlink removes the link");
        VendorEntry l = *lc;
        l.install.clear();
        // No models_dir configured (a config dir of our own, not the real one): the root is <state>/models/llamacpp.
        setenv("XDG_CONFIG_HOME", (ws / "xdg-config-empty").c_str(), 1);
        fs::path root = state / "maic" / "models" / "llamacpp";
        expect(llamacpp_models_root() == root && expand_vars("${MAIC_MODELS}/llamacpp") == root.string(), "without models_dir the models root is <state>/models/llamacpp, for the helper and the service alike");
        write_file(root / "notes.txt", "not a model\n");
        write_file(root / "tiny.gguf", "fake\n");
        write_file(root / "sha256-blob", "GGUFxxxx");  // a suffixless GGUF: the format's magic
        threw = false;
        try {
            vendor_use(l, root / "notes.txt");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw && !fs::is_symlink(vendor_model_link(l)), "vendor use refuses a file that is not a GGUF");
        threw = false;
        try {
            vendor_use(l, root / "missing.gguf");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "vendor use refuses a missing path");
        vendor_use(l, root / "tiny.gguf");
        expect(vendor_model_link(l) == state / "maic" / "vendor" / "llamacpp" / "current-model.gguf" && fs::is_symlink(vendor_model_link(l)) &&
                   fs::read_symlink(vendor_model_link(l)) == fs::weakly_canonical(root / "tiny.gguf"),
               "vendor use links current-model.gguf at the file");
        expect(llamacpp_current_id() == "tiny" && resolve_model_alias("llamacpp/current") == "llamacpp/tiny", "and llamacpp/current resolves to its router id");
        vendor_use(l, root / "sha256-blob");
        expect(fs::read_symlink(vendor_model_link(l)).filename() == "sha256-blob", "a suffixless GGUF is accepted by its magic and replaces the link");
        expect(vendor_status(l).model.rfind("sha256-blob", 0) == 0, "vendor status shows the model id");
        threw = false;
        try {
            vendor_use(c, root / "tiny.gguf");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "vendor use is refused for a service that takes no model");
        unsetenv("XDG_CONFIG_HOME");
        expect(expand_vars("${MAIC_VENDOR}/x") == (state / "maic" / "vendor" / "x").string() && expand_vars("${MAIC_STATE}") == (state / "maic").string(),
               "service files can use ${MAIC_VENDOR} and ${MAIC_STATE}");
        unsetenv("XDG_STATE_HOME");
    }

    section("llama.cpp is the default and a failed call says how to start it");
    {
        expect(Settings{}.model == "llamacpp/current", "the default model is llamacpp/current");
        fs::path state = ws / "xdg-state-llamacpp";
        setenv("XDG_STATE_HOME", state.c_str(), 1);
        auto lc = find_vendor("llamacpp");
        const ServiceDef* svc = nullptr;
        auto services = load_services(root_dir() / "services");
        for (const auto& s : services) {
            if (s.name == "llamacpp") svc = &s;
        }
        auto arg = [&](const std::string& a) { return std::find(svc->command.begin(), svc->command.end(), a) != svc->command.end(); };
        auto pair = [&](const std::string& a, const std::string& b) {
            for (size_t i = 0; i + 1 < svc->command.size(); ++i) {
                if (svc->command[i] == a && svc->command[i + 1] == b) return true;
            }
            return false;
        };
        setenv("XDG_CONFIG_HOME", (ws / "xdg-config-empty").c_str(), 1);
        fs::path root = state / "maic" / "models" / "llamacpp";
        expect(pair("--ctx-size", std::getenv("MAIC_CONTEXT") ? std::getenv("MAIC_CONTEXT") : "16384"), "the service takes its context size from ${MAIC_CONTEXT}, 16384 by default");
        expect(svc && svc->port == 8081 && pair("--host", "127.0.0.1") && pair("--port", "8081") && pair("--models-dir", root.string()) && pair("--models-max", "1") && arg("--jinja"),
               "services/llamacpp.json: loopback, port 8081, router mode over the models root, one resident model, jinja templates");
        expect(svc && lc && svc->requires_paths.size() == 1 && svc->requires_paths[0] == root, "the service requires the models root");
        {
            // The side server: the same router over the same GGUFs, on 8082, with its own context variable.
            const ServiceDef* side = nullptr;
            for (const auto& s : services) {
                if (s.name == "llamacpp-2") side = &s;
            }
            auto spair = [&](const std::string& a, const std::string& b) {
                for (size_t i = 0; side && i + 1 < side->command.size(); ++i) {
                    if (side->command[i] == a && side->command[i + 1] == b) return true;
                }
                return false;
            };
            expect(side && side->port == 8082 && spair("--port", "8082") && spair("--models-dir", root.string()) && spair("--models-max", "1") && side->needs_gpu,
                   "services/llamacpp-2.json: port 8082, the same models root, one resident model, marked needs_gpu");
            expect(side && spair("--ctx-size", std::getenv("MAIC_CONTEXT_2") ? std::getenv("MAIC_CONTEXT_2") : "8192"), "the side server takes its context size from ${MAIC_CONTEXT_2}, 8192 by default");
            expect(side && side->requires_paths.size() == 1 && side->requires_paths[0] == root, "the side server requires the same models root");
            expect(is_llama_server("llamacpp") && is_llama_server("llamacpp-2") && !is_llama_server("comfyui"), "both are llama servers, ComfyUI is not");
            unsetenv("MAIC_CONTEXT_2");
            expect(expand_vars("${MAIC_CONTEXT_2}") == "8192", "${MAIC_CONTEXT_2} expands to 8192 when nothing sets it");
            setenv("MAIC_CONTEXT_2", "4096", 1);
            expect(expand_vars("${MAIC_CONTEXT_2}") == "4096" && expand_vars("${MAIC_CONTEXT}") != "4096", "and to the environment's value, independently of ${MAIC_CONTEXT}");
            unsetenv("MAIC_CONTEXT_2");
            ServiceDef sdef = *side;
            sdef.port = closed_port();
            std::string shint = unreachable_hint(Provider{"llamacpp-2", "openai", "http://127.0.0.1:" + std::to_string(sdef.port) + "/v1", "", "", json::object()}, {sdef});
            expect(shint.find("maic up llamacpp-2") != std::string::npos, "a failed call to the side server says how to start it: " + shint);
        }
        // The same definition on a port nothing listens on, so this passes whatever is running on 8081 here.
        ServiceDef def = *svc;
        def.port = closed_port();
        Provider p{"llamacpp", "openai", "http://127.0.0.1:" + std::to_string(def.port) + "/v1", "", "", json::object()};
        std::string hint = unreachable_hint(p, {def});
        expect(hint.find("maic up llamacpp") != std::string::npos && hint.find("maic vendor model llamacpp") != std::string::npos,
               "no models root yet: the hint says to start it and how to get a model: " + hint);
        expect(missing_requirement(def).find("maic vendor model llamacpp") != std::string::npos, "maic up refuses with the same hint while the root is missing");
        fs::create_directories(root);
        hint = unreachable_hint(p, {def});
        expect(missing_requirement(def).empty() && hint.find("maic up llamacpp") != std::string::npos && hint.find("vendor model") == std::string::npos,
               "models root present: the hint is to start it: " + hint);
        unsetenv("XDG_CONFIG_HOME");
        expect(unreachable_hint(Provider{"lab", "openai", "http://127.0.0.1:9/v1", "", "", json::object()}, {def}).empty(), "a provider no service backs gets no hint");
        ChatOptions o;
        o.retries = 0;
        bool transport = false;
        try {
            chat(p, o, {{"user", "hi"}}, json::array(), [](std::string_view, bool) {}, no_cancel);
        } catch (const TransportError&) {
            transport = true;
        }
        expect(transport, "a call to the closed port is a TransportError, which the CLI follows with the hint");
        unsetenv("XDG_STATE_HOME");
    }

    section("ban filter");
    {
        Bans b;
        b.strings = {"as an AI", "Certainly!"};
        BanFilter f(b);
        std::string shown = f.feed("Sure, ");
        expect(shown == "Sure, ", "text that cannot start a ban is released at once");
        shown += f.feed("here it is. as ");
        expect(shown == "Sure, here it is. ", "a possible ban start is held back, not shown: [" + shown + "]");
        shown += f.feed("an A");
        expect(shown == "Sure, here it is. ", "still held while it could complete");
        shown += f.feed("I model, I");
        expect(f.triggered() && f.hit() == "as an AI" && shown == "Sure, here it is. ", "the ban completes across three chunks and nothing of it was shown");
        BanFilter cs(b);
        cs.feed("As an AI model");
        expect(!cs.triggered(), "matching is case-sensitive by default");
        BanFilter g(b);
        std::string out = g.feed("Yes. as an");
        out += g.feed(" AI, I");
        expect(g.triggered() && g.hit() == "as an AI" && out == "Yes. " && g.clean() == "Yes. ", "a ban split across chunks is caught and only the text before it is released");
        expect(g.feed("more").empty() && g.flush().empty(), "nothing more comes through after a cut");
        BanFilter h(b);
        std::string o2 = h.feed("no bans here");
        o2 += h.flush();
        expect(o2 == "no bans here" && h.clean() == o2, "flush releases the held tail at the end");
        Bans ci = b;
        ci.ignore_case = true;
        BanFilter k(ci);
        k.feed("AS AN ai model");
        expect(k.triggered(), "ignore_case matches any case");
        BanFilter r(b, true);
        std::string o3 = r.feed("Well, Certainly! I can. Certainly!");
        o3 += r.flush();
        expect(o3 == "Well, [banned] I can. [banned]" && !r.triggered(), "replace mode swaps every occurrence and never cuts: " + o3);
        Bans tok;
        tok.tokens = {nlohmann::json("▲")};
        BanFilter t(tok);
        t.feed("up ▲ down");
        expect(t.triggered() && t.hit() == "▲", "a text token ban is a string ban in the filter");
        auto j = b.to_json();
        Bans back = Bans::from_json(j);
        expect(back.strings == b.strings && back.retries == 3 && back.replacement == "[banned]", "bans round-trip through JSON");
        Bans rx;
        rx.patterns = {"as an (ai|assistant)", "certainly[!,]"};
        rx.window = 16;
        BanFilter pf(rx);
        std::string o4 = pf.feed("Well. ");
        expect(o4.empty(), "under the window nothing is released yet");
        o4 += pf.feed("I think, as an ");
        o4 += pf.feed("assistant, I");
        expect(pf.triggered() && pf.hit() == "as an assistant" && o4.find("as an") == std::string::npos && pf.clean() == o4 && o4 == "Well. I think, ",
               "a regex ban across chunks is cut with only the text before it shown: [" + o4 + "]");
        BanFilter pg(rx);
        std::string o5 = pg.feed("a plain reply with nothing banned in it at all, quite long really");
        o5 += pg.flush();
        expect(o5 == "a plain reply with nothing banned in it at all, quite long really" && pg.clean() == o5, "flush releases the window at the end");
        BanFilter ph(rx, true);
        std::string o6 = ph.feed("Certainly! Yes. certainly, as an AI here");
        o6 += ph.flush();
        expect(o6 == "Certainly! Yes. [banned] as an AI here" && !ph.triggered(), "replace mode swaps regex matches, case-sensitively by default: [" + o6 + "]");
        Bans rx2 = rx;
        rx2.ignore_case = true;
        BanFilter pi(rx2, true);
        std::string o7 = pi.feed("Certainly! ok");
        o7 += pi.flush();
        expect(o7 == "[banned] ok", "ignore_case applies to patterns too");
        Bans both;
        both.strings = {"pelican"};
        both.patterns = {"[0-9]{3}-[0-9]{4}"};
        BanFilter pj(both);
        std::string o8 = pj.feed("call 555-1234 now");
        o8 += pj.flush();
        expect(pj.triggered() && pj.hit() == "555-1234" && o8 == "call ", "string and regex stages combine");
        write_file(ws / "bans.txt", "# phrases\npelican\n\n  \nas an AI \n");
        auto lines = expand_ban_entry("@" + (ws / "bans.txt").string());
        expect(lines == std::vector<std::string>{"pelican", "as an AI"}, "@file gives one entry per line, comments and blanks skipped, trailing space trimmed");
        expect(expand_ban_entry("plain") == std::vector<std::string>{"plain"}, "anything else is one entry");
        bool missing = false;
        try {
            expand_ban_entry("@" + (ws / "nope.txt").string());
        } catch (const std::exception&) {
            missing = true;
        }
        expect(missing, "a missing ban file is an error");
        write_file(ws / "toks.txt", "1234\n▲\n");
        Bans bf = Bans::from_json({{"strings", {"@" + (ws / "bans.txt").string(), "x"}}, {"tokens", {"@" + (ws / "toks.txt").string()}}});
        expect(bf.strings == std::vector<std::string>{"pelican", "as an AI", "x"} && bf.tokens.size() == 2 && bf.tokens[0] == 1234 && bf.tokens[1] == "▲",
               "settings entries expand @file too, numeric token lines become ids");
        Bans bad;
        bad.patterns = {"(unclosed"};
        BanFilter pk(bad);
        expect(pk.bad_patterns().size() == 1 && pk.feed("text").size() + pk.flush().size() == 4, "a pattern that does not compile is reported and ignored");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { bans = { patterns = {'x+'}, window = 32 }, sampling = { temperature = 0.3, xtc_probability = 0.5 } }");
        Settings sp = load_settings(ws / "proj");
        expect(sp.bans.patterns == std::vector<std::string>{"x+"} && sp.bans.window == 32 && sp.sampling["xtc_probability"] == 0.5, "patterns, window and global sampling load from settings");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { context = 32768 }");
        expect(load_settings(ws / "proj").context == 32768, "context loads from settings");
        setenv("MAIC_CONTEXT", "4096", 1);
        expect(expand_vars("${MAIC_CONTEXT}") == "4096", "${MAIC_CONTEXT} follows the environment main() sets");
        unsetenv("MAIC_CONTEXT");
        expect(expand_vars("${MAIC_CONTEXT}") == "16384", "and defaults to 16384");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { harness = 'dumb', reviewer_model = 'llamacpp/Qwen3.5-4B-Q4_K_M', dumb_auto_ok = true }");
        Settings sh = load_settings(ws / "proj");
        expect(sh.harness == "dumb" && sh.reviewer_model == "llamacpp/Qwen3.5-4B-Q4_K_M" && sh.dumb_auto_ok, "harness, reviewer_model and dumb_auto_ok load from settings");
        {
            // vendor model: a checked download from a local server, never linked on a hash mismatch.
            httplib::Server srv;
            std::string blob = std::string("GGUF") + std::string(64, 'x');
            srv.Get("/m/tiny.gguf", [&](const httplib::Request&, httplib::Response& res) { res.set_content(blob, "application/octet-stream"); });
            srv.Get("/m/mmproj-F16.gguf", [&](const httplib::Request&, httplib::Response& res) { res.set_content(blob, "application/octet-stream"); });
            int port = srv.bind_to_any_port("127.0.0.1");
            std::thread th([&] { srv.listen_after_bind(); });
            srv.wait_until_ready();
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/m/tiny.gguf";
            std::string good;
            {
                FILE* p = popen(("printf '%s' '" + blob + "' | sha256sum").c_str(), "r");
                char buf[128] = "";
                if (p && fgets(buf, sizeof(buf), p)) good = std::string(buf).substr(0, 64);
                if (p) pclose(p);
            }
            fs::path state = ws / "xdg-state-model";
            setenv("XDG_STATE_HOME", state.c_str(), 1);
            setenv("XDG_CONFIG_HOME", (ws / "no-config").c_str(), 1);
            fs::create_directories(ws / "no-config" / "maic");
            write_file(ws / "no-config" / "maic" / "settings.lua", "return { models_dir = '" + (ws / "mroot").string() + "' }");
            auto ll = find_vendor("llamacpp");
            fs::path into = ws / "mroot" / "llamacpp";
            bool threw_hash = false;
            try {
                vendor_model(*ll, url, std::string(64, '0'), into);
            } catch (const std::exception& ex) {
                threw_hash = std::string(ex.what()).find("SHA-256 mismatch") != std::string::npos;
            }
            expect(threw_hash && !fs::exists(into / "tiny.gguf") && !fs::exists(into / "tiny.gguf.part"), "a hash mismatch discards the download and links nothing");
            fs::path got = vendor_model(*ll, url, good, into);
            expect(got == into / "tiny.gguf" && fs::exists(got) && fs::read_symlink(vendor_model_link(*ll)) == fs::weakly_canonical(got), "a matching hash keeps the file and links it as the model");
            bool threw_short = false;
            try {
                vendor_model(*ll, url, "abc", into);
            } catch (const std::exception&) {
                threw_short = true;
            }
            expect(threw_short, "the SHA-256 is required");
            fs::path proj = vendor_model(*ll, "http://127.0.0.1:" + std::to_string(port) + "/m/mmproj-F16.gguf", good, into / "Proj-Q4");
            expect(proj == into / "Proj-Q4" / "mmproj-F16.gguf" && fs::exists(proj) && fs::read_symlink(vendor_model_link(*ll)) == fs::weakly_canonical(got),
                   "a vision projector is saved where asked and never linked as the current model");
            // Router ids: a flat GGUF by stem, a subdirectory by its name; `current` resolves to the linked id.
            fs::create_directories(ws / "mroot" / "llamacpp" / "Big-Q4");
            write_file(ws / "mroot" / "llamacpp" / "Big-Q4" / "Big-Q4.gguf", "GGUF....");
            write_file(ws / "mroot" / "llamacpp" / "Big-Q4" / "mmproj-F16.gguf", "GGUF....");
            write_file(ws / "mroot" / "llamacpp" / "Small-Q4.gguf", "GGUF....");
            write_file(ws / "mroot" / "llamacpp" / "notes.txt", "x");
            expect(llamacpp_models_root() == ws / "mroot" / "llamacpp", "the models root follows models_dir");
            expect(llamacpp_model_ids() == std::vector<std::string>{"Big-Q4", "Small-Q4", "tiny"}, "ids: subdirectory name or file stem, mmproj and other files ignored");
            vendor_use(*ll, ws / "mroot" / "llamacpp" / "Big-Q4" / "Big-Q4.gguf");
            expect(llamacpp_current_id() == "Big-Q4" && resolve_model_alias("llamacpp/current") == "llamacpp/Big-Q4", "current resolves to the linked file's router id");
            expect(resolve_model_alias("anthropic/x") == "anthropic/x", "other models pass through");
            write_file(ws / "elsewhere.gguf", "GGUF....");
            bool outside = false;
            try {
                vendor_use(*ll, ws / "elsewhere.gguf");
            } catch (const std::exception& e) {
                outside = std::string(e.what()).find("only sees GGUFs under") != std::string::npos;
            }
            expect(outside, "a GGUF outside the models root is refused with the layout explained");
            unsetenv("XDG_CONFIG_HOME");
            srv.stop();
            th.join();
            unsetenv("XDG_STATE_HOME");
        }
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { harness = 'clever' }");
        bool bad_harness = false;
        try {
            load_settings(ws / "proj");
        } catch (const std::exception&) {
            bad_harness = true;
        }
        expect(bad_harness, "an unknown harness value is an error");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { bans = { strings = {'lol'}, tokens = {42, 'x'}, retries = 1 } }");
        Settings s3 = load_settings(ws / "proj");
        expect(s3.bans.strings == std::vector<std::string>{"lol"} && s3.bans.tokens.size() == 2 && s3.bans.retries == 1, "bans load from settings");
        fs::remove(ws / "proj" / ".maic" / "settings.lua");
    }

    section("places");
    {
        Settings s;
        s.models_dir = (ws / "mdl").string();
        auto services = load_services(root_dir() / "services");
        auto places = known_places(s, ws, services, ws / "t.jsonl");
        auto has = [&](const std::string& n) { return std::any_of(places.begin(), places.end(), [&](const Place& p) { return p.name == n; }); };
        expect(has("workspace") && has("session") && has("sessions") && has("settings") && has("models") && has("models/llamacpp") && has("workflows") && has("templates") && has("comfyui/outputs") && has("maic/sessions"),
               "the registry has MAIC's places, the models root, and every artifact as owner/name");
        expect(find_place(places, "workspace").path == ws && find_place(places, "session").path == ws / "t.jsonl" && find_place(places, "models").path == ws / "mdl", "exact names resolve");
        expect(find_place(places, "worksp").name == "workspace" && find_place(places, "templ").name == "templates" && find_place(places, "outputs").name == "comfyui/outputs", "a unique prefix resolves; a top-level name wins over owner/name");
        bool ambiguous = false;
        try {
            find_place(places, "sess");  // session and sessions are both top-level names here
        } catch (const std::exception& e) {
            ambiguous = std::string(e.what()).find("several") != std::string::npos && std::string(e.what()).find("session, sessions") != std::string::npos;
        }
        expect(ambiguous, "an ambiguous prefix lists the candidates");
        bool none = false;
        try {
            find_place(places, "zzz");
        } catch (const std::exception& e) {
            none = std::string(e.what()).find("no place named") == 0;
        }
        expect(none, "an unknown name lists the places");
        std::string zsh = shell_init("zsh"), bash = shell_init("bash"), fish = shell_init("fish");
        expect(zsh.find("mcd()") != std::string::npos && zsh.find("compdef") != std::string::npos && bash.find("complete -F") != std::string::npos && fish.find("function mcd") != std::string::npos,
               "shell-init gives mcd with completion for zsh, bash and fish");
        std::set<std::string> names;
        for (const auto& p : places) expect(names.insert(p.name).second, "place names are unique: " + p.name);
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { allow = { 'pytest *' } }");
        Settings sa = load_settings(ws / "proj");
        auto allows = [](const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); };
        expect(allows(sa.permission.allow, "run_shell:pytest *") && allows(sa.permission.allow, "run_shell:maic-storyboard*"),
               "the old allow key lands in permission.allow as run_shell entries, beside the default helpers");
        write_file(ws / "proj" / ".maic" / "settings.lua",
                   "return { permission = { allow = { 'run_shell:npm test' }, ask = { 'edit_file:src/core.cpp' }, deny = { 'write:build/**' } },\n"
                   "  profiles = { scout = { budget_tokens = 20000, max_steps = 10 },\n"
                   "               docs = { mode = 'edit', write_paths = { 'docs/**' }, tools = { 'read_file', 'edit_file' }, model = 'qwen-4b', reviewer = false } } }");
        Settings sp = load_settings(ws / "proj");
        expect(allows(sp.permission.allow, "run_shell:npm test") && allows(sp.permission.allow, "run_shell:maic-storyboard*") && sp.permission.ask == std::vector<std::string>{"edit_file:src/core.cpp"} && sp.permission.deny == std::vector<std::string>{"write:build/**"},
               "permission lists load and add to the default allow entries");
        const Profile* scout = find_profile(sp.profiles, "scout");
        const Profile* docs = find_profile(sp.profiles, "docs");
        expect(scout && scout->budget_tokens == 20000 && scout->max_steps == 10 && scout->mode == Mode::AutoRead && scout->tools.size() == 5 && !scout->reviewer,
               "a built-in profile is narrowed in place and keeps what the file does not mention");
        expect(docs && sp.profiles.size() == 5 && docs->mode == Mode::Edit && docs->write_paths == std::vector<std::string>{"docs/**"} && docs->tools.size() == 2 && docs->model == "llamacpp/Qwen3.5-4B-Q4_K_M" && !docs->reviewer && docs->read_outside,
               "a new profile is added with its fields, a preset name resolved to its model");
        auto rejects = [&](const char* lua, const char* needle) {
            write_file(ws / "proj" / ".maic" / "settings.lua", lua);
            try {
                load_settings(ws / "proj");
            } catch (const std::exception& e) {
                return std::string(e.what()).find(needle) != std::string::npos;
            }
            return false;
        };
        expect(rejects("return { profiles = { scout = { mode = 'auto' } } }", "wider than the built-in scout"), "a built-in profile cannot be given a wider mode");
        expect(rejects("return { profiles = { scout = { tools = { 'read_file', 'write_file' } } } }", "has no write_file tool"), "nor a tool it lacks");
        expect(rejects("return { profiles = { scout = { budget_tokens = 100000 } } }", "above the built-in"), "nor a bigger budget");
        expect(rejects("return { profiles = { reviewer = { read_outside = true } } }", "does not read outside"), "nor reads outside the workspace");
        expect(rejects("return { profiles = { wired = { network = true } } }", "network"), "no profile gets the network");
        expect(rejects("return { profiles = { x = { mode = 'fast' } } }", "mode must be"), "a bad mode is an error");
        expect(rejects("return { permission = { allow = { 'pytest *' } } }", "tool:pattern"), "a permission entry without its tool is an error");
        expect(rejects("return { permission = { deny = { 'run_shell:' } } }", "tool:pattern"), "and so is one without a pattern");
        fs::remove(ws / "proj" / ".maic" / "settings.lua");
    }

    section("freeing the GPU for a service");
    {
        fs::path gpu_state = ws / "xdg-state-gpu";
        setenv("XDG_STATE_HOME", gpu_state.c_str(), 1);
        httplib::Server srv;
        std::vector<std::string> unloaded;
        srv.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"data":[{"id":"Big","status":{"value":"loaded"}},{"id":"Small","status":{"value":"unloaded"}}]})", "application/json");
        });
        srv.Post("/models/unload", [&](const httplib::Request& req, httplib::Response& res) {
            unloaded.push_back(json::parse(req.body)["model"]);
            res.set_content(R"({"success":true})", "application/json");
        });
        int port = srv.bind_to_any_port("127.0.0.1");
        std::thread th([&] { srv.listen_after_bind(); });
        srv.wait_until_ready();
        std::string url = "http://127.0.0.1:" + std::to_string(port);
        expect(resident_models(url) == std::vector<std::string>{"Big"}, "resident_models lists only what is loaded");
        expect(unload_resident(url) == std::vector<std::string>{"Big"} && unloaded == std::vector<std::string>{"Big"}, "unload_resident asks the router to unload each resident model");
        expect(resident_models("http://127.0.0.1:" + std::to_string(closed_port())).empty(), "a server that is not there has nothing resident");
        srv.stop();
        th.join();
        ServiceDef comfy;
        comfy.name = "comfyui";
        comfy.needs_gpu = true;
        ServiceDef lc;
        lc.name = "llamacpp";
        lc.port = closed_port();
        expect(free_gpu_for(comfy, {lc}).empty(), "nothing to free when llamacpp is not running");
        ServiceDef plain;
        plain.name = "plain";
        expect(free_gpu_for(plain, {lc}).empty(), "a service that does not need the GPU frees nothing");
        auto defs = load_services(root_dir() / "services");
        bool marked = false;
        for (const auto& d : defs) marked = marked || (d.name == "comfyui" && d.needs_gpu);
        expect(marked, "services/comfyui.json is marked needs_gpu");
        // explain_exit reads the log's tail.
        ServiceDef dead;
        dead.name = "zz-test-svc";
        dead.port = 9;
        fs::create_directories(service_log_path(dead).parent_path());
        write_file(service_log_path(dead), "starting\ntorch.AcceleratorError: CUDA error: out of memory\nReturning 2 (CUDA_ERROR_OUT_OF_MEMORY)\n");
        std::string why = explain_exit(dead, {lc});
        expect(why.rfind("CUDA out of memory", 0) == 0 && why.find("maic gpu") != std::string::npos, "an out-of-memory exit is explained with the way out: " + why);
        write_file(service_log_path(dead), "OSError: [Errno 98] Address already in use\n");
        expect(explain_exit(dead, {lc}).find("port 9 is already in use") == 0, "a port clash is explained");
        write_file(service_log_path(dead), "ModuleNotFoundError: No module named 'aiohttp'\n");
        expect(explain_exit(dead, {lc}).find("Python module is missing") != std::string::npos, "a missing module is explained");
        write_file(service_log_path(dead), "all fine\n");
        expect(explain_exit(dead, {lc}).empty(), "an unrecognised log explains nothing");
        fs::remove(service_log_path(dead));
        GpuReport g;
        g.servers = {{"llamacpp", true, {"Big"}}, {"llamacpp-2", false, {}}};
        g.comfyui_running = true;
        g.comfyui_vram_used = 3L << 30;
        g.comfyui_vram_total = 8L << 30;
        std::string t = g.text();
        expect(t.find("llamacpp: holds Big") != std::string::npos && t.find("llamacpp-2: not running") != std::string::npos && t.find("3.0 GB used of 8.0 GB") != std::string::npos,
               "the report names each server, the holder and the figures: " + t);

        // Two fake routers, one per server, each holding a model; MAIC's pid files say it started them.
        auto router = [](httplib::Server& r, const std::string& held, std::vector<std::string>& unloads) {
            r.Get("/v1/models", [held](const httplib::Request&, httplib::Response& res) {
                res.set_content(json{{"data", {{{"id", held}, {"status", {{"value", "loaded"}}}}}}}.dump(), "application/json");
            });
            r.Post("/models/unload", [&unloads](const httplib::Request& req, httplib::Response& res) {
                unloads.push_back(json::parse(req.body)["model"]);
                res.set_content(R"({"success":true})", "application/json");
            });
        };
        httplib::Server r1, r2;
        std::vector<std::string> unloaded1, unloaded2;
        router(r1, "Qwen3.5-4B-Q4_K_M", unloaded1);
        router(r2, "Qwen3.5-9B-Q4_K_M-text", unloaded2);
        int port1 = r1.bind_to_any_port("127.0.0.1"), port2 = r2.bind_to_any_port("127.0.0.1");
        std::thread t1([&] { r1.listen_after_bind(); }), t2([&] { r2.listen_after_bind(); });
        r1.wait_until_ready();
        r2.wait_until_ready();
        ServiceDef main_def, side_def;
        main_def.name = "llamacpp";
        main_def.port = port1;
        side_def.name = "llamacpp-2";
        side_def.port = port2;
        side_def.needs_gpu = true;
        {
            // This process stands in for both servers: its pid and start time (field 22 of /proc/self/stat).
            std::ifstream in("/proc/self/stat");
            std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string after = stat.substr(stat.rfind(')') + 2);
            std::istringstream fields(after);
            std::string f;
            for (int i = 0; i < 19; ++i) fields >> f;  // fields 3..21
            std::string start_time;
            fields >> start_time;
            for (const auto* name : {"llamacpp", "llamacpp-2"}) write_file(gpu_state / "maic" / "run" / (std::string(name) + ".pid"), std::to_string(getpid()) + " " + start_time + "\n");
        }
        std::vector<ServiceDef> two = {main_def, side_def, comfy};
        GpuReport both = gpu_report(two);
        expect(both.servers.size() == 2 && both.servers[0].running && both.servers[0].models == std::vector<std::string>{"Qwen3.5-4B-Q4_K_M"} &&
               both.servers[1].running && both.servers[1].models == std::vector<std::string>{"Qwen3.5-9B-Q4_K_M-text"},
               "gpu_report lists the model resident in each server");
        std::string freed = free_gpu_for(comfy, two);
        expect(unloaded1 == std::vector<std::string>{"Qwen3.5-4B-Q4_K_M"} && unloaded2 == std::vector<std::string>{"Qwen3.5-9B-Q4_K_M-text"} &&
               freed.find("from llamacpp;") != std::string::npos && freed.find("from llamacpp-2") != std::string::npos, "starting ComfyUI frees both servers: " + freed);
        expect(free_gpu_for(side_def, two).empty() && unloaded1.size() == 1, "starting a llama server evicts nothing from the other");
        unloaded2.clear();
        std::string one = gpu_free(two, "llamacpp-2");
        expect(unloaded1.size() == 1 && unloaded2.size() == 1 && one.find("llamacpp-2: unloaded Qwen3.5-9B") == 0, "maic gpu free llamacpp-2 unloads the side server only: " + one);
        threw = false;
        try { gpu_free(two, "llamacpp-3"); } catch (const std::exception& e) { threw = std::string(e.what()).find("llamacpp-2") != std::string::npos; }
        expect(threw, "an unknown name lists the choices");
        r1.stop();
        r2.stop();
        t1.join();
        t2.join();
        fs::remove_all(gpu_state / "maic" / "run");

        // The budget: GGUF sizes on disk plus an estimated KV cache, against the card.
        fs::path mroot = ws / "budget-models";
        fs::create_directories(mroot / "Qwen3.5-4B-Q4_K_M");
        auto sized = [](const fs::path& p, long bytes) {
            std::ofstream(p, std::ios::binary) << "GGUF";
            fs::resize_file(p, bytes);
        };
        sized(mroot / "Qwen3.5-4B-Q4_K_M" / "Qwen3.5-4B-Q4_K_M.gguf", 2560L << 20);  // 2.5 GB
        sized(mroot / "Qwen3.5-4B-Q4_K_M" / "mmproj-F16.gguf", 512L << 20);          // loaded with it
        sized(mroot / "Qwen3.5-9B-Q4_K_M-text.gguf", 5376L << 20);                   // 5.25 GB, a bare file
        long four = model_footprint({"Qwen3.5-4B-Q4_K_M", 16384}, mroot);
        expect(four == (3072L << 20) + static_cast<long>(16384 * 0.065 * 1024 * 1024), "a 4B at 16k: the folder's GGUFs plus 65 MB per 1k tokens");
        long nine = model_footprint({"Qwen3.5-9B-Q4_K_M-text", 8192}, mroot);
        expect(nine == (5376L << 20) + static_cast<long>(8192 * 0.13 * 1024 * 1024), "a 9B at 8k: the file plus 130 MB per 1k tokens");
        expect(model_footprint({"nope", 8192}, mroot) == -1, "a model that is not under the root has no footprint");
        std::string fit = budget_sentence({{"Qwen3.5-4B-Q4_K_M", 16384}, {"Qwen3.5-4B-Q4_K_M", 8192}}, mroot, 8L << 30, false, -1);
        expect(fit == "4B at 16k (4.0 GB est.) + 4B at 8k (3.5 GB est.) = 7.6 GB of 8.0 GB: fits with ComfyUI stopped", "two 4Bs on an 8 GB card: " + fit);
        fit = budget_sentence({{"Qwen3.5-4B-Q4_K_M", 16384}, {"Qwen3.5-9B-Q4_K_M-text", 8192}}, mroot, 8L << 30, false, -1);
        expect(fit.find("4B at 16k (4.0 GB est.) + 9B at 8k (6.3 GB est.) = 10.3 GB of 8.0 GB: does not fit") == 0, "a 4B and a 9B do not: " + fit);
        fit = budget_sentence({{"Qwen3.5-4B-Q4_K_M", 8192}}, mroot, 8L << 30, true, 2L << 30);
        expect(fit.find("= 3.5 GB of 8.0 GB: fits beside ComfyUI (2.0 GB in use)") != std::string::npos, "one model beside a running ComfyUI: " + fit);
        fit = budget_sentence({{"Qwen3.5-4B-Q4_K_M", 16384}, {"Qwen3.5-4B-Q4_K_M", 8192}}, mroot, 8L << 30, true, 3L << 30);
        expect(fit.find("fits only with ComfyUI stopped (it holds 3.0 GB)") != std::string::npos, "or only once ComfyUI lets go: " + fit);
        fit = budget_sentence({{"Qwen3.5-4B-Q4_K_M", 8192}}, mroot, -1, false, -1);
        expect(fit.find("= 3.5 GB estimated; the card's size is unknown") != std::string::npos, "no card figure: the estimate alone, labelled: " + fit);
        expect(budget_sentence({{"nope", 8192}}, mroot, 8L << 30, false, -1).empty(), "nothing to say about models that are not there");
        fs::remove_all(mroot);

        // The story-chat workflow's deep pass points at the side server.
        std::ifstream wf(root_dir() / "vendor" / "comfyui-maic-llamacpp" / "example_workflows" / "story-chat-llamacpp.json");
        json w = json::parse(wf, nullptr, false);
        std::string deep_url, deep_title, quick_url;
        for (const auto& n : w.value("nodes", json::array())) {
            if (n.value("type", "") != "MaicLlmServer") continue;
            std::string title = n.value("title", "");
            if (title.rfind("Deep", 0) == 0) deep_url = n["widgets_values"][0], deep_title = title;
            if (title.rfind("Quick", 0) == 0) quick_url = n["widgets_values"][0];
        }
        expect(deep_url == "http://127.0.0.1:8082/v1" && deep_title == "Deep model (llamacpp-2)" && quick_url == "http://127.0.0.1:8081/v1",
               "the workflow's deep MaicLlmServer node is on 8082 and says so; the quick one stays on 8081");
        unsetenv("XDG_STATE_HOME");
    }

    section("model presets");
    {
        Settings d;
        auto op = find_preset(d.presets, "Opus 5.5");
        expect(op && op->model == "anthropic/claude-opus-5-5" && op->context == 1000000 && op->reviewer == "anthropic/claude-sonnet-5" && op->think == 1, "the Opus 5.5 preset resolves from a loose spelling");
        expect(find_preset(d.presets, "opus55") && find_preset(d.presets, "claude-opus-5-5") && find_preset(d.presets, "OPUS_5.5"), "hyphens, dots, spaces, underscores and a claude- prefix all match");
        expect(!find_preset(d.presets, "gpt-9"), "an unknown name is no preset");
        auto q9 = find_preset(d.presets, "qwen-9b");
        auto q9v = find_preset(d.presets, "qwen-9b-vision");
        expect(q9 && q9->model == "llamacpp/Qwen3.5-9B-Q4_K_M-text" && q9->context == 16384 && q9v && q9v->model == "llamacpp/Qwen3.5-9B-Q4_K_M" && q9v->context == 8192,
               "the 9B has a text preset at 16k and a vision preset at 8k");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { models = { ['opus-5.5'] = { model = 'anthropic/claude-opus-5-5', context = 500000, reviewer = 'same' }, mine = { model = 'llamacpp/Other', context = 4096 } } }");
        Settings sp = load_settings(ws / "proj");
        auto over = find_preset(sp.presets, "opus-5.5");
        expect(over && over->context == 500000 && over->reviewer == "same" && over->think == -1, "settings override a built-in preset by name");
        expect(find_preset(sp.presets, "mine") && find_preset(sp.presets, "mine")->model == "llamacpp/Other", "and add new ones");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { models = { bad = { context = 1 } } }");
        bool threw = false;
        try {
            load_settings(ws / "proj");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a preset without a model is an error");
        fs::remove(ws / "proj" / ".maic" / "settings.lua");
    }

    section("tripwire scope");
    {
        fs::path session_lock = ws / "t.jsonl.tripped";
        set_tripwire_scope("session", session_lock);
        expect(!tripwire_state() && !session_tripped(), "armed to begin with");
        trip_tripwire("test trip");
        expect(session_tripped() && tripwire_state() && tripwire_state()->find("test trip") != std::string::npos && fs::exists(session_lock), "a session-scoped trip writes the session lock, no root helper");
        bool refused = false;
        try {
            require_armed("do a thing");
        } catch (const std::exception&) {
            refused = true;
        }
        expect(refused, "and require_armed refuses while it stands");
        expect(unlock_session() && !tripwire_state() && !fs::exists(session_lock), "unlock_session removes it without sudo");
        // The machine lock (the test's scratch file) is honoured even in session scope.
        write_file(std::getenv("MAIC_TRIPWIRE_FILE"), "reason: machine\n");
        expect(tripwire_state() && tripwire_state()->find("machine") != std::string::npos && !unlock_session(), "the machine lock still counts, and unlock_session cannot clear it");
        fs::remove(std::getenv("MAIC_TRIPWIRE_FILE"));
        // isolated: the machine lock is ignored, the session lock still counts.
        set_tripwire_scope("isolated", session_lock);
        write_file(std::getenv("MAIC_TRIPWIRE_FILE"), "reason: machine\n");
        expect(!tripwire_state(), "an isolated session ignores the machine lock");
        trip_tripwire("own trip");
        expect(tripwire_state() && session_tripped(), "but its own lock still stops it");
        unlock_session();
        fs::remove(std::getenv("MAIC_TRIPWIRE_FILE"));
        set_tripwire_scope("machine", {});
        expect(browser_command("default", "http://x").rfind("xdg-open", 0) == 0 && browser_command("firefox", "http://x").rfind("firefox", 0) == 0 && browser_command("chrome", "http://x").find("chromium") != std::string::npos,
               "browser_command builds the right opener for default, firefox and chrome");
        SessionInfo dead;
        dead.pid = 0;
        expect(!session_running(dead), "a session with no pid is not running");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { tripwire = 'isolated', allow_isolated = true, browser = 'firefox', remote = 'https://box:7373' }");
        Settings si = load_settings(ws / "proj");
        expect(si.tripwire == "isolated" && si.allow_isolated && si.browser == "firefox" && si.remote == "https://box:7373", "isolated, allow_isolated, browser and remote load from settings");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { browser = 'lynx' }");
        bool bad_browser = false;
        try {
            load_settings(ws / "proj");
        } catch (const std::exception&) {
            bad_browser = true;
        }
        expect(bad_browser, "an unknown browser is an error");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { tripwire = 'session' }");
        expect(load_settings(ws / "proj").tripwire == "session", "the scope loads from settings");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { tripwire = 'sometimes' }");
        bool bad = false;
        try {
            load_settings(ws / "proj");
        } catch (const std::exception&) {
            bad = true;
        }
        expect(bad, "an unknown scope is an error");
        fs::remove(ws / "proj" / ".maic" / "settings.lua");
    }

    section("docker runtime, ready patterns and health");
    {
        fs::path state = ws / "xdg-state-docker";
        setenv("XDG_STATE_HOME", state.c_str(), 1);
        setenv("XDG_CONFIG_HOME", (ws / "xdg-config-empty").c_str(), 1);
        fs::create_directories(state / "maic" / "models");
        // The shipped files: host services, ready patterns, no docker; the .example is not loaded.
        auto shipped = load_services(root_dir() / "services");
        bool patterns = false, no_docker = true;
        for (const auto& d : shipped) {
            if (d.name == "comfyui") patterns = d.ready_pattern == "To see the GUI go to";
            if (d.name == "llamacpp") patterns = patterns && d.ready_pattern == "listening on";
            no_docker = no_docker && d.runtime == "host";
        }
        expect(patterns && no_docker && fs::exists(root_dir() / "services" / "comfyui-docker.json.example"), "the shipped services are host services with ready patterns; the docker example is not loaded");

        fs::path sdir = ws / "services-docker";
        std::string vol_ok = (state / "maic" / "models").string();
        write_file(sdir / "dk.json", R"({"name":"dk","runtime":"docker","image":"example/comfy:1","command":["--listen","0.0.0.0"],"port":8188,"gpu":true,
            "volumes":{"${MAIC_MODELS}":"/models","${MAIC_STATE}/workflows/comfyui":"/wf"},"env":{"PYTHONUNBUFFERED":"1"},"ready_pattern":"GUI go to"})");
        write_file(sdir / "skipped.json.example", R"({"name":"skipped","runtime":"docker","image":"x"})");
        auto defs = load_services(sdir);
        expect(defs.size() == 1 && defs[0].runtime == "docker" && defs[0].image == "example/comfy:1" && defs[0].gpu && defs[0].volumes.size() == 2 && defs[0].volumes.at(vol_ok) == "/models",
               "a docker service loads its image, volumes and gpu flag; the .example file is skipped");
        std::vector<std::string> want = {"docker", "run", "--rm", "-d", "--name", "maic-dk", "-p", "127.0.0.1:8188:8188", "-v", vol_ok + ":/models", "-v",
                                         (state / "maic" / "workflows" / "comfyui").string() + ":/wf", "-e", "PYTHONUNBUFFERED=1", "--gpus", "all", "example/comfy:1", "--listen", "0.0.0.0"};
        expect(launch_argv(defs[0]) == want, "docker run binds loopback only, mounts the volumes, passes env and the GPU, then the image and its command");
        auto refuses = [&](const std::string& json, const std::string& needle) {
            fs::path bad = ws / "services-bad";
            fs::remove_all(bad);
            write_file(bad / "bad.json", json);
            try {
                load_services(bad);
            } catch (const std::exception& e) {
                return std::string(e.what()).find(needle) != std::string::npos;
            }
            return false;
        };
        expect(refuses(R"({"name":"b","runtime":"docker","image":"x","volumes":{"/etc":"/etc"}})", "outside ${MAIC_STATE}"), "a volume outside MAIC's trees is refused");
        expect(refuses(R"({"name":"b","runtime":"docker","command":["x"]})", "needs an image"), "a docker service without an image is refused");
        expect(refuses(R"({"name":"b","runtime":"podman","command":["x"]})", "unknown (host or docker)"), "an unknown runtime is refused");
        expect(refuses(R"({"name":"b","command":["x"],"ready_pattern":"("})", "ready_pattern"), "a ready_pattern that does not compile is refused");

        // A fake docker on PATH: records its argv, answers inspect from a marker file, logs from a file.
        fs::path fake = ws / "fake-docker";
        write_file(fake / "bin" / "docker", "#!/bin/sh\necho \"$*\" >> \"$FAKE_DOCKER_DIR/argv\"\n"
                                           "case \"$1\" in\n"
                                           "run) [ -e \"$FAKE_DOCKER_DIR/fail\" ] && { echo 'Unable to find image' >&2; exit 125; }; echo abc123; touch \"$FAKE_DOCKER_DIR/running\";;\n"
                                           "inspect) [ -e \"$FAKE_DOCKER_DIR/running\" ] && echo true || { echo \"Error: No such object: $4\" >&2; exit 1; };;\n"
                                           "logs) cat \"$FAKE_DOCKER_DIR/log\";;\n"
                                           "stop) rm -f \"$FAKE_DOCKER_DIR/running\"; echo \"$4\";;\n"
                                           "esac\n");
        fs::permissions(fake / "bin" / "docker", fs::perms::owner_all);
        std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
        setenv("PATH", ((fake / "bin").string() + ":" + old_path).c_str(), 1);
        setenv("FAKE_DOCKER_DIR", fake.c_str(), 1);
        ServiceDef dk = defs[0];
        dk.port = 0;  // the fake opens nothing; readiness comes from the pattern
        write_file(fake / "log", "loading\nTo see the GUI go to: http://0.0.0.0:8188\n");
        expect(service_status(dk).state == ServiceState::Stopped, "nothing recorded: stopped, and docker is not even asked");
        expect(start_service(dk), "start_service runs docker and is ready once the pattern shows in docker logs");
        std::string argv = read_whole_text(fake / "argv");
        expect(argv.find("run --rm -d --name maic-dk -v ") == 0 && argv.find("--gpus all example/comfy:1 --listen 0.0.0.0\n") != std::string::npos && argv.find("inspect -f {{.State.Running}} maic-dk") != std::string::npos,
               "the fake saw docker run and docker inspect: " + argv.substr(0, argv.find('\n')));
        expect(fs::exists(state / "maic" / "run" / "dk.container") && read_whole_text(state / "maic" / "run" / "dk.container") == "maic-dk\n", "the container name is recorded where a pid file would be");
        ServiceStatus st = service_status(dk);
        expect(st.state == ServiceState::Running && st.container == "maic-dk" && st.pid == 0 && st.who() == "container maic-dk", "status asks docker inspect and names the container");
        auto reports = service_reports({dk});
        std::string shown = format_status(StatusReport{false, "", reports, {}});
        expect(reports.size() == 1 && reports[0].runtime == "docker" && reports[0].where.rfind("container maic-dk", 0) == 0 && shown.find("dk: starting [docker]  container maic-dk") != std::string::npos,
               "maic status shows [docker] and the container: " + shown);
        expect(log_tail(dk, 10) == read_whole_text(fake / "log") && argv.find("logs --tail 10 maic-dk") == std::string::npos && read_whole_text(fake / "argv").find("logs --tail 10 maic-dk") != std::string::npos,
               "logs come from docker logs while the container runs");
        expect(!recorded_command(dk).empty() && !command_changed(dk), "the docker run argv is the recorded command");
        bool again = false;
        try {
            start_service(dk);
        } catch (const std::exception& e) {
            again = std::string(e.what()).find("already running (container maic-dk)") != std::string::npos;
        }
        expect(again, "starting it twice says which container runs");
        stop_service(dk);
        expect(read_whole_text(fake / "argv").find("stop -t 15 maic-dk") != std::string::npos && !fs::exists(state / "maic" / "run" / "dk.container") && service_status(dk).state == ServiceState::Stopped,
               "stop_service runs docker stop and forgets the container");
        bool not_running = false;
        try {
            stop_service(dk);
        } catch (const std::exception& e) {
            not_running = std::string(e.what()) == "dk is not running";
        }
        expect(not_running, "stopping it again says so");
        write_file(fake / "log", "loading\nstill loading\n");
        dk.ready_timeout = std::chrono::seconds{1};
        expect(!start_service(dk) && service_status(dk).state == ServiceState::Running, "without the pattern in the logs the container is still starting when the timeout runs out");
        stop_service(dk);
        expect(log_tail(dk, 5).find("=== maic: starting dk") != std::string::npos && log_tail(dk, 5).find("abc123") != std::string::npos, "stopped, logs fall back to MAIC's own notes (the start header, docker's output)");
        write_file(fake / "fail", "");
        bool failed = false;
        try {
            start_service(dk);
        } catch (const std::exception& e) {
            failed = std::string(e.what()).find("docker run failed (Unable to find image)") != std::string::npos;
        }
        expect(failed && !fs::exists(state / "maic" / "run" / "dk.container"), "a failed docker run is reported with docker's words and records nothing");
        fs::remove(fake / "fail");
        setenv("PATH", old_path.c_str(), 1);
        unsetenv("FAKE_DOCKER_DIR");

        // Host services: the pattern must appear in the output written since this start.
        ServiceDef hp;
        hp.name = "hp";
        hp.command = {"sh", "-c", "echo hello ready; sleep 30"};
        hp.ready_pattern = "hello ready";
        hp.ready_timeout = std::chrono::seconds{5};
        write_file(service_log_path(hp), "hello ready from an earlier run\n");
        expect(start_service(hp) && service_status(hp).state == ServiceState::Running, "a host service is ready once its output matches ready_pattern");
        stop_service(hp);
        hp.command = {"sh", "-c", "echo nothing of note; sleep 30"};
        hp.ready_timeout = std::chrono::seconds{1};
        expect(!start_service(hp), "an old match in the log does not count; the pattern must come from this start");
        stop_service(hp);
        hp.ready_pattern = "";
        expect(start_service(hp), "without a pattern (and without a port) a started process is ready");
        stop_service(hp);
        expect(service_status(hp).state == ServiceState::Stopped, "and stops cleanly");

        // Health detail: a fake ComfyUI and a fake router.
        httplib::Server comfy;
        std::string queue = R"({"queue_running":[[0,"a"]],"queue_pending":[[1,"b"],[2,"c"]]})";
        comfy.Get("/system_stats", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"devices":[{"name":"cuda:0","vram_total":8589934592,"vram_free":5368709120}]})", "application/json");
        });
        comfy.Get("/queue", [&](const httplib::Request&, httplib::Response& res) { res.set_content(queue, "application/json"); });
        comfy.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"data":[{"id":"Big","status":{"value":"loaded"}}]})", "application/json");
        });
        int cport = comfy.bind_to_any_port("127.0.0.1");
        std::thread ct([&] { comfy.listen_after_bind(); });
        comfy.wait_until_ready();
        ServiceDef cf;
        cf.name = "comfyui";
        cf.port = cport;
        ServiceStatus open;
        open.port_open = true;
        expect(service_detail(cf, open) == "VRAM 3.0 GB used of 8.0 GB; queue: 1 running, 2 pending", "ComfyUI's detail: VRAM from /system_stats and the queue: " + service_detail(cf, open));
        queue = R"({"queue_running":[],"queue_pending":[]})";
        expect(service_detail(cf, open) == "VRAM 3.0 GB used of 8.0 GB; queue idle", "an empty queue is idle");
        ServiceDef lsrv;
        lsrv.name = "llamacpp-2";
        lsrv.port = cport;
        expect(service_detail(lsrv, open) == "model: Big", "a llama server's detail is its resident model");
        expect(service_detail(cf, ServiceStatus{}).empty(), "a closed port has no detail");
        ServiceDef other;
        other.name = "server";
        other.port = cport;
        expect(service_detail(other, open).empty(), "other services have none");
        comfy.stop();
        ct.join();

        // The venv's torch against the driver.
        expect(cuda_agreement("13.0", "13.0").find("they agree") != std::string::npos && cuda_agreement("12.8", "13.0").find("they agree") != std::string::npos, "a driver at or above torch's CUDA agrees");
        std::string bad = cuda_agreement("13.0", "12.8");
        expect(bad.find("only 12.8") != std::string::npos && bad.find("update the driver (580 or newer for CUDA 13)") != std::string::npos && bad.find("cu128") != std::string::npos, "an older driver names the fix: " + bad);
        expect(cuda_agreement("", "13.0").empty() && cuda_agreement("13.0", "").empty(), "nothing to say when either side is unknown");
        expect(comfyui_torch_cuda().empty(), "no vendored ComfyUI venv here: torch's CUDA is unknown");

        // extra_model_paths.yaml: only the maic: block is regenerated.
        std::map<std::string, std::string> cats = {{"checkpoints", "checkpoints"}, {"upscale_models", "upscale_models"}, {"vae", "vae/"}};
        std::string yaml = "# mine\nother:\n    base_path: /elsewhere/\n    loras: loras/\n\nmaic:\n    base_path: \"/old/\"\n    checkpoints: checkpoints/\n\nthird:\n    vae: v/\n";
        std::string merged = merge_model_paths_yaml(yaml, "/models", cats);
        expect(merged == "# mine\nother:\n    base_path: /elsewhere/\n    loras: loras/\n\nthird:\n    vae: v/\n\nmaic:\n    base_path: \"/models/\"\n    checkpoints: checkpoints/\n    upscale_models: upscale_models/\n    vae: vae/\n",
               "other root keys and comments stay, the old maic: block goes, the new one lists every category: " + merged);
        expect(merge_model_paths_yaml(merged, "/models", cats) == merged, "running it again changes nothing");
        expect(merge_model_paths_yaml("", "/models/", cats).rfind("maic:\n    base_path: \"/models/\"\n", 0) == 0, "a missing file gets the block alone");
        auto comfy_entry = find_vendor("comfyui");
        expect(comfy_entry && comfy_entry->models.size() == 11 && comfy_entry->models.count("upscale_models") && comfy_entry->models.count("model_patches"), "the manifest maps every category ComfyUI reads");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_STATE_HOME");
    }

    section("system prompt setting");
    {
        expect(resolve_system_prompt("plain text") == "plain text", "text is used as is");
        write_file(ws / "sp.md", "from a file\n\n");
        expect(resolve_system_prompt("@" + (ws / "sp.md").string()) == "from a file", "@path reads the file and trims the end");
        bool threw2 = false;
        try {
            resolve_system_prompt("@" + (ws / "missing.md").string());
        } catch (const std::exception&) {
            threw2 = true;
        }
        expect(threw2, "a missing @file is an error");
        write_file(ws / "proj" / ".maic" / "settings.lua", "return { system_prompt = 'be brief', load_instructions = false }");
        Settings s2 = load_settings(ws / "proj");
        expect(s2.system_prompt == "be brief" && !s2.load_instructions, "settings carry system_prompt and load_instructions");
        fs::remove(ws / "proj" / ".maic" / "settings.lua");
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
