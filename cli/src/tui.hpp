#pragma once

#include <string>

namespace maic {

// The interactive agent: full-screen, vim-style input and navigation.
int run_tui(const std::string& model);

}  // namespace maic
