#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace maic {

// One headless nvim with the user's configuration (the theme import, the keymap check): stdin, stdout and stderr
// on /dev/null so nothing waits on a prompt, in its own process group so a plugin manager's children go with it,
// the whole group killed when it ends, at `timeout`, or when `cancel` turns true. The environment is this
// process's without the variables named in `drop` (a name, or a prefix ending in '_'), plus `add` (NAME=value).
struct NvimRun {
    int status = 0;  // as waitpid gives it; meaningless when timed_out or cancelled
    bool timed_out = false;
    bool cancelled = false;
};
NvimRun run_nvim_child(const std::vector<std::string>& args, const std::vector<std::string>& drop, const std::vector<std::string>& add,
                       std::chrono::seconds timeout, const std::atomic<bool>* cancel = nullptr);

}  // namespace maic
