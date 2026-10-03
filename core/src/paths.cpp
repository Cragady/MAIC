#include "maid/paths.hpp"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace maid {

namespace {

std::filesystem::path installed_root() {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return exe.parent_path().parent_path() / "share" / "maid";
}

}  // namespace

std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        throw std::runtime_error("HOME is not set");
    }
    return home;
}

// MAID_HOME, else (an installed copy) <prefix>/share/maid, else the source tree this binary was built from. An
// installed release reads the files it shipped with, never a checkout that has moved on since.
std::filesystem::path root_dir() {
    if (const char* env = std::getenv("MAID_HOME"); env && *env) {
        return env;
    }
    std::filesystem::path share = installed_root();
    std::error_code ec;
    if (!share.empty() && std::filesystem::is_directory(share / "services", ec)) return share;
    return MAID_ROOT;
}

std::string root_dir_looked() {
    if (const char* env = std::getenv("MAID_HOME"); env && *env) {
        return "MAID_HOME (" + std::string(env) + ")";
    }
    std::filesystem::path share = installed_root();
    return (share.empty() ? "" : share.string() + " and ") + MAID_ROOT;
}

std::filesystem::path state_dir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "maid";
    }
    return home_dir() / ".local" / "state" / "maid";
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
        // MAID's own locations first, so service files never hard-code them.
        if (name == "MAID_VENDOR") out.append((state_dir() / "vendor").string());
        else if (name == "MAID_STATE") out.append(state_dir().string());
        else if (name == "MAID_ROOT") out.append(root_dir().string());
        else if (name == "MAID_CONTEXT") {
            const char* c = std::getenv("MAID_CONTEXT");
            out.append(c && *c ? std::string(c) : "16384");
        } else if (name == "MAID_CONTEXT_2") {
            const char* c = std::getenv("MAID_CONTEXT_2");
            out.append(c && *c ? std::string(c) : "8192");
        } else if (name == "MAID_MODELS") {
            // The models directory from settings (main() exports it), else a default under the state directory.
            const char* m = std::getenv("MAID_MODELS_DIR");
            out.append(m && *m ? std::string(m) : (state_dir() / "models").string());
        }
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

void report_skipped(std::vector<std::string>* problems, std::string problem) {
    if (problems) {
        problems->push_back(std::move(problem));
        return;
    }
    static std::mutex mu;
    static std::set<std::string> told;
    std::lock_guard lock(mu);
    if (told.insert(problem).second) std::cerr << "maid: " << problem << "\n";
}

}  // namespace maid
