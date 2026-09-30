#include "maic/tripwire.hpp"

#include <cstdio>
#include <cstdlib>
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
}  // namespace

std::optional<std::string> tripwire_state() {
    std::ifstream in(lock_file());
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

void require_armed(const std::string& action) {
    if (auto state = tripwire_state()) {
        throw std::runtime_error("tripwire is tripped, refusing to " + action + ".\n" + *state +
                                 "Inspect what happened, then run `maic unlock` (asks for your sudo password).");
    }
}

void trip_tripwire(const std::string& reason) {
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
