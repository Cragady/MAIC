#pragma once

#include <string>
#include <vector>

namespace maid {

struct Gpu {
    std::string name;
    int vram_mb = 0;  // 0 when unknown
    std::string note;
};
Gpu detect_gpu();

// One line of the tools list: what, whether it is there, and what to do when it is not.
struct Check {
    std::string what;
    bool ok = false;
    std::string detail;
    bool optional = false;  // docker, nvidia-smi: MAID works without them
};
// The programs MAID and its services need: python3, git, uv, curl, bubblewrap, the tripwire; docker and
// nvidia-smi as optional. Shared by `maid doctor` and `maid setup`.
std::vector<Check> prerequisites();

// The quick and deep models the card (or the RAM, without one) can carry, with the reason.
struct Recommendation {
    std::string quick, deep, why;
};
Recommendation recommend(const Gpu& gpu, long ram_gb);

// Prints the machine, the tools MAID depends on, the models present, and a recommended local setup.
int run_doctor();

}  // namespace maid
