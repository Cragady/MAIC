#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace maic {

// A conversation read from another tool's transcript, as the records a MAIC session file would hold.
struct ImportedSession {
    std::string format;     // "claude-ai" (a claude.ai export, JSON) or "claude-code" (a Claude Code transcript, JSONL)
    std::string workspace;  // from the source when it records one, else the current directory
    std::string title;      // "" when the source has none
    std::string model;      // the model named in the source, informational only
    std::vector<std::pair<std::string, nlohmann::json>> records;  // (type, data): msg, user, assistant, tool
    size_t messages = 0;    // msg records
    size_t skipped = 0;     // records and content blocks of kinds MAIC does not carry over (thinking, metadata, ...)
    size_t malformed = 0;   // lines that were not a JSON object (claude-code only)
};

// "claude-ai" or "claude-code" from the file's content.
std::string detect_import_format(const std::filesystem::path& source);

// format: "auto", "claude-ai" or "claude-code". A claude.ai export holding several conversations needs
// `conversation` (a uuid) to pick one; the error lists them otherwise.
ImportedSession read_import(const std::filesystem::path& source, const std::string& format = "auto", const std::string& conversation = "");

// Writes it as a new session file in `home` (a SessionLog, so 0600 and append-only): a start record, an
// imported_from record naming the source, the title, then the conversation. Returns the new file's path.
std::filesystem::path write_import(const ImportedSession& session, const std::filesystem::path& source, const std::filesystem::path& home);

}  // namespace maic
