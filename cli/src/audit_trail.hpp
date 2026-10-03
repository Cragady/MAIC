#pragma once

#include "maid/settings.hpp"

#include <string>
#include <vector>

namespace maid {

// maid audit-trail init | status [--json] | purge | offsite DEST [--older-than 90d] | schedule install|remove
// (docs/audit-trail.md). Handed off before maid's own options are read, so --json stays its own.
int cmd_audit_trail(const std::vector<std::string>& args);

// The start-up check at every entry point (the TUI, maid -p, maid status): nothing unless the trail is on and an
// audit is due with no scheduler, overdue past grace, or the live trail is past live_mb; then `enforce` decides
// (judge-and-hold, scan-and-continue or notify). Writes to stderr only.
void audit_gate(const Settings& settings);

// An executable named `name` on PATH, or "".
std::string find_on_path(const std::string& name);
// This binary's path (/proc/self/exe), or "".
std::string self_exe();
// Runs argv with stdin on /dev/null and stderr inherited; stdout into `out` when given. The exit code, or -1 when
// it could not start.
int run(const std::vector<std::string>& argv, std::string* out);

}  // namespace maid
