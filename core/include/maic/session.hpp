#pragma once

#include "maic/llm.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace maic {

// Where session transcripts go: <state_dir>/sessions, one JSONL file per session.
std::filesystem::path sessions_dir();

// Append-only transcript of one agent session: what was said, every tool call, and the harness's decision on it.
// Files are created 0600 in a 0700 directory, since transcripts can hold anything the agent read.
class SessionLog {
public:
    explicit SessionLog(const std::string& kind);  // kind: "tui", "headless"

    // Continues an earlier session in place: new records append to its file.
    static SessionLog reopen(const std::filesystem::path& path) { return SessionLog(Reopen{}, path); }

    // A new session whose history starts as the first `records` lines of `parent`. The new file holds only a
    // pointer to the parent (path and record count), not a copy; load_session follows it.
    static SessionLog fork(const std::filesystem::path& parent, size_t records, const std::string& kind) {
        return SessionLog(Fork{}, parent, records, kind);
    }

    const std::filesystem::path& path() const { return path_; }
    void write(const std::string& type, nlohmann::json data);

    // Tag constructors behind reopen() and fork(), public so a SessionLog can be made with make_unique.
    struct Reopen {};
    struct Fork {};
    SessionLog(Reopen, const std::filesystem::path& path);
    SessionLog(Fork, const std::filesystem::path& parent, size_t records, const std::string& kind);

private:
    void create(const std::string& kind);
    std::filesystem::path path_;
    std::mutex mu_;
    std::ofstream out_;
};

// What `maic sessions` and `--resume` show.
struct SessionInfo {
    std::filesystem::path path;
    std::string id;          // the file name without .jsonl
    std::string workspace;
    std::string started;     // from the file name
    std::string kind;
    std::string first_prompt;
    size_t turns = 0;
    std::string parent;  // id of the session this one was resumed from, if any
    size_t parent_records = 0;
};

// Newest first. With `workspace`, only sessions started in that directory.
std::vector<SessionInfo> list_sessions(const std::optional<std::filesystem::path>& workspace = std::nullopt);

// Finds a session by id, unique id prefix, or path.
std::optional<SessionInfo> find_session(const std::string& id_or_path);

// One displayable record of a past session, in order.
struct TranscriptEntry {
    std::string type;  // user, assistant, tool_call, tool_result, notice
    std::string text;
    bool ok = true;
};

struct LoadedSession {
    std::vector<Message> messages;  // the conversation to continue, system prompt included
    std::vector<TranscriptEntry> transcript;
    std::string model;
    std::string mode;
    size_t records = 0;  // lines in this file (what a fork of it would point at)
};

// Follows `resumed_from` pointers, so a forked session loads its parent's history first.
LoadedSession load_session(const std::filesystem::path& path);

// Number of records (lines) in a session file.
size_t count_records(const std::filesystem::path& path);

}  // namespace maic
