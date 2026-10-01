#include "maic/nvim_keymaps.hpp"

#include "maic/paths.hpp"
#include "nvim_run.hpp"

#include <sys/wait.h>

#include <cstdlib>
#include <fstream>
#include <set>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

std::vector<KeymapItem> KeymapReport::collisions() const {
    std::vector<KeymapItem> out;
    for (const auto& i : items) {
        if (i.level == "warn" || i.level == "error") out.push_back(i);
    }
    return out;
}

KeymapReport parse_keymap_report(const json& j) {
    KeymapReport r;
    if (!j.is_object()) {
        r.error = "the check answered with something that is not a JSON object";
        return r;
    }
    if (j.contains("error")) {
        r.error = "the check failed inside nvim: " + j.value("error", std::string("?"));
        return r;
    }
    for (const auto& s : j.value("sections", json::array())) {
        for (const auto& i : s.value("items", json::array())) {
            r.items.push_back({s.value("name", ""), i.value("level", ""), i.value("text", ""), i.value("hint", ""), i.value("id", "")});
        }
    }
    r.hash = j.value("hash", "");
    r.nvim = j.value("nvim", "");
    return r;
}

KeymapReport run_keymap_check(const std::string& config, const std::atomic<bool>* cancel, std::chrono::seconds timeout, const std::string& nvim) {
    KeymapReport r;
    fs::path script = root_dir() / "maic.nvim" / "lua" / "maic" / "keymaps.lua";
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) {
        r.error = "maic.nvim is not at " + script.parent_path().parent_path().parent_path().string();
        return r;
    }
    std::string tmpl = (fs::temp_directory_path() / "maic-keymaps-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) {
        r.error = "can't create a temporary directory for the nvim run";
        return r;
    }
    fs::path dir = tmpl, out = dir / "report.json";
    std::vector<std::string> args = {nvim, "--headless", "-i", "NONE", "-n", "-V1" + (dir / "verbose.log").string()};
    if (!config.empty()) args.insert(args.end(), {"-u", config});
    args.insert(args.end(), {"--cmd", "let g:maic_keymap_check = 1", "-c", "lua dofile(os.getenv('MAIC_KEYMAP_SCRIPT')).headless(os.getenv('MAIC_KEYMAP_OUT'))"});
    try {
        // $NVIM is dropped: the check is no job of the nvim MAIC may run inside.
        NvimRun run = run_nvim_child(args, {"MAIC_KEYMAP_", "NVIM", "NVIM_LISTEN_ADDRESS"},
                                     {"MAIC_KEYMAP_SCRIPT=" + script.string(), "MAIC_KEYMAP_OUT=" + out.string()}, timeout, cancel);
        if (run.cancelled) r.error = "stopped";
        else if (run.timed_out) r.error = nvim + " did not finish within " + std::to_string(timeout.count()) + " s and was stopped (a plugin manager installing in headless mode?)";
        else if (!fs::is_regular_file(out, ec)) {
            r.error = WIFEXITED(run.status) && WEXITSTATUS(run.status) == 127
                          ? "can't run " + nvim + " (is neovim installed and on PATH?)"
                          : nvim + " exited without an answer (status " + std::to_string(WIFEXITED(run.status) ? WEXITSTATUS(run.status) : 128 + WTERMSIG(run.status)) + ")";
        } else {
            std::ifstream in(out);
            json j = json::parse(in, nullptr, false);
            r = parse_keymap_report(j.is_discarded() ? json() : j);
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    fs::remove_all(dir, ec);
    return r;
}

std::string format_keymap_report(const KeymapReport& r, bool all) {
    if (!r.error.empty()) return "nvim keymaps: " + r.error + "\n";
    auto collisions = r.collisions();
    std::string out = "nvim " + r.nvim + " with your configuration: " +
                      (collisions.empty() ? "no collisions" : std::to_string(collisions.size()) + (collisions.size() == 1 ? " collision" : " collisions")) + "\n";
    std::string section;
    for (const auto& i : r.items) {
        bool collision = i.level == "warn" || i.level == "error";
        if (!all && !collision) continue;
        if (i.section != section) out += "\n" + (section = i.section) + "\n";
        std::string tag = i.level == "error" ? "ERROR" : i.level == "warn" ? "WARN" : i.level == "info" ? "info" : "ok";
        out += "  " + tag + std::string(7 - tag.size(), ' ') + i.text + "\n";
        if (!i.hint.empty()) out += "         fix: " + i.hint + "\n";
    }
    out += "\n:checkhealth maic in nvim shows the same" + std::string(all ? "" : " with every key (maic nvim keymaps --all here)") + "\n";
    return out;
}

std::string keymap_summary(const KeymapReport& r) {
    if (!r.error.empty()) return r.error;
    size_t n = r.collisions().size();
    if (n == 0) return "no collisions";
    return std::to_string(n) + (n == 1 ? " collision" : " collisions") + ": maic nvim keymaps";
}

int keymap_exit_code(const KeymapReport& r) {
    if (!r.error.empty()) return 2;
    return r.collisions().empty() ? 0 : 1;
}

fs::path keymap_record_path() {
    return state_dir() / "nvim" / "keymaps.json";
}

KeymapRecord load_keymap_record() {
    KeymapRecord rec;
    std::ifstream in(keymap_record_path());
    if (!in) return rec;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return rec;
    rec.exists = true;
    rec.lock_hash = j.value("lazy_lock", "");
    rec.hash = j.value("hash", "");
    rec.error = j.value("error", "");
    for (const auto& id : j.value("collisions", json::array())) {
        if (id.is_string()) rec.ids.push_back(id.get<std::string>());
    }
    return rec;
}

void save_keymap_record(const KeymapReport& r, const std::string& lock_hash) {
    json ids = json::array();
    for (const auto& c : r.collisions()) ids.push_back(c.id);
    json j = {{"lazy_lock", lock_hash}, {"hash", r.hash}, {"collisions", ids}};
    if (!r.error.empty()) j["error"] = r.error;
    fs::create_directories(keymap_record_path().parent_path());
    std::ofstream(keymap_record_path()) << j.dump(2) << "\n";
}

std::vector<KeymapItem> new_collisions(const KeymapRecord& before, const KeymapReport& now) {
    std::set<std::string> old(before.ids.begin(), before.ids.end());
    std::vector<KeymapItem> out;
    for (const auto& c : now.collisions()) {
        if (!old.count(c.id)) out.push_back(c);
    }
    return out;
}

}  // namespace maic
