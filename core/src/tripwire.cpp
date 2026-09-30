#include "maic/tripwire.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace maic {

namespace {
constexpr const char* kLockFile = "/var/lib/maic/tripwire";
}

std::optional<std::string> tripwire_state() {
    std::ifstream in(kLockFile);
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
