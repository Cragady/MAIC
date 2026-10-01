#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace maic {

// nvim's lazy-lock.json (lazy.nvim's plugin pins), watched so a plugin or package manager update never goes
// unnoticed. docs/lazy-lock.md. Nothing here runs nvim, writes the lock file or touches the network.

// `lazy_lock` from settings when set (a leading ~ expands), else $XDG_CONFIG_HOME (~/.config) /
// $NVIM_APPNAME (nvim) / lazy-lock.json.
std::filesystem::path lazy_lock_path(const std::string& setting);
// <config>/maic/nvim-lazy-lock.sha256: one sha256sum line, the file to commit with the dotfiles.
std::filesystem::path lazy_lock_hash_path();
// <state>/lazy-lock/recorded.json: the lock file as it was when recorded, for per-plugin diffs; not for git.
std::filesystem::path lazy_lock_snapshot_path();

struct LazyLockState {
    // Quiet: no lock file and nothing recorded (a machine without lazy.nvim). Missing: no lock file, but a hash
    // was recorded. Error: the hash file or the lock file could not be read.
    enum class Kind { Quiet, Missing, NotRecorded, InSync, Changed, Error };
    Kind kind = Kind::Quiet;
    std::filesystem::path lock;
    std::string hash;         // the lock file's SHA-256 now
    std::string recorded;     // the one in the hash file
    std::string recorded_on;  // the hash file's date, 2026-10-01
    bool snapshot = false;    // Changed: a snapshot of the recorded file is here, so the counts are known
    int updated = 0, added = 0, removed = 0;
    bool manager_updated = false;  // lazy.nvim's own entry is among the changes
    std::string error;
};

LazyLockState lazy_lock_state(const std::filesystem::path& lock);
// One line for `maic status` and `maic doctor`: "in sync", "changed since 2026-09-30: 3 updated, ...". "" when Quiet.
std::string lazy_lock_summary(const LazyLockState& s);
// The TUI's start notice: "" when in sync or quiet.
std::string lazy_lock_notice(const LazyLockState& s);
// 0 in sync, 1 changed or not recorded, 2 no lock file or an error.
int lazy_lock_exit_code(const LazyLockState& s);

// `maic lazy-lock [record|diff]` and `:lazylock`: fills `out` with the text to show and returns the exit code.
int lazy_lock_command(const std::string& sub, const std::filesystem::path& lock, std::string& out);

// The state kept between checks: check() reads the files again only when the lock file, the hash file or the
// snapshot changed size or mtime since the last call. Safe to call from the UI and the agent's thread.
class LazyLockWatch {
public:
    explicit LazyLockWatch(std::filesystem::path lock) : lock_(std::move(lock)) {}
    LazyLockState check();
    bool marker() const { return marker_; }  // out of sync at the last check: the status strip's lock≠
    int reads() const { return reads_; }     // how often check() read and hashed the files

private:
    std::filesystem::path lock_;
    std::mutex mu_;
    std::string key_;
    LazyLockState state_;
    std::atomic<bool> marker_{false};
    std::atomic<int> reads_{0};
};

}  // namespace maic
