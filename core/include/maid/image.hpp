#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace maid {

// An image attached to a user turn: the bytes as base64 with their media type, and the file name for
// transcripts and notices. Loaded from a file by `load_image`; PNG, JPEG, WebP and GIF are recognised by
// their extension, anything else is refused.
struct ImageData {
    std::string mime;
    std::string base64;
    std::string name;
    std::string data_url() const { return "data:" + mime + ";base64," + base64; }
};

bool is_image_path(const std::filesystem::path& p);  // by extension
ImageData load_image(const std::filesystem::path& p);  // throws when missing, not an image, or over 20 MB
std::string base64_encode(const std::string& bytes);

}  // namespace maid
