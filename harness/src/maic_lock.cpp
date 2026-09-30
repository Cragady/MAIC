// maic-lock: sets and clears MAIC's tripwire. Installed root-owned in /usr/local/sbin and run via sudo:
//   sudo maic-lock trip     (passwordless sudoers rule; reason is read from stdin)
//   sudo maic-lock reset    (password required)
//   maic-lock status        (no sudo needed)
// The lock path is fixed and nothing from the caller is used as a path.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

constexpr const char* kLockDir = "/var/lib/maic";
constexpr const char* kLockFile = "/var/lib/maic/tripwire";
constexpr size_t kMaxReason = 500;

std::string read_reason() {
    std::string raw((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    std::string out;
    for (char c : raw) {
        if (out.size() >= kMaxReason) {
            break;
        }
        out += (c == '\n' || c == '\t') ? ' ' : (c >= 0x20 && c < 0x7f ? c : '?');
    }
    return out.empty() ? "(no reason given)" : out;
}

int status() {
    std::ifstream in(kLockFile);
    if (!in) {
        std::cout << "armed\n";
        return 0;
    }
    std::cout << "TRIPPED\n" << in.rdbuf();
    return 3;
}

int trip() {
    if (access(kLockFile, F_OK) == 0) {
        std::cout << "already tripped\n";
        return 0;
    }
    mkdir(kLockDir, 0755);
    const char* who = std::getenv("SUDO_USER");
    std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    umask(022);
    std::ofstream out(kLockFile, std::ios::trunc);
    out << "time: " << when << "\n"
        << "user: " << (who ? who : "root") << "\n"
        << "reason: " << read_reason() << "\n";
    if (!out) {
        std::cerr << "maic-lock: could not write " << kLockFile << " (run through sudo)\n";
        return 1;
    }
    std::cout << "tripped\n";
    return 0;
}

int reset() {
    if (unlink(kLockFile) != 0) {
        if (access(kLockFile, F_OK) != 0) {
            std::cout << "not tripped\n";
            return 0;
        }
        std::cerr << "maic-lock: could not remove " << kLockFile << " (run through sudo)\n";
        return 1;
    }
    std::cout << "reset\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cmd = argc == 2 ? argv[1] : "";
    if (cmd == "status") {
        return status();
    }
    if (cmd == "trip") {
        return trip();
    }
    if (cmd == "reset") {
        return reset();
    }
    std::cerr << "usage: maic-lock trip|reset|status\n";
    return 2;
}
