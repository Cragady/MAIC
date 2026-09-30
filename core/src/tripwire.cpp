#include "maic/tripwire.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace maic {

namespace {
constexpr const char* kLockFile = "/var/lib/maic/tripwire";

// Tests point MAIC_TRIPWIRE_FILE at a scratch path so the machine's real lock never decides a test; with it
// set, tripping writes that file directly instead of going through the root helper.
const char* override_file() {
    const char* env = std::getenv("MAIC_TRIPWIRE_FILE");
    return env && *env ? env : nullptr;
}

std::string lock_file() {
    const char* env = override_file();
    return env ? std::string(env) : std::string(kLockFile);
}

std::string g_scope = "machine";
std::filesystem::path g_session_lock;

std::optional<std::string> read_lock(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}
}  // namespace

void set_tripwire_scope(const std::string& scope, const std::filesystem::path& session_lock) {
    g_scope = scope == "session" ? "session" : scope == "isolated" ? "isolated" : "machine";
    g_session_lock = session_lock;
}

bool session_tripped() {
    std::error_code ec;
    return !g_session_lock.empty() && std::filesystem::exists(g_session_lock, ec);
}

bool unlock_session() {
    std::error_code ec;
    if (!g_session_lock.empty()) std::filesystem::remove(g_session_lock, ec);
    return !tripwire_state();
}

std::optional<std::string> tripwire_state() {
    // The machine lock outranks everything, except for a session that opted out of it (isolated: it is
    // confined to its directory and takes no remote work instead).
    if (g_scope != "isolated") {
        if (auto machine = read_lock(lock_file())) return machine;
    }
    if (session_tripped()) return read_lock(g_session_lock.string());
    return std::nullopt;
}

void require_armed(const std::string& action) {
    if (auto state = tripwire_state()) {
        throw std::runtime_error("tripwire is tripped, refusing to " + action + ".\n" + *state +
                                 "Inspect what happened, then run `maic unlock` (asks for your sudo password).");
    }
}

void trip_tripwire(const std::string& reason) {
    if ((g_scope == "session" || g_scope == "isolated") && !g_session_lock.empty()) {
        std::ofstream(g_session_lock, std::ios::trunc) << "session lock (this session only; :unlock removes it)\nreason: " << reason << "\n";
        return;
    }
    if (const char* f = override_file()) {
        std::ofstream(f, std::ios::trunc) << "reason: " << reason << "\n";
        return;
    }
    FILE* p = popen("sudo -n /usr/local/sbin/maic-lock trip >/dev/null 2>&1", "w");
    if (!p) {
        throw std::runtime_error("could not run maic-lock");
    }
    std::fputs(reason.c_str(), p);
    if (pclose(p) != 0) {
        throw std::runtime_error("maic-lock trip failed; is the tripwire installed? (sudo ./harness/install-tripwire.sh)");
    }
}

}  // namespace maic
