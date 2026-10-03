#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace maid {

// Self-describing files (docs/design/engine-protocol.md, Event identity): a transcript or a protocol recording
// declares the shape of each kind of line it holds the first time it holds one, so a reader needs no source and no
// history to read it. A shape is a skeleton: the value with every name kept and every value emptied.

// The serialization every hash is taken over: JCS, RFC 8785 (keys sorted by UTF-16 code units, no whitespace,
// ECMAScript number form, minimal string escapes).
inline constexpr const char* kCanonical = "RFC 8785";
std::string canonical_json(const nlohmann::json& v);

// null stays null, false, 0 and "" stand for a boolean, a number and a string, an object keeps its names, and an
// array holds the distinct skeletons of its elements in canonical order.
nlohmann::json skeleton_of(const nlohmann::json& v);

// OCI's digest form, "sha256:<hex>", so a later algorithm is a new prefix rather than a new field.
std::string sha256_digest(const std::string& bytes);

inline std::string skeleton_hash(const nlohmann::json& skeleton) { return sha256_digest(canonical_json(skeleton)); }

}  // namespace maid
