#include "maic/lazy_lock.hpp"

#include "maic/paths.hpp"
#include "maic/settings.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "";
}

// ~/.config/nvim/lazy-lock.json for a person to read.
std::string tilde(const fs::path& p) {
    std::string s = p.string(), h = home();
    if (!h.empty() && s.rfind(h + "/", 0) == 0) return "~" + s.substr(h.size());
    return s;
}

// "" when the file cannot be read.
std::string sha256_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr);
    char buf[65536];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) EVP_DigestUpdate(ctx.get(), buf, static_cast<size_t>(in.gcount()));
    if (in.bad()) return "";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_DigestFinal_ex(ctx.get(), md, &n);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) out += {hex[md[i] >> 4], hex[md[i] & 15]};
    return out;
}

std::string short_hash(const json& entry, const char* key) {
    std::string v = entry.is_object() && entry.contains(key) && entry[key].is_string() ? entry[key].get<std::string>() : "";
    return v.empty() ? "?" : std::string(key) == "commit" ? v.substr(0, 7) : v;
}

struct Change {
    std::string what;  // updated, added, removed
    std::string name;
    json before, after;
};

// Per plugin, in name order with lazy.nvim first. False when either file is not a JSON object.
bool compare(const fs::path& before_file, const fs::path& after_file, std::vector<Change>& out) {
    std::ifstream b(before_file), a(after_file);
    json before = json::parse(b, nullptr, false), after = json::parse(a, nullptr, false);
    if (!before.is_object() || !after.is_object()) return false;
    for (const auto& [name, entry] : after.items()) {
        if (!before.contains(name)) out.push_back({"added", name, nullptr, entry});
        else if (before[name] != entry) out.push_back({"updated", name, before[name], entry});
    }
    for (const auto& [name, entry] : before.items()) {
        if (!after.contains(name)) out.push_back({"removed", name, entry, nullptr});
    }
    std::stable_partition(out.begin(), out.end(), [](const Change& c) { return c.name == "lazy.nvim"; });
    return true;
}

std::string counts(const LazyLockState& s) {
    return std::to_string(s.updated) + " updated" + (s.manager_updated ? " (lazy.nvim among them)" : "") + ", " + std::to_string(s.added) + " added, " + std::to_string(s.removed) + " removed";
}

// The recorded hash: the first word of the hash file's first line, which must be 64 hex characters.
bool read_hash_file(const fs::path& p, std::string& hash) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    hash = line.substr(0, line.find_first_of(" \t"));
    for (auto& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string file_date(const fs::path& p) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return "?";
    char buf[16];
    std::tm tm{};
    localtime_r(&st.st_mtime, &tm);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

int record(const fs::path& lock, std::string& out) {
    std::string hash = sha256_file(lock);
    if (hash.empty()) {
        out = "no lazy-lock.json at " + tilde(lock) + "; nothing to record\n";
        return 2;
    }
    std::string old;
    bool had = fs::exists(lazy_lock_hash_path()) && read_hash_file(lazy_lock_hash_path(), old);
    // The path as sha256sum -c sees it from $HOME (relative), so the line is the same on every machine.
    std::string shown = tilde(lock);
    if (shown.rfind("~/", 0) == 0) shown = shown.substr(2);
    fs::create_directories(lazy_lock_hash_path().parent_path());
    // Written in place, so a hash file that is a symlink into a dotfiles repository stays one.
    std::ofstream(lazy_lock_hash_path()) << hash << "  " << shown << "\n";
    fs::create_directories(lazy_lock_snapshot_path().parent_path());
    fs::copy_file(lock, lazy_lock_snapshot_path(), fs::copy_options::overwrite_existing);
    out = "recorded " + tilde(lock) + "\n  old: " + (had ? old : "none") + "\n  new: " + hash + "\nhash file: " + tilde(lazy_lock_hash_path()) +
          " (commit it with your dotfiles; sha256sum -c " + tilde(lazy_lock_hash_path()) + " works from ~)\nsnapshot: " + tilde(lazy_lock_snapshot_path()) + "\n";
    return 0;
}

int diff(const LazyLockState& s, std::string& out) {
    if (s.kind != LazyLockState::Kind::InSync && s.kind != LazyLockState::Kind::Changed) {
        std::string line = lazy_lock_summary(s);
        out = (line.empty() ? "no lazy-lock.json at " + tilde(s.lock) : line) + "\n";
        return lazy_lock_exit_code(s);
    }
    fs::path snap = lazy_lock_snapshot_path();
    bool matches = sha256_file(snap) == s.recorded;
    if (!matches) {
        out = (fs::exists(snap) ? "the snapshot here (" + tilde(snap) + ") is not the recorded lock file (recorded on another machine?)"
                                : "no snapshot of the recorded lock file on this machine (" + tilde(snap) + ")") +
              ", so there are no plugins to compare.\n";
        if (s.kind == LazyLockState::Kind::InSync) out += "the hashes match: in sync\n";
        else out += "the hashes differ: recorded " + s.recorded + ", now " + s.hash + "\n";
        out += "maic lazy-lock record keeps a snapshot here from now on\n";
        return lazy_lock_exit_code(s);
    }
    if (s.kind == LazyLockState::Kind::InSync) {
        out = "in sync: nothing changed since " + s.recorded_on + "\n";
        return 0;
    }
    std::vector<Change> changes;
    if (!compare(snap, s.lock, changes)) {
        out = "the hashes differ, but " + tilde(s.lock) + " or the snapshot is not a JSON object, so there are no plugins to compare\n";
        return 1;
    }
    out = tilde(s.lock) + " against the snapshot recorded " + s.recorded_on + ":\n";
    for (const auto& c : changes) {
        std::string line = "  " + c.what + std::string(9 - c.what.size(), ' ') + c.name + "  ";
        if (c.what == "added") line += short_hash(c.after, "branch") + " " + short_hash(c.after, "commit");
        else if (c.what == "removed") line += "was " + short_hash(c.before, "branch") + " " + short_hash(c.before, "commit");
        else {
            std::string parts;
            if (short_hash(c.before, "branch") != short_hash(c.after, "branch")) parts += "branch " + short_hash(c.before, "branch") + ".." + short_hash(c.after, "branch");
            if (short_hash(c.before, "commit") != short_hash(c.after, "commit")) parts += std::string(parts.empty() ? "" : ", ") + "commit " + short_hash(c.before, "commit") + ".." + short_hash(c.after, "commit");
            line += parts.empty() ? "entry changed" : parts;
        }
        if (c.name == "lazy.nvim") line += "  (package manager updated)";
        out += line + "\n";
    }
    if (changes.empty()) out += "  no plugin changed (only the file's formatting)\n";
    out += counts(s) + ". maic lazy-lock record once you are happy with it\n";
    return 1;
}

}  // namespace

fs::path lazy_lock_path(const std::string& setting) {
    if (!setting.empty()) {
        if (setting == "~" || setting.rfind("~/", 0) == 0) return home() + setting.substr(1);
        return setting;
    }
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path config = xdg && *xdg ? fs::path(xdg) : fs::path(home()) / ".config";
    const char* app = std::getenv("NVIM_APPNAME");
    return config / (app && *app ? app : "nvim") / "lazy-lock.json";
}

fs::path lazy_lock_hash_path() {
    return settings_path().parent_path() / "nvim-lazy-lock.sha256";
}

fs::path lazy_lock_snapshot_path() {
    return state_dir() / "lazy-lock" / "recorded.json";
}

LazyLockState lazy_lock_state(const fs::path& lock) {
    LazyLockState s;
    s.lock = lock;
    std::error_code ec;
    fs::path hash_file = lazy_lock_hash_path();
    bool hash_exists = fs::exists(hash_file, ec);
    bool hash_ok = hash_exists && read_hash_file(hash_file, s.recorded);
    if (hash_exists) s.recorded_on = file_date(hash_file);
    if (!fs::exists(lock, ec)) {
        s.kind = hash_exists ? LazyLockState::Kind::Missing : LazyLockState::Kind::Quiet;
        return s;
    }
    s.hash = sha256_file(lock);
    if (s.hash.empty()) {
        s.kind = LazyLockState::Kind::Error;
        s.error = "cannot read " + tilde(lock);
        return s;
    }
    if (!hash_exists) {
        s.kind = LazyLockState::Kind::NotRecorded;
        return s;
    }
    if (!hash_ok) {
        s.kind = LazyLockState::Kind::Error;
        s.error = tilde(hash_file) + " does not start with a SHA-256 (maic lazy-lock record writes it again)";
        return s;
    }
    if (s.hash == s.recorded) {
        s.kind = LazyLockState::Kind::InSync;
        return s;
    }
    s.kind = LazyLockState::Kind::Changed;
    std::vector<Change> changes;
    fs::path snap = lazy_lock_snapshot_path();
    if (sha256_file(snap) == s.recorded && compare(snap, lock, changes)) {
        s.snapshot = true;
        for (const auto& c : changes) {
            (c.what == "added" ? s.added : c.what == "removed" ? s.removed : s.updated)++;
            if (c.name == "lazy.nvim") s.manager_updated = true;
        }
    }
    return s;
}

std::string lazy_lock_summary(const LazyLockState& s) {
    switch (s.kind) {
        case LazyLockState::Kind::Quiet: return "";
        case LazyLockState::Kind::Missing: return "no lazy-lock.json at " + tilde(s.lock) + " (a hash was recorded " + s.recorded_on + ")";
        case LazyLockState::Kind::NotRecorded: return "not recorded yet (maic lazy-lock record)";
        case LazyLockState::Kind::InSync: return "in sync";
        case LazyLockState::Kind::Changed:
            return "changed since " + s.recorded_on + (s.snapshot ? ": " + counts(s) : " (no snapshot here to count plugins; maic lazy-lock diff)");
        case LazyLockState::Kind::Error: return s.error;
    }
    return "";
}

std::string lazy_lock_notice(const LazyLockState& s) {
    const std::string tail = ". maic lazy-lock diff / record";
    switch (s.kind) {
        case LazyLockState::Kind::Quiet:
        case LazyLockState::Kind::InSync: return "";
        case LazyLockState::Kind::Missing: return "nvim's lazy-lock.json is gone from " + tilde(s.lock) + ", but a hash of it was recorded. maic lazy-lock";
        case LazyLockState::Kind::NotRecorded: return "nvim's lazy-lock.json is not recorded yet: maic lazy-lock record";
        case LazyLockState::Kind::Error: return "nvim's lazy-lock.json: " + s.error;
        case LazyLockState::Kind::Changed: break;
    }
    std::string out = "nvim's lazy-lock.json changed since it was recorded";
    if (!s.snapshot) return out + " (no snapshot on this machine to compare plugins)" + tail;
    std::vector<std::string> parts;
    if (s.updated) parts.push_back(std::to_string(s.updated) + " updated" + (s.manager_updated ? ", lazy.nvim itself among them" : ""));
    else if (s.manager_updated) parts.push_back("lazy.nvim itself among the changes");
    if (s.added) parts.push_back(std::to_string(s.added) + " added");
    if (s.removed) parts.push_back(std::to_string(s.removed) + " removed");
    if (parts.empty()) return out + ", though no plugin did (only the file's formatting)" + tail;
    out += ": ";
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out + tail;
}

int lazy_lock_exit_code(const LazyLockState& s) {
    switch (s.kind) {
        case LazyLockState::Kind::InSync: return 0;
        case LazyLockState::Kind::Changed:
        case LazyLockState::Kind::NotRecorded: return 1;
        default: return 2;
    }
}

int lazy_lock_command(const std::string& sub, const fs::path& lock, std::string& out) {
    try {
        if (sub == "record") return record(lock, out);
        LazyLockState s = lazy_lock_state(lock);
        if (sub == "diff") return diff(s, out);
        if (!sub.empty()) {
            out = "usage: maic lazy-lock [record|diff]\n";
            return 2;
        }
        std::string line = lazy_lock_summary(s);
        out = (line.empty() ? "no lazy-lock.json at " + tilde(lock) : line) + "\n";
        if (s.kind == LazyLockState::Kind::Changed) out += "maic lazy-lock diff / record\n";
        return lazy_lock_exit_code(s);
    } catch (const std::exception& e) {
        out = std::string("lazy-lock: ") + e.what() + "\n";
        return 2;
    }
}

LazyLockState LazyLockWatch::check() {
    std::string key;
    for (const auto& p : {lock_, lazy_lock_hash_path(), lazy_lock_snapshot_path()}) {
        struct stat st{};
        if (stat(p.c_str(), &st) != 0) key += "-;";
        else key += std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec) + ":" + std::to_string(st.st_size) + ";";
    }
    std::lock_guard lock(mu_);
    if (reads_ > 0 && key == key_) return state_;
    key_ = key;
    state_ = lazy_lock_state(lock_);
    ++reads_;
    marker_ = state_.kind == LazyLockState::Kind::Changed || state_.kind == LazyLockState::Kind::Missing;
    return state_;
}

}  // namespace maic
