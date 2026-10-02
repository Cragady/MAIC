#pragma once

// The audit trail (docs/audit-trail.md): one entry per tool call of every session, recorded or not, for
// maic-leak-audit. Off by default; every parameter is in ~/.config/maic/audit.lua, the user's own file, which no
// project can set or override.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

namespace maic {

struct AuditSettings {
    bool enabled = false;
    std::string every = "1d";        // how often an audit runs (the systemd timer, the due check)
    long every_seconds = 86400;
    int grace = 3;                   // with a schedule installed, enforcement waits until grace x every overdue
    std::string live_window = "1d";  // entries younger than this are live
    long live_window_seconds = 86400;
    int stale_days = 14;             // days after an entry's audit result last changed before it is archival
    std::string order = "stale-first";       // stale-first, live-first, oldest-first, newest-first
    std::string enforce = "judge-and-hold";  // judge-and-hold, scan-and-continue, notify
    bool start_services = true;      // judge-and-hold starts the judge's local service when it is down
    std::string judge = "qwen-9b";   // the preset maic-leak-audit judges with
    bool judge_thinking = true;      // the judge thinks before its verdict (enable_thinking on llama-server)
    int judge_max_tokens = 2048;     // the judge's reply budget per candidate, thinking included
    int file_mb = 16;                // a container past this continues in a numbered part
    int live_mb = 256;               // the live trail past this triggers enforcement
    int chunk_mb = 256;              // archive chunks are split at this
    std::string archive = "off";     // "off": retired entries are purged; else the directory chunks go to
};

// <config>/maic/audit.lua, beside settings.lua.
std::filesystem::path audit_settings_path();
// audit.lua over the defaults, evaluated at the user's Lua level (lua_data_limits, set by load_settings); the
// defaults when it does not exist. Throws naming the file on a bad value or an unknown key.
AuditSettings load_audit_settings();
// "90", "30m", "12h", "1d", "2w" in seconds; throws on anything else.
long parse_duration(const std::string& text);
// Writes audit.lua with every key at its default and a comment on each. Never overwrites; returns false when the
// file already exists.
bool write_default_audit_settings();

// <state_dir>/audit-trail: containers <YYYYMMDD>.jsonl (then <YYYYMMDD>.2.jsonl, ... past file_mb), index.json
// (maic-leak-audit's), `seq` (the last id given, and the lock every writer and container rewrite takes) and
// `.lock` (one audit at a time). 0600 files in a 0700 directory.
std::filesystem::path audit_trail_dir();
// Appends one entry under the seq lock: stamps `id` (one more than the last) and `time` (UTC). Throws when the
// line cannot be written.
void append_audit_trail(nlohmann::json entry, int file_mb);

// What the start-up check found (cheap: index.json, the containers' sizes and the timer unit's existence).
struct AuditDue {
    bool scheduled = false;  // the systemd timer unit is installed
    bool due = false;        // next_audit_due has passed
    bool overdue = false;    // grace x every past the last audit
    bool over_size = false;  // the live trail is past live_mb
    uintmax_t live_bytes = 0;
    // Enforce: due with no scheduler, overdue, or over the size cap.
    bool enforce() const { return (due && !scheduled) || overdue || over_size; }
    std::string why;  // one sentence for the hold line
};
AuditDue audit_due(const AuditSettings& settings, std::time_t now);

// The systemd user unit directory ($XDG_CONFIG_HOME/systemd/user) and the units `maic audit-trail schedule
// install` writes from contrib/systemd/: @EXEC@, @MAIC@ and @EVERY@ filled in.
std::filesystem::path systemd_user_dir();
std::string render_unit(const std::string& template_text, const std::string& exec, const std::string& maic, const std::string& every);

}  // namespace maic
