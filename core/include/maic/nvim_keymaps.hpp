#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace maic {

// `maic nvim keymaps`, the doctor's line and the re-check after a lazy-lock.json change: maic.nvim's keymap check
// (maic.nvim/lua/maic/keymaps.lua, what :checkhealth maic shows) run in a headless nvim with the user's own
// configuration. docs/nvim.md.

struct KeymapItem {
    std::string section, level, text, hint;  // level: ok, info, warn, error
    std::string id;                          // a collision's name without line numbers; "" for ok and info
};

struct KeymapReport {
    std::string error;  // nvim could not run, timed out or gave no answer; nothing else is then set
    std::vector<KeymapItem> items;
    std::string hash;  // sha256 of the sorted collision ids
    std::string nvim;  // nvim's version
    std::vector<KeymapItem> collisions() const;  // the warn and error items
};

// The JSON maic.nvim's check writes, as a report.
KeymapReport parse_keymap_report(const nlohmann::json& j);
// Runs the check: `nvim --headless -i NONE -n` with stdin closed, -V1 so a mapping made from Lua names its file,
// `--cmd 'let g:maic_keymap_check = 1'` (maic.nvim then plans its keys and sets none), User VeryLazy dispatched
// first so lazy-loaded maps appear. `config` non-empty: `-u config` instead of the user's init. Stopped at `timeout`
// or when `cancel` turns true.
KeymapReport run_keymap_check(const std::string& config = "", const std::atomic<bool>* cancel = nullptr,
                              std::chrono::seconds timeout = std::chrono::seconds(60), const std::string& nvim = "nvim");
// The text `maic nvim keymaps` prints: the collisions with their fixes (`all`: every key).
std::string format_keymap_report(const KeymapReport& r, bool all);
// One line for `maic doctor`: "no collisions", "2 collisions: maic nvim keymaps", or the error.
std::string keymap_summary(const KeymapReport& r);
// 0 no collisions, 1 collisions, 2 the check could not run.
int keymap_exit_code(const KeymapReport& r);

// <state>/nvim/keymaps.json: the last check's collision ids and hash, and the lazy-lock.json hash it ran at.
std::filesystem::path keymap_record_path();
struct KeymapRecord {
    bool exists = false;
    std::string lock_hash, hash, error;
    std::vector<std::string> ids;
};
KeymapRecord load_keymap_record();
void save_keymap_record(const KeymapReport& r, const std::string& lock_hash);
// The collisions in `now` that `before` did not have.
std::vector<KeymapItem> new_collisions(const KeymapRecord& before, const KeymapReport& now);

}  // namespace maic
