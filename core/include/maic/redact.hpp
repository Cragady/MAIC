#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace maic {

struct RedactReport {
    std::map<std::string, size_t> counts;  // kind -> replacements
    size_t records = 0;                    // lines written
    size_t malformed = 0;                  // lines that were not JSON: redacted as text and copied
    size_t total() const;
};

// Replaces credential material in `text` with [REDACTED:<kind>] and counts each kind: private key blocks,
// passwords in URLs and on command lines, KEY=value and "key": "value" pairs whose name suggests a secret,
// Bearer and Basic authorization, JWTs, API keys and tokens by their vendor prefix (sk-, ghp_, xox., AKIA,
// AIza, ...), and the opaque shapes cai's redact removes (uppercase hex, long numbers, long base64 blobs).
std::string redact_text(const std::string& text, std::map<std::string, size_t>& counts);

// Reads a session file and writes a copy with every string value redacted (record-threading fields such as
// tool call ids aside). `in` is never modified; `out` must not exist yet and is created 0600. The kept outputs its
// records name are copied redacted beside it (side_dir(out)), and the records point at the copies.
RedactReport redact_session(const std::filesystem::path& in, const std::filesystem::path& out);

// `maic sessions redact --in-place`: copies the file with backup_session first, then rewrites it through a
// temporary file and a rename; a MAIC session then gets a `rewritten` record naming the copy and `invocation`,
// as cai's trans-fairy-write appends one. Its kept outputs move beside the copy (side_dir of it) and redacted ones
// take their place. Returns the copy's path.
std::filesystem::path redact_session_in_place(const std::filesystem::path& path, const std::string& invocation, RedactReport& report);

}  // namespace maic
