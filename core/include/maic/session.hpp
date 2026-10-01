#pragma once

#include "maic/llm.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace maic {

// Session transcripts live under <state_dir>/sessions in "homes": general/ (the default), projects/<encoded
// workspace path>/ (like Claude Code's layout), or any other named directory. One JSONL file per session.
std::filesystem::path sessions_dir();
std::filesystem::path sessions_home(const std::string& home);  // "general", "project:<workspace>", or a name

// Where unrecorded sessions go: $XDG_RUNTIME_DIR/maic/sessions (tmpfs, cleared at logout), else
// /tmp/maic-<uid>/sessions. They are never listed by `maic sessions`. (Windows: %TEMP%\maic\sessions.)
std::filesystem::path runtime_sessions_dir();
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

    std::filesystem::path path() const {
        std::lock_guard lock(mu_);
        return path_;
    }
    // Stamps `type` and, unless the record already carries one (a record copied from another session), `time`.
    void write(const std::string& type, nlohmann::json data);

    // Moves the open session into `dest_dir` (`:init` into the project home) and returns the new path. Records
    // written meanwhile go to <dest>/<id>.jsonl.pending and are appended once the file is in place. The `sub`
    // sessions it started move with it, and a `rehomed` record {from, to, reason} follows. On failure the session
    // stays where it was with every record, and this throws.
    std::filesystem::path relocate(const std::filesystem::path& dest_dir, const std::string& reason);
    // Tests: take the copy path as if the homes were on different filesystems, and run a hook once writes are
    // diverted, before the file moves.
    bool relocate_by_copy = false;
    std::function<void()> while_relocating;
    // What reopening repaired from a move a crash interrupted (recover_relocations), for the caller to show.
    const std::vector<std::string>& recovered() const { return recovered_; }

    // Tag constructors behind reopen() and fork(), public so a SessionLog can be made with make_unique.
    struct Reopen {};
    struct Fork {};
    SessionLog(Reopen, const std::filesystem::path& path);
    SessionLog(Fork, const std::filesystem::path& parent, size_t records, const std::string& kind, const std::filesystem::path& home);

private:
    void create(const std::string& kind, const std::filesystem::path& home);
    std::filesystem::path path_;
    mutable std::mutex mu_;
    std::ofstream out_;
    int pending_fd_ = -1;  // while relocating: where write() puts records
    std::vector<std::string> recovered_;
};

// Finishes a move a crash interrupted (SessionLog::relocate): records left in <id>.jsonl.pending are appended to the
// session file wherever it ended up, a copy that never got renamed into place (<id>.jsonl.moving, or a complete copy
// beside a source that was not yet removed) is discarded. `id` limits it to one session; "" looks at every home.
// Returns one notice per repair.
std::vector<std::string> recover_relocations(const std::string& id = "");

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
    std::string title;  // from a `title` record (:rename or an auto-title); "" when none
    size_t turns = 0;
    std::string model;   // from the start record
    long pid = 0;        // the process that last opened it
    std::string parent;  // id of the session this one was resumed from, if any
    size_t parent_records = 0;
    std::string delegated_from;  // kind sub: id of the session whose task call started this one
    std::string agent;           // kind sub: the agent it ran as (`agent`, or `profile` in older records)
};

// Newest first, across every home. With `workspace`, only sessions started or last opened in that directory.
std::vector<SessionInfo> list_sessions(const std::optional<std::filesystem::path>& workspace = std::nullopt);

// Moves a session file to another home. Forks keep working: they find their parent by id.
std::filesystem::path rehome_session(const SessionInfo& session, const std::string& home);

// The `sub` sessions `path` started (they live in its home and name it as their parent).
std::vector<std::filesystem::path> sub_sessions_of(const std::filesystem::path& path);

// Whether `:init` moves a session into the project home of `workspace`, the current one: Stay when it is not
// recorded or already there; Ask when it worked outside `workspace` (any write, or more than
// `outside_reads_allowed` files read), judged over the whole session, so work done before a `:cd` or under another
// workspace on an earlier open counts unless its paths fall inside this one; Move otherwise. `reason` says why.
struct InitMove {
    enum Verdict { Move, Ask, Stay } verdict = Stay;
    size_t outside_reads = 0;   // distinct files read outside the workspace
    size_t outside_writes = 0;  // written, edited, moved, copied to, deleted or made outside it, or shell workdirs there
    std::string reason;
};
// `records`: the session's records in order (walk_records), then those of its `sub` sessions.
InitMove init_move_check(const std::filesystem::path& path, const std::vector<nlohmann::json>& records, const std::filesystem::path& workspace,
                         bool recorded, size_t outside_reads_allowed);
// The same, reading the records of `path` and its `sub` sessions.
InitMove init_move_check(const std::filesystem::path& path, const std::filesystem::path& workspace, bool recorded, size_t outside_reads_allowed);

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

// Every record of a session in order: the parent a `resumed_from` pointer names first (its first `records`
// lines, recursively), then the file's own lines up to `limit`. Returns how many lines of the file itself were
// read. load_session and the summaries below are built on it.
size_t walk_records(const std::filesystem::path& path, size_t limit, const std::function<void(const nlohmann::json&)>& fn);

// Follows `resumed_from` pointers, so a forked session loads its parent's history first. `records` stops
// after that many lines of the file itself (what `--fork-at N` forks from).
LoadedSession load_session(const std::filesystem::path& path, size_t records = ~size_t(0));

// What `maic sessions state` shows, over the whole history (parents included).
struct SessionStats {
    size_t records = 0;  // lines in this file
    size_t turns = 0;    // user records
    size_t replies = 0;  // assistant records
    size_t tool_calls = 0;
    size_t tool_errors = 0;
    std::map<std::string, size_t> tools;  // calls per tool
    std::vector<std::string> files;       // paths written, edited, moved, copied, deleted or restored, first seen first
    long input_tokens = 0;                // usage totals
    long output_tokens = 0;
    int context = 0;  // the last window the provider reported
    std::map<std::string, size_t> compactions;  // by stage
    size_t clears = 0;
    size_t undos = 0;
    std::string first_time;  // of the first and the last record
    std::string last_time;
};
SessionStats session_stats(const std::filesystem::path& path);

// What `maic sessions time` shows: each turn from its prompt to the last record before the next prompt, and each
// tool call from the record before it to its result, in whole seconds (records are stamped to the second).
struct TurnTiming {
    std::string prompt;  // a preview
    long seconds = 0;
    size_t tool_calls = 0;
};
struct ToolTiming {
    std::string summary;
    long seconds = 0;
    bool ok = true;
};
struct SessionTiming {
    std::vector<TurnTiming> turns;
    std::vector<ToolTiming> tools;  // in order; sort by seconds for the slowest
};
SessionTiming session_timing(const std::filesystem::path& path);

// New sessions built from old ones. No source file is modified; each result is an ordinary session in `home`.
//
// inject: continues `parent`'s first `records` (a fork pointer) and adds one note to the conversation: a message
// of `role` (user or system), marked by an `inject` record and shown as a notice, never as a typed turn.
std::filesystem::path inject_note(const std::filesystem::path& parent, size_t records, const std::string& role, const std::string& text,
                                  const std::filesystem::path& home);
// graft: continues `onto`'s first `records` and then holds `graft`'s whole conversation, copied (its system prompt,
// start and title records left out), after a `graft` record and a note saying where the messages came from.
std::filesystem::path graft_session(const std::filesystem::path& onto, size_t records, const std::filesystem::path& graft,
                                    const std::filesystem::path& home);
// compose: `source`'s records from line `first` on, copied, with the system prompt from before the cut kept, after
// a `compose` record, the optional `root` text (a user message shown as a notice) and a note that the earlier part
// of the conversation is not present.
std::filesystem::path compose_session(const std::filesystem::path& source, size_t first, const std::string& root,
                                      const std::filesystem::path& home);

// True when the session's last process is still alive on this host (a maic process with that pid).
bool session_running(const SessionInfo& info);
// The session's own lock file (tripwire = "session"), if set: <transcript>.tripped
std::optional<std::string> session_lock_reason(const SessionInfo& info);

// Number of records (lines) in a session file.
size_t count_records(const std::filesystem::path& path);

// A copy of a session file taken before it is rewritten in place, named as cai's trans-fairy-write names its own
// (its safety_backup): <sessions>/.backups/<id>/<UTC %Y%m%dT%H%M%SZ>.jsonl, a -N suffix for a second copy in the
// same second, 0600 in 0700 directories, so `cai trans-fairy-write list-backups` and `restore` find it.
std::filesystem::path backup_session(const std::filesystem::path& path);
// Is this file a MAIC session, by content, as cai's grammar decides it: a MAIC record (a start with its workspace,
// a msg with its role, ...) and none of Claude Code's keys outside cai's own record types.
bool is_maic_session(const std::filesystem::path& path);

// The transcript as markdown: a title line, the session id, ## User / ## Assistant sections, tool calls and
// results in fenced blocks when `tool_details`.
std::string export_markdown(const SessionInfo& info, const LoadedSession& session, bool tool_details = true);

// The transcript as plain text for reading or piping: `[user]` and `[assistant]` blocks for user turns `from` to
// `to` (1-based, inclusive; 0 means no bound), notices, and tool calls and results as `[tool]` / `[result]` when `tools`.
std::string render_text(const LoadedSession& session, size_t from = 0, size_t to = 0, bool tools = false);

}  // namespace maic
