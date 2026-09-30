#pragma once

#include <optional>
#include <string>

namespace maic {

// Contents of the root-owned lock file if the tripwire is tripped, otherwise nullopt.
std::optional<std::string> tripwire_state();

// Throws if tripped. Call before anything that acts on the system; stopping things stays allowed.
void require_armed(const std::string& action);

// Trips the lock through the passwordless `sudo -n maic-lock trip` rule.
void trip_tripwire(const std::string& reason);

}  // namespace maic
