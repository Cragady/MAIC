// nvim's lazy-lock.json tracking: path resolution, record, status, diff, the watch's cache, the start notice.
// Everything lives in a temporary HOME with its own XDG directories; the real ~/.config is never read.
#include "check.hpp"

#include "maic/lazy_lock.hpp"
#include "maic/nvim_keymaps.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace maic;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string lock_json(const std::string& lazy_commit, const std::string& telescope_commit, const std::string& plenary_branch, bool gone, bool added) {
    std::string out = "{\n";
    if (gone) out += "  \"gone.nvim\": { \"branch\": \"main\", \"commit\": \"5555555555555555555555555555555555555555\" },\n";
    out += "  \"lazy.nvim\": { \"branch\": \"main\", \"commit\": \"" + lazy_commit + "\" },\n";
    if (added) out += "  \"new.nvim\": { \"branch\": \"main\", \"commit\": \"6666666666666666666666666666666666666666\" },\n";
    out += "  \"plenary.nvim\": { \"branch\": \"" + plenary_branch + "\", \"commit\": \"7777777777777777777777777777777777777777\" },\n";
    out += "  \"telescope.nvim\": { \"branch\": \"master\", \"commit\": \"" + telescope_commit + "\" }\n}\n";
    return out;
}

int command(const std::string& sub, const fs::path& lock, std::string& out) {
    out.clear();
    return lazy_lock_command(sub, lock, out);
}

}  // namespace

int main() {
    fs::path root = fs::temp_directory_path() / ("maic-lazy-lock-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / ".config").c_str(), 1);
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    unsetenv("NVIM_APPNAME");

    section("path resolution");
    expect(lazy_lock_path("") == root / ".config" / "nvim" / "lazy-lock.json", "default: $XDG_CONFIG_HOME/nvim/lazy-lock.json");
    setenv("NVIM_APPNAME", "lazyvim", 1);
    expect(lazy_lock_path("") == root / ".config" / "lazyvim" / "lazy-lock.json", "NVIM_APPNAME picks the config directory");
    unsetenv("NVIM_APPNAME");
    unsetenv("XDG_CONFIG_HOME");
    expect(lazy_lock_path("") == root / ".config" / "nvim" / "lazy-lock.json", "without XDG_CONFIG_HOME: ~/.config/nvim/lazy-lock.json");
    setenv("XDG_CONFIG_HOME", (root / "xdg").c_str(), 1);
    expect(lazy_lock_path("") == root / "xdg" / "nvim" / "lazy-lock.json", "XDG_CONFIG_HOME moves it");
    expect(lazy_lock_hash_path() == root / "xdg" / "maic" / "nvim-lazy-lock.sha256", "the hash file follows XDG_CONFIG_HOME");
    expect(lazy_lock_path("~/dots/lazy-lock.json") == root / "dots" / "lazy-lock.json", "the setting wins, ~ expands");
    expect(lazy_lock_path("/srv/lock.json") == "/srv/lock.json", "an absolute setting is kept");
    expect(lazy_lock_snapshot_path() == root / "state" / "maic" / "lazy-lock" / "recorded.json", "the snapshot is under the state directory");
    write_file(root / "xdg" / "maic" / "settings.lua", "return { lazy_lock = '~/dots/lazy-lock.json', lazy_lock_notice = false }\n");
    fs::create_directories(root / "ws");
    Settings set = load_settings(root / "ws");
    expect(set.lazy_lock == "~/dots/lazy-lock.json" && !set.lazy_lock_notice, "settings carry lazy_lock and lazy_lock_notice");
    expect(Settings{}.lazy_lock.empty() && Settings{}.lazy_lock_notice, "defaults: no path, the notice on");
    setenv("XDG_CONFIG_HOME", (root / ".config").c_str(), 1);

    fs::path lock = lazy_lock_path("");
    std::string out;

    section("quiet when nothing exists");
    {
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Quiet, "no lock file and no hash: quiet");
        expect(lazy_lock_summary(s).empty() && lazy_lock_notice(s).empty(), "no status line, no notice");
        LazyLockWatch w(lock);
        w.check();
        expect(!w.marker(), "no marker");
        expect(command("", lock, out) == 2 && out == "no lazy-lock.json at ~/.config/nvim/lazy-lock.json\n", "the command still says so, exit 2: " + out);
        expect(command("diff", lock, out) == 2, "diff: exit 2");
        expect(command("record", lock, out) == 2 && has(out, "nothing to record") && !fs::exists(lazy_lock_hash_path()), "record without a lock file writes nothing, exit 2");
        expect(command("bogus", lock, out) == 2 && has(out, "usage"), "an unknown subcommand: usage, exit 2");
    }

    section("the hash");
    {
        write_file(root / "abc.json", "abc");
        expect(lazy_lock_state(root / "abc.json").hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of \"abc\" matches the standard vector");
    }

    const std::string c1(40, '1'), c2(40, '2'), t1(40, '3'), t2(40, '4');
    const std::string original = lock_json(c1, t1, "main", true, false);

    section("not recorded");
    write_file(lock, original);
    {
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::NotRecorded, "a lock file and no hash: not recorded");
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json is not recorded yet: maic lazy-lock record", "notice: " + lazy_lock_notice(s));
        expect(command("", lock, out) == 1 && out == "not recorded yet (maic lazy-lock record)\n", "status: exit 1: " + out);
        expect(command("diff", lock, out) == 1 && has(out, "not recorded yet"), "diff: exit 1");
    }

    section("record");
    {
        int rc = command("record", lock, out);
        std::string hash = lazy_lock_state(lock).hash;
        expect(rc == 0 && has(out, "old: none") && has(out, "new: " + hash), "record prints the old and new hash: " + out);
        expect(read_file(lazy_lock_hash_path()) == hash + "  .config/nvim/lazy-lock.json\n", "one sha256sum line, the path relative to ~: " + read_file(lazy_lock_hash_path()));
        expect(read_file(lazy_lock_snapshot_path()) == original, "the snapshot is a copy of the lock file");
        expect(read_file(lock) == original, "the lock file is untouched");
        if (std::system("command -v sha256sum >/dev/null 2>&1") == 0) {
            std::string cmd = "cd '" + root.string() + "' && sha256sum -c --status '" + lazy_lock_hash_path().string() + "'";
            expect(std::system(cmd.c_str()) == 0, "sha256sum -c accepts the hash file from $HOME");
        } else {
            std::cout << "  skip  sha256sum is not installed\n";
        }
        expect(command("record", lock, out) == 0 && has(out, "old: " + hash), "recording again shows the previous hash");
    }

    section("in sync");
    {
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::InSync && lazy_lock_notice(s).empty(), "in sync: no notice");
        expect(command("", lock, out) == 0 && out == "in sync\n", "status: in sync, exit 0");
        expect(command("diff", lock, out) == 0 && has(out, "in sync: nothing changed since"), "diff: exit 0: " + out);
    }

    section("changed");
    write_file(lock, lock_json(c2, t2, "master", false, true));
    {
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Changed && s.snapshot, "changed, with the snapshot to compare");
        expect(s.updated == 3 && s.added == 1 && s.removed == 1 && s.manager_updated, "3 updated, 1 added, 1 removed, lazy.nvim among them");
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json changed since it was recorded: 3 updated, lazy.nvim itself among them, 1 added, 1 removed. maic lazy-lock diff / record", "notice: " + lazy_lock_notice(s));
        expect(command("", lock, out) == 1 && has(out, "changed since ") && has(out, ": 3 updated (lazy.nvim among them), 1 added, 1 removed\nmaic lazy-lock diff / record\n"), "status: exit 1: " + out);
        int rc = command("diff", lock, out);
        expect(rc == 1, "diff: exit 1");
        expect(has(out, "  updated  lazy.nvim  commit 1111111..2222222  (package manager updated)\n"), "diff calls out lazy.nvim as the package manager: " + out);
        expect(out.find("lazy.nvim") < out.find("plenary.nvim"), "lazy.nvim comes first");
        expect(has(out, "  updated  telescope.nvim  commit 3333333..4444444\n"), "a commit change, short hashes");
        expect(has(out, "  updated  plenary.nvim  branch main..master\n"), "a branch change");
        expect(has(out, "  added    new.nvim  main 6666666\n"), "an added plugin");
        expect(has(out, "  removed  gone.nvim  was main 5555555\n"), "a removed plugin");
    }

    section("maic status");
    {
        StatusReport rep;
        expect(format_status(rep).find("lazy-lock") == std::string::npos, "nothing to say: no line");
        rep.lazy_lock = lazy_lock_summary(lazy_lock_state(lock));
        expect(has(format_status(rep), "\nnvim lazy-lock: changed since "), "a line when out of sync: " + format_status(rep));
    }

    section("the start notice");
    {
        LazyLockState s;
        s.kind = LazyLockState::Kind::Changed;
        s.snapshot = true;
        s.updated = 3;
        s.manager_updated = true;
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json changed since it was recorded: 3 updated, lazy.nvim itself among them. maic lazy-lock diff / record", "notice: " + lazy_lock_notice(s));
        s.manager_updated = false;
        s.updated = 0;
        s.added = 2;
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json changed since it was recorded: 2 added. maic lazy-lock diff / record", "only additions: " + lazy_lock_notice(s));
        s.added = 0;
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json changed since it was recorded, though no plugin did (only the file's formatting). maic lazy-lock diff / record", "formatting only: " + lazy_lock_notice(s));
        s.snapshot = false;
        expect(lazy_lock_notice(s) == "nvim's lazy-lock.json changed since it was recorded (no snapshot on this machine to compare plugins). maic lazy-lock diff / record", "no snapshot: " + lazy_lock_notice(s));
    }

    section("the watch rereads only on a change");
    {
        LazyLockWatch w(lock);
        w.check();
        expect(w.reads() == 1 && w.marker(), "first check reads; out of sync sets the marker");
        w.check();
        w.check();
        expect(w.reads() == 1, "an unchanged file is not hashed again");
        write_file(lock, original);
        LazyLockState s = w.check();
        expect(w.reads() == 2 && s.kind == LazyLockState::Kind::InSync && !w.marker(), "a changed size or mtime rereads; back in sync clears the marker");
        write_file(lock, lock_json(c2, t1, "main", true, false));
        w.check();
        expect(w.reads() == 3 && w.marker(), "the lazy.nvim update sets it again");
        command("record", lock, out);
        w.check();
        expect(w.reads() == 4 && !w.marker(), "a record (the hash file changes) rereads and clears it");
        write_file(lock, original);
    }

    section("formatting only");
    {
        command("record", lock, out);
        std::string compact = original;
        compact.erase(std::remove(compact.begin(), compact.end(), '\n'), compact.end());
        write_file(lock, compact);
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Changed && s.snapshot && s.updated + s.added + s.removed == 0, "a reformatted file changes the hash, no plugin");
        expect(command("diff", lock, out) == 1 && has(out, "no plugin changed"), "diff says no plugin changed: " + out);
        write_file(lock, original);
    }

    section("no snapshot on this machine");
    {
        fs::remove(lazy_lock_snapshot_path());
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::InSync, "the hash alone decides in sync");
        expect(command("diff", lock, out) == 0 && has(out, "no snapshot of the recorded lock file on this machine") && has(out, "the hashes match: in sync"), "diff says so and the hashes match, exit 0: " + out);
        write_file(lock, lock_json(c2, t1, "main", true, false));
        s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Changed && !s.snapshot, "changed, counts unknown");
        expect(has(lazy_lock_summary(s), "no snapshot here to count plugins"), "summary: " + lazy_lock_summary(s));
        expect(command("diff", lock, out) == 1 && has(out, "no snapshot") && has(out, "the hashes differ: recorded "), "diff: the hashes differ, exit 1: " + out);
        write_file(lazy_lock_snapshot_path(), "{}");
        expect(command("diff", lock, out) == 1 && has(out, "is not the recorded lock file"), "a snapshot of something else is not used: " + out);
        expect(!lazy_lock_state(lock).snapshot, "nor counted");
    }

    section("lock file gone, or a broken hash file");
    {
        fs::remove(lock);
        LazyLockState s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Missing && lazy_lock_exit_code(s) == 2, "recorded but no lock file: exit 2");
        expect(has(lazy_lock_notice(s), "is gone from ~/.config/nvim/lazy-lock.json"), "notice: " + lazy_lock_notice(s));
        LazyLockWatch w(lock);
        w.check();
        expect(w.marker(), "the marker shows");
        expect(command("", lock, out) == 2 && has(out, "no lazy-lock.json at ~/.config/nvim/lazy-lock.json (a hash was recorded"), "status: " + out);
        write_file(lock, original);
        write_file(lazy_lock_hash_path(), "not a hash\n");
        s = lazy_lock_state(lock);
        expect(s.kind == LazyLockState::Kind::Error && command("", lock, out) == 2 && has(out, "does not start with a SHA-256"), "a broken hash file is an error, exit 2: " + out);
    }

    section("the keymap check's record, for the re-check after a lock change");
    {
        auto report = [](std::vector<std::pair<std::string, std::string>> items) {
            nlohmann::json sections = nlohmann::json::array();
            nlohmann::json list = nlohmann::json::array();
            for (const auto& [level, id] : items) list.push_back({{"level", level}, {"text", id + " text"}, {"hint", "fix " + id}, {"id", level == "ok" ? "" : id}});
            sections.push_back({{"name", "llama.vim"}, {"items", list}});
            return parse_keymap_report({{"sections", sections}, {"hash", "h" + std::to_string(items.size())}, {"nvim", "0.12.2"}});
        };
        KeymapReport first = report({{"ok", "a"}, {"warn", "llama.vim|<leader>llf"}});
        expect(first.error.empty() && first.items.size() == 2 && first.collisions().size() == 1 && keymap_exit_code(first) == 1, "a warn is a collision: exit 1");
        expect(keymap_summary(first) == "1 collision: maic nvim keymaps", "doctor's line: " + keymap_summary(first));
        std::string text = format_keymap_report(first, false);
        expect(has(text, "1 collision") && has(text, "  WARN   llama.vim|<leader>llf text") && has(text, "fix: fix llama.vim|<leader>llf") && !has(text, "a text"), "the CLI lists the collisions with their fixes: " + text);
        expect(has(format_keymap_report(first, true), "  ok     a text"), "--all lists every key");
        expect(!load_keymap_record().exists, "no record before the first check");
        save_keymap_record(first, "lock1");
        KeymapRecord rec = load_keymap_record();
        expect(rec.exists && rec.lock_hash == "lock1" && rec.ids == std::vector<std::string>{"llama.vim|<leader>llf"}, "the record keeps the lock hash and the collision ids");
        KeymapReport second = report({{"warn", "llama.vim|<leader>llf"}, {"error", "MAIC's terminal input|<C-w>"}});
        auto fresh = new_collisions(rec, second);
        expect(fresh.size() == 1 && fresh[0].id == "MAIC's terminal input|<C-w>", "only the collision the record did not have is new");
        KeymapReport broken = parse_keymap_report({{"error", "boom"}});
        expect(keymap_exit_code(broken) == 2 && has(keymap_summary(broken), "boom"), "a check that failed inside nvim: exit 2");
        KeymapReport missing = run_keymap_check("", nullptr, std::chrono::seconds(10), "/nonexistent/nvim");
        expect(keymap_exit_code(missing) == 2 && has(missing.error, "can't run /nonexistent/nvim"), "no nvim: exit 2 and why: " + missing.error);
    }

    fs::remove_all(root);
    return finish();
}
