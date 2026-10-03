#pragma once

#include <string>
#include <vector>

namespace maid {

// The environment a fixed helper program starts with (nvidia-smi, sha256sum, docker, the vendor builds, a clipboard
// tool, sudo for the tripwire, a browser opener): MAID's own without any model key (is_key_env), as claude-cli,
// services, nvim and git get it. Commands the user runs themselves (`!cmd`, $EDITOR, api_key_command) keep theirs.
std::vector<std::string> keyless_environ();

// `sh -c command` in keyless_environ(), where system() or popen() ran one before: `input` (when given) written to its
// stdin, else the stdin it inherits; its stdout into `out` when given, else inherited; stderr inherited. The exit
// code, 127 when it could not start, 128 + the signal when one ended it.
int run_helper(const std::string& command, std::string* out = nullptr, const std::string* input = nullptr);

}  // namespace maid
