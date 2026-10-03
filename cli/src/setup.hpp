#pragma once

namespace maid {

// `maid setup`: a guided first run on a terminal. Each step is a yes/no question and nothing runs without a
// yes; off a terminal it prints the plan and returns 2.
int run_setup();

}  // namespace maid
