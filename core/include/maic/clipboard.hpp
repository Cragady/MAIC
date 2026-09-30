#pragma once

#include <string>

namespace maic {

// Copies text to the system clipboard: through the terminal (OSC 52, works over ssh) and, when present,
// wl-copy or xclip. Returns what was used, for the status line.
std::string copy_to_clipboard(const std::string& text);

}  // namespace maic
