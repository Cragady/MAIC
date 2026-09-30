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

    const std::filesystem::path& path() const { return path_; }
    void write(const std::string& type, nlohmann::json data);

private:
    struct Reopen {};
    SessionLog(Reopen, const std::filesystem::path& path);
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
};

LoadedSession load_session(const std::filesystem::path& path);

}  // namespace maic
