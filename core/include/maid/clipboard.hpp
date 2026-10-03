#pragma once

#include <string>

namespace maid {

// Copies text to the system clipboard: through the terminal (OSC 52, works over ssh) and, when present,
// wl-copy or xclip. Returns what was used, for the status line.
std::string copy_to_clipboard(const std::string& text);

// The system clipboard's text through wl-paste, xclip or xsel; "" when none is available.
std::string paste_from_clipboard();

}  // namespace maid
