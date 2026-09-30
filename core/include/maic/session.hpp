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

// Session transcripts live under <state_dir>/sessions in "homes": general/ (the default), projects/<encoded
// workspace path>/ (like Claude Code's layout), or any other named directory. One JSONL file per session.
std::filesystem::path sessions_dir();
std::filesystem::path sessions_home(const std::string& home);  // "general", "project:<workspace>", or a name
std::string project_home_name(const std::filesystem::path& workspace);  // "/home/x/dev/app" -> "-home-x-dev-app"

// Append-only transcript of one agent session: what was said, every tool call, and the harness's decision on it.
// Files are created 0600 in a 0700 directory, since transcripts can hold anything the agent read.
class SessionLog {
public:
    // kind: "tui", "headless". home: a directory under sessions_dir() (see sessions_home()).
    explicit SessionLog(const std::string& kind, const std::filesystem::path& home = sessions_home("general"));

    // Continues an earlier session in place: new records append to its file.
    static SessionLog reopen(const std::filesystem::path& path) { return SessionLog(Reopen{}, path); }

    // A new session whose history starts as the first `records` lines of `parent`. The new file holds only a
    // pointer to the parent (path and record count), not a copy; load_session follows it.
    static SessionLog fork(const std::filesystem::path& parent, size_t records, const std::string& kind,
                           const std::filesystem::path& home = sessions_home("general")) {
        return SessionLog(Fork{}, parent, records, kind, home);
    }

    const std::filesystem::path& path() const { return path_; }
    void write(const std::string& type, nlohmann::json data);

    // Tag constructors behind reopen() and fork(), public so a SessionLog can be made with make_unique.
    struct Reopen {};
    struct Fork {};
    SessionLog(Reopen, const std::filesystem::path& path);
    SessionLog(Fork, const std::filesystem::path& parent, size_t records, const std::string& kind, const std::filesystem::path& home);

private:
    void create(const std::string& kind, const std::filesystem::path& home);
    std::filesystem::path path_;
    std::mutex mu_;
    std::ofstream out_;
};

// What `maic sessions` and `--resume` show.
struct SessionInfo {
    std::filesystem::path path;
    std::string id;          // the file name without .jsonl
    std::string home;        // directory under sessions_dir(): "general", "projects/-home-...", ...
    std::string workspace;   // where it was started
    std::string opened_in;   // where it was last opened (the same, unless it was resumed elsewhere)
    std::string host;        // machine of the last open
    size_t opens = 0;        // how many times it was opened (start + every resume)
    std::string started;     // from the file name
    std::string kind;
    std::string first_prompt;
    size_t turns = 0;
    std::string parent;  // id of the session this one was resumed from, if any
    size_t parent_records = 0;
};

// Newest first, across every home. With `workspace`, only sessions started or last opened in that directory.
std::vector<SessionInfo> list_sessions(const std::optional<std::filesystem::path>& workspace = std::nullopt);

// Moves a session file to another home. Forks keep working: they find their parent by id.
std::filesystem::path rehome_session(const SessionInfo& session, const std::string& home);

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
