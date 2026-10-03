#include "maid/image.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace maid {

namespace fs = std::filesystem;

namespace {
std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
}  // namespace

std::string base64_encode(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
        i += 3;
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool is_image_path(const fs::path& p) {
    std::string e = lower_ext(p);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".webp" || e == ".gif";
}

ImageData load_image(const fs::path& p) {
    if (!is_image_path(p)) throw std::runtime_error(p.string() + " is not an image (png, jpg, webp, gif)");
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) throw std::runtime_error("no such image: " + p.string());
    if (fs::file_size(p, ec) > 20u * 1024 * 1024) throw std::runtime_error(p.string() + " is over 20 MB");
    std::ifstream in(p, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string e = lower_ext(p);
    std::string mime = e == ".png" ? "image/png" : e == ".webp" ? "image/webp" : e == ".gif" ? "image/gif" : "image/jpeg";
    return {mime, base64_encode(bytes), p.filename().string()};
}

}  // namespace maid
