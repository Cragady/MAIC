#pragma once

#include "maic/settings.hpp"

#include <string>
#include <vector>

namespace maic {

// maic audit-trail init | status [--json] | purge | offsite DEST [--older-than 90d] | schedule install|remove
// (docs/audit-trail.md). Handed off before maic's own options are read, so --json stays its own.
int cmd_audit_trail(const std::vector<std::string>& args);

// The start-up check at every entry point (the TUI, maic -p, maic status): nothing unless the trail is on and an
// audit is due with no scheduler, overdue past grace, or the live trail is past live_mb; then `enforce` decides
// (judge-and-hold, scan-and-continue or notify). Writes to stderr only.
void audit_gate(const Settings& settings);

}  // namespace maic
