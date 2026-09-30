#include "maic/paths.hpp"

#include <cstdlib>
#include <stdexcept>

namespace maic {

namespace {

std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        throw std::runtime_error("HOME is not set");
    }
    return home;
}

}  // namespace

// MAIC_HOME, else the source tree this binary was built from, else (an installed copy) <prefix>/share/maic.
std::filesystem::path root_dir() {
    if (const char* env = std::getenv("MAIC_HOME"); env && *env) {
        return env;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(MAIC_ROOT) / "services", ec)) {
        return MAIC_ROOT;
    }
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        std::filesystem::path share = exe.parent_path().parent_path() / "share" / "maic";
        if (std::filesystem::is_directory(share / "services", ec)) return share;
    }
    return MAIC_ROOT;
}

std::filesystem::path state_dir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "maic";
    }
    return home_dir() / ".local" / "state" / "maic";
}

std::string expand_vars(std::string_view text) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t open = text.find("${", pos);
        if (open == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        size_t close = text.find('}', open + 2);
        if (close == std::string_view::npos) {
            throw std::runtime_error("unterminated ${ in: " + std::string(text));
        }
        out.append(text.substr(pos, open - pos));
        std::string name(text.substr(open + 2, close - open - 2));
        // MAIC's own locations first, so service files never hard-code them.
        if (name == "MAIC_VENDOR") out.append((state_dir() / "vendor").string());
        else if (name == "MAIC_STATE") out.append(state_dir().string());
        else if (name == "MAIC_ROOT") out.append(root_dir().string());
        else {
            const char* value = std::getenv(name.c_str());
            if (!value) {
                throw std::runtime_error("environment variable " + name + " is not set");
            }
            out.append(value);
        }
        pos = close + 1;
    }
    return out;
}

}  // namespace maic
