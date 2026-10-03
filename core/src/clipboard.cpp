#include "maid/clipboard.hpp"

#include "maid/helper.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace maid {

namespace {

std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += tbl[v >> 18], out += tbl[(v >> 12) & 63], out += tbl[(v >> 6) & 63], out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[v >> 18], out += tbl[(v >> 12) & 63], out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[v >> 18], out += tbl[(v >> 12) & 63], out += tbl[(v >> 6) & 63], out += '=';
    }
    return out;
}

bool has_program(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::error_code ec;
        if (end > start && std::filesystem::exists(std::filesystem::path(p.substr(start, end - start)) / name, ec)) return true;
        start = end + 1;
    }
    return false;
}

bool pipe_to(const char* command, const std::string& text) {
    return run_helper(command, nullptr, &text) == 0;
}

}  // namespace

std::string copy_to_clipboard(const std::string& text) {
    if (std::getenv("MAID_NO_CLIPBOARD")) return "clipboard disabled";  // tests, and anyone who wants it off
    std::string used;
    if (std::getenv("WAYLAND_DISPLAY") && has_program("wl-copy") && pipe_to("wl-copy 2>/dev/null", text)) {
        used = "wl-copy";
    } else if (std::getenv("DISPLAY") && has_program("xclip") && pipe_to("xclip -selection clipboard 2>/dev/null", text)) {
        used = "xclip";
    }
    // OSC 52 goes straight to the terminal, even when this session is over ssh.
    if (text.size() < 100000 && isatty(STDOUT_FILENO)) {
        std::string seq = "\x1b]52;c;" + base64(text) + "\x07";
        if (write(STDOUT_FILENO, seq.data(), seq.size()) > 0) used += used.empty() ? "terminal (OSC 52)" : " + terminal";
    }
    return used.empty() ? "internal register only" : used;
}

std::string paste_from_clipboard() {
    if (std::getenv("MAID_NO_CLIPBOARD")) return "";
    const char* commands[] = {"wl-paste -n 2>/dev/null", "xclip -selection clipboard -o 2>/dev/null", "xsel -ob 2>/dev/null"};
    const char* needs[] = {"wl-paste", "xclip", "xsel"};
    for (int i = 0; i < 3; ++i) {
        if (!has_program(needs[i])) continue;
        std::string out;
        if (run_helper(commands[i], &out) == 0 && !out.empty()) return out;
    }
    return "";
}

}  // namespace maid
