// Hostile and broken inputs must give an error result, never a crash or a hang.
// Add a case here for every crash found in the wild (the first one: std::regex overflowing the stack).
#include "check.hpp"

#include "maic/artifacts.hpp"
#include "maic/harness.hpp"
#include "maic/instructions.hpp"
#include "maic/sandbox.hpp"
#include "maic/session.hpp"
#include "maic/tools.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    expect(tool(h, "read_file", {{"path", "binary.bin"}}).ok, "read_file on a binary file returns something");
    expect(!tool(h, "read_file", {{"path", "missing.txt"}}).ok, "read_file on a missing file -> error");
    fs::create_directories(ws / "adir");
    expect(!tool(h, "list_dir", {{"path", "binary.bin"}}).ok, "list_dir on a file -> error");
    expect(!tool(h, "list_dir", {{"path", "nope"}}).ok, "list_dir on a missing directory -> error");
    write_file(ws / "small.txt", "one\ntwo\n");
    expect(tool(h, "read_file", {{"path", "small.txt"}, {"offset", 1000000}}).ok, "read_file far past the end is fine");
    expect(!tool(h, "edit_file", {{"path", "small.txt"}, {"old_string", ""}, {"new_string", "x"}}).ok, "edit_file with an empty old_string -> error");
    expect(tool(h, "write_file", {{"path", "deep/er/still/new.txt"}, {"content", "x"}}).ok, "write_file creates missing parent directories");

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
