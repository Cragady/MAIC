#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace maic {

// Contents of the root-owned lock file if the tripwire is tripped, otherwise nullopt.
// Where a trip from this process lands. "machine": the root-owned lock every MAIC process on the machine
// respects (the default; unlocking asks for sudo). "session": a user-owned file beside this session's
// transcript, respected by this session only (`:unlock` removes it, no sudo). The machine lock is honoured
// in both scopes. From `tripwire` in settings, so a project's .maic/settings.lua can pick per project.
// "isolated": like "session", and the machine lock is ignored; the harness confines such a session (no reads
// outside its directory, no remote requests). Allowed only when settings say allow_isolated = true.
void set_tripwire_scope(const std::string& scope, const std::filesystem::path& session_lock);
bool session_tripped();      // the session lock file exists
bool unlock_session();       // removes the session lock; true when nothing is tripped afterwards

std::optional<std::string> tripwire_state();

// Throws if tripped. Call before anything that acts on the system; stopping things stays allowed.
void require_armed(const std::string& action);

// Trips the lock through the passwordless `sudo -n maic-lock trip` rule.
void trip_tripwire(const std::string& reason);

}  // namespace maic
