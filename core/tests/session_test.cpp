// Import, redact and fork-at, against fixture files and a throwaway state directory. The real
// ~/.local/state/maic is never touched: XDG_STATE_HOME points under ~/.cache for the whole run.
#include "check.hpp"

#include "maic/import.hpp"
#include "maic/redact.hpp"
#include "maic/session.hpp"

#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using namespace maic;
using nlohmann::json;

namespace {

std::string read_whole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Every line of a session file that is a JSON object, in order.
std::vector<json> records(const fs::path& p) {
    std::vector<json> out;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) {
        auto j = json::parse(line, nullptr, false);
        if (j.is_object()) out.push_back(j);
    }
    return out;
}

std::string last_line(const fs::path& p) {
    std::string last;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) last = line;
    return last;
}

std::vector<json> messages_of(const ImportedSession& s) {
    std::vector<json> out;
    for (const auto& [type, data] : s.records) {
        if (type == "msg") out.push_back(data);
    }
    return out;
}

std::vector<std::string> roles_of(const ImportedSession& s) {
    std::vector<std::string> out;
    for (const auto& m : messages_of(s)) out.push_back(m.value("role", ""));
    return out;
}

bool any_content_has(const ImportedSession& s, const std::string& needle) {
    for (const auto& m : messages_of(s)) {
        if (m.value("content", "").find(needle) != std::string::npos) return true;
    }
    return false;
}

bool mode_is_0600(const fs::path& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600;
}

}  // namespace

int main() {
    fs::path home = std::getenv("HOME");
    fs::path ws = home / ".cache" / "maic-session-test";
    fs::remove_all(ws);
    fs::create_directories(ws);
    setenv("XDG_STATE_HOME", (ws / "state").c_str(), 1);
    fs::path fixtures = MAIC_FIXTURES;
    fs::path ai_export = fixtures / "claude-ai-export.json";
    fs::path cc_transcript = fixtures / "claude-code.jsonl";

    section("import: a claude.ai export");
    {
        bool listed = false;
        try {
            read_import(ai_export);
        } catch (const std::exception& e) {
            listed = std::string(e.what()).find("22222222-aaaa-4bbb-8ccc-000000000002  A second conversation") != std::string::npos;
        }
        expect(listed, "an export with several conversations asks for --conversation and lists them");
        expect(detect_import_format(ai_export) == "claude-ai", "the format is detected from the content");
        ImportedSession s = read_import(ai_export, "auto", "11111111-aaaa-4bbb-8ccc-000000000001");
        expect(s.format == "claude-ai" && s.title == "Fennec ears for the mascot" && s.workspace == fs::current_path().string(),
               "format, the export's name as the title, the current directory as the workspace");
        expect(roles_of(s) == std::vector<std::string>{"user", "assistant", "tool", "assistant", "user", "assistant"} && s.messages == 6,
               "messages: user, assistant with a call, its result, assistant, user, assistant");
        auto msgs = messages_of(s);
        expect(msgs[1]["content"] == "Let me check the reference." && msgs[1]["tool_calls"][0]["name"] == "web_search" &&
                   msgs[1]["tool_calls"][0]["id"] == "toolu_ai_1" && msgs[1]["tool_calls"][0]["arguments"]["query"] == "fennec ear proportions",
               "a tool_use block becomes a tool call on the assistant message");
        expect(msgs[2]["tool_call_id"] == "toolu_ai_1" && msgs[2]["tool_name"] == "web_search" && msgs[2]["content"] == "Ears are about a third of body length.",
               "a tool_result block becomes a tool message answering that call");
        const json* shown = nullptr;
        for (const auto& [type, data] : s.records) {
            if (type == "tool") shown = &data;
        }
        expect(shown && (*shown)["tool"] == "web_search" && (*shown)["arguments"]["query"] == "fennec ear proportions" && (*shown)["ok"] == true,
               "the tool display record names the call and its arguments");
        expect(msgs[3]["content"] == "Massive: about a third of her height." && msgs[3]["role"] == "assistant", "text after the result is its own assistant turn");
        expect(!any_content_has(s, "never mind"), "an abandoned sibling branch is left out");
        expect(any_content_has(s, "[pasted file: sketch.txt]\n\ntail: big and fluffy") && any_content_has(s, "1 attached file(s) had no extractable text"),
               "attachments are inlined and files without text are noted");
        expect(s.skipped == 2, "the thinking block and the text-less file are counted as skipped: " + std::to_string(s.skipped));
        size_t displays = 0;
        for (const auto& [type, data] : s.records) displays += type == "user" || type == "assistant";
        expect(displays == 5, "user and assistant display records for every turn with text");

        fs::path path = write_import(s, ai_export, sessions_home("general"));
        expect(mode_is_0600(path) && path.parent_path() == sessions_home("general"), "the session file is a 0600 SessionLog in the chosen home");
        auto recs = records(path);
        expect(recs.size() >= 3 && recs[0]["type"] == "start" && recs[0]["workspace"] == s.workspace && recs[0]["mode"] == "manual",
               "the first record is a start record with the workspace");
        expect(recs[1]["type"] == "imported_from" && recs[1]["format"] == "claude-ai" && recs[1]["path"] == fs::weakly_canonical(ai_export).string() &&
                   recs[1]["messages"] == 6 && recs[1]["skipped"] == 2,
               "imported_from names the source, its format and the counts");
        expect(recs[2]["type"] == "title" && recs[2]["text"] == "Fennec ears for the mascot", "then the title");
        bool found = false;
        for (const auto& info : list_sessions()) {
            if (info.path != path) continue;
            found = true;
            expect(info.title == "Fennec ears for the mascot" && info.turns == 2 && info.kind == "import" && info.first_prompt == "How big should the ears be?",
                   "maic sessions lists it with the title, the turns and the first prompt");
        }
        expect(found, "the imported session is listed");
        // trans-fairy-write's copies (docs/cai.md) sit under sessions/.backups/<id>/ and are not sessions.
        fs::path copy = sessions_dir() / ".backups" / path.stem() / "20261001T000000Z.jsonl";
        fs::create_directories(copy.parent_path());
        fs::copy_file(path, copy);
        bool backup_listed = false;
        for (const auto& info : list_sessions()) backup_listed = backup_listed || info.path.string().find("/.backups/") != std::string::npos;
        expect(!backup_listed, "a backup under sessions/.backups is not listed as a session");
        fs::remove_all(sessions_dir() / ".backups");
        LoadedSession loaded = load_session(path);
        expect(loaded.messages.size() == 6 && loaded.messages[1].tool_calls.size() == 1 && loaded.messages[2].role == "tool" && loaded.transcript.size() == 7,
               "it loads back as a resumable conversation with a displayable transcript");
    }

    section("import: a Claude Code transcript");
    {
        expect(detect_import_format(cc_transcript) == "claude-code", "a typed record per line is a transcript");
        ImportedSession s = read_import(cc_transcript);
        expect(s.format == "claude-code" && s.workspace == "/home/mica/dev/fennec" && s.model == "claude-opus-5", "workspace and model come from the records");
        expect(s.title == "Notes line count", "custom-title beats ai-title beats summary");
        expect(s.malformed == 2, "a non-JSON line and a bare number are counted as malformed: " + std::to_string(s.malformed));
        expect(roles_of(s) == std::vector<std::string>{"user", "assistant", "tool", "assistant", "user", "assistant"} && s.messages == 6,
               "messages: user, assistant with a call, its result, assistant, user, assistant");
        auto msgs = messages_of(s);
        expect(msgs[1]["content"] == "I'll count them." && msgs[1]["tool_calls"][0]["id"] == "toolu_01" && msgs[1]["tool_calls"][0]["name"] == "Bash" &&
                   msgs[1]["tool_calls"][0]["arguments"]["command"] == "wc -l notes.md",
               "consecutive assistant records (thinking, text, tool_use) merge into one assistant message");
        expect(msgs[2]["tool_call_id"] == "toolu_01" && msgs[2]["tool_name"] == "Bash" && msgs[2]["content"] == "12 notes.md" && msgs[2].value("is_error", false) == false,
               "the tool_result user record becomes the tool message");
        expect(msgs[4]["content"] == "Thanks, that is all." && msgs[5]["content"] == "Any time.", "text blocks in a user record are the user's message");
        expect(!any_content_has(s, "<command-name>") && !any_content_has(s, "a subagent prompt"), "isMeta and sidechain records are left out");
        expect(s.skipped == 6, "thinking, meta, sidechain and metadata records are counted as skipped: " + std::to_string(s.skipped));

        fs::path path = write_import(s, cc_transcript, sessions_home("project:" + s.workspace));
        expect(path.parent_path() == sessions_dir() / "projects" / "-home-mica-dev-fennec", "--home project files it under the source's workspace");
        auto info = find_session(path.stem().string());
        expect(info && info->workspace == "/home/mica/dev/fennec" && info->home == "projects/-home-mica-dev-fennec" && info->title == "Notes line count",
               "it lists under that project with the source workspace");
        LoadedSession loaded = load_session(path);
        expect(loaded.messages.size() == 6 && loaded.messages[1].tool_calls[0].id == "toolu_01" && loaded.messages[2].tool_call_id == "toolu_01",
               "calls and results stay paired after the round trip");
        std::string md = export_markdown(*info, loaded);
        expect(md.find("## User\n\nCount the lines in notes.md") != std::string::npos && md.find("```\n12 notes.md\n```") != std::string::npos,
               "maic sessions export works on an imported session");
    }

    section("import: overrides and bad input");
    {
        bool threw = false;
        try {
            read_import(cc_transcript, "claude-ai");
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("not a claude.ai export") != std::string::npos;
        }
        expect(threw, "--as claude-ai on a transcript fails with a clear message");
        threw = false;
        try {
            read_import(ai_export, "claude-code", "11111111-aaaa-4bbb-8ccc-000000000001");
        } catch (const std::exception&) {
            threw = true;
        }
        ImportedSession forced = read_import(ai_export, "claude-code");
        expect(!threw && forced.messages == 0 && forced.malformed > 0, "--as claude-code on an export is not fatal: every line is malformed or skipped");
        threw = false;
        try {
            read_import(ai_export, "markdown");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "an unknown format is refused");
        threw = false;
        try {
            read_import(ws / "missing.json");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a missing file is an error");
    }

    section("redact: every kind");
    {
        std::map<std::string, size_t> counts;
        auto r = [&](const std::string& t) { return redact_text(t, counts); };
        struct Case {
            std::string in, out, what;
        };
        const Case cases[] = {
            {"export OPENAI_API_KEY=sk-proj-abcdefghijklmnopqrstuvwxyz0123", "export OPENAI_API_KEY=[REDACTED:env-secret]", "KEY=value by the name of the key"},
            {"DB_PASSWORD=\"hunter2hunter2\"", "DB_PASSWORD=\"[REDACTED:env-secret]\"", "quoted .env values keep their quotes"},
            {"the key sk-ant-api03-abcdefghijklmnopqrstuv1234 here", "the key [REDACTED:api-key] here", "sk- keys"},
            {"stripe sk_live_abcdefghijklmnopqrst", "stripe [REDACTED:api-key]", "Stripe live keys"},
            {"google AIzaSyA1234567890abcdefghijklmnopqrstuv", "google [REDACTED:api-key]", "Google API keys"},
            {"ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef0123", "[REDACTED:github-token]", "GitHub tokens"},
            {"xoxb-1234567890-abcdefghij", "[REDACTED:slack-token]", "Slack tokens"},
            {"aws AKIAIOSFODNN7EXAMPLE", "aws [REDACTED:aws-key]", "AWS access key ids"},
            {"Authorization: Bearer abcdef0123456789abcdef", "Authorization: Bearer [REDACTED:bearer]", "Bearer tokens"},
            {"jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0In0.SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c", "jwt [REDACTED:jwt]", "JWTs"},
            {"postgres://mica:hunter2@db.local/app", "postgres://mica:[REDACTED:url-password]@db.local/app", "passwords in URLs"},
            {"curl -u mica:hunter2 https://x", "curl -u mica:[REDACTED:basic-auth] https://x", "curl -u user:password"},
            {"Authorization: Basic bWljYTpodW50ZXIy", "Authorization: Basic [REDACTED:basic-auth]", "Basic authorization"},
            {"mysql --password=hunter2 db", "mysql --password=[REDACTED:cli-password] db", "--password on a command line"},
            {"{\"password\": \"hunter2\"}", "{\"password\": \"[REDACTED:json-secret]\"}", "\"password\": \"...\" in text"},
            {"salt 0123456789ABCDEF0123456789ABCDEF", "salt [REDACTED:hex-32]", "32 uppercase hex digits (cai's hex-32)"},
            {"id 12345678901234567890", "id [REDACTED:numeric-handle]", "long opaque numbers (cai's numeric-handle)"},
            {"blob QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5eg==", "blob [REDACTED:base64-blob]", "long base64 blobs (cai's base64-blob)"},
        };
        for (const auto& c : cases) {
            std::string got = r(c.in);
            expect(got == c.out, c.what + (got == c.out ? "" : ": got " + got));
        }
        std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\nQyNTUxOQAAACA=\n-----END OPENSSH PRIVATE KEY-----";
        expect(r("key:\n" + pem + "\nend") == "key:\n[REDACTED:private-key]\nend", "private key blocks, whole");
        expect(counts.size() == 15 && counts["api-key"] == 3 && counts["basic-auth"] == 2 && counts["env-secret"] == 2, "counts are kept per kind");
        for (const char* keep : {"git 3f2a9c1e5d7b8a0f4c6e2d1b9a8f7e6d5c4b3a21 sha", "monkey=bananas123", "sort_key=name", "a normal sentence with no secrets in it"}) {
            expect(r(keep) == keep, std::string("left alone: ") + keep);
        }
        std::string one = r("Authorization: Bearer sk-abcdefghijklmnopqrstuvwxyz");
        expect(one == "Authorization: Bearer [REDACTED:bearer]", "the first pattern to match wins; a marker is never redacted again: " + one);
        std::string two = r("A=sk-abcdefghijklmnopqrstuvwxyz B=sk-zyxwvutsrqponmlkjihgfedcba");
        expect(two == "A=[REDACTED:api-key] B=[REDACTED:api-key]", "several matches in one value: " + two);
        std::string nul = r(std::string("x\0sk-abcdefghijklmnopqrstuvwxyz", 31));
        expect(nul == std::string("x\0[REDACTED:api-key]", 20), "text after an embedded NUL is still scanned");
    }

    section("redact: a session file");
    {
        SessionLog log("session-test");
        log.write("start", {{"workspace", "/w"}, {"model", "m"}, {"mode", "manual"}});
        log.write("msg", message_to_json({"user", "here is my key sk-abcdefghijklmnopqrstuvwxyz"}));
        log.write("msg", message_to_json({"assistant", "", {{"call_1", "run_shell", {{"command", "export GITHUB_TOKEN=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef0123"}}}}}));
        log.write("msg", message_to_json({"tool", "postgres://u:pw12345@h/db", {}, "run_shell", "call_1"}));
        log.write("tool", {{"tool", "run_shell"}, {"arguments", {{"command", "x"}, {"env", {{"API_KEY", "does-not-look-like-one"}}}}}, {"result", "ok"}, {"ok", true}});
        Message thought{"assistant", "done"};
        thought.raw_kind = "anthropic";
        thought.raw = json::array({{{"type", "thinking"}, {"thinking", "t"}, {"signature", std::string(80, 'Q')}}});
        log.write("msg", message_to_json(thought));
        std::ofstream(log.path(), std::ios::app) << "not json but has ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef9999\n";
        std::string before = read_whole(log.path());

        fs::path out = ws / "copy.redacted.jsonl";
        RedactReport rep = redact_session(log.path(), out);
        expect(read_whole(log.path()) == before, "the original is untouched");
        expect(mode_is_0600(out), "the copy is created 0600");
        auto recs = records(out);
        expect(recs.size() == 6 && recs[1]["content"] == "here is my key [REDACTED:api-key]", "user text is redacted");
        expect(recs[2]["tool_calls"][0]["arguments"]["command"] == "export GITHUB_TOKEN=[REDACTED:env-secret]" && recs[2]["tool_calls"][0]["id"] == "call_1" &&
                   recs[2]["tool_calls"][0]["name"] == "run_shell",
               "tool arguments are redacted; call ids and names are kept");
        expect(recs[3]["content"] == "postgres://u:[REDACTED:url-password]@h/db" && recs[3]["tool_call_id"] == "call_1" && recs[3]["tool_name"] == "run_shell",
               "tool results are redacted; tool_call_id is kept");
        expect(recs[4]["arguments"]["env"]["API_KEY"] == "[REDACTED:secret-field]" && recs[4]["arguments"]["command"] == "x",
               "a field named like a secret is replaced whatever its value looks like");
        expect(recs[5]["raw"][0]["signature"] == std::string(80, 'Q') && recs[5]["content"] == "done", "provider signatures are structural and kept");
        expect(last_line(out) == "not json but has [REDACTED:github-token]" && rep.malformed == 1, "a malformed line is redacted as text and counted");
        expect(rep.records == 7 && rep.total() == 5 && rep.counts["api-key"] == 1 && rep.counts["env-secret"] == 1 && rep.counts["url-password"] == 1 &&
                   rep.counts["secret-field"] == 1 && rep.counts["github-token"] == 1,
               "the report counts records and every kind");
        std::string copy = read_whole(out);
        bool threw = false;
        try {
            redact_session(log.path(), out);
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("never written over") != std::string::npos;
        }
        expect(threw && read_whole(out) == copy, "an existing output file is never overwritten");
        threw = false;
        try {
            redact_session(log.path(), log.path());
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw && read_whole(log.path()) == before, "writing over the source is refused the same way");
    }

    section("redact --in-place: a copy first, then a rewritten record");
    {
        SessionLog log("session-test");
        log.write("start", {{"workspace", "/w"}, {"model", "m"}, {"mode", "manual"}});
        log.write("msg", message_to_json({"user", "here is my key sk-abcdefghijklmnopqrstuvwxyz"}));
        log.write("user", {{"text", "here is my key sk-abcdefghijklmnopqrstuvwxyz"}});
        std::string before = read_whole(log.path());
        RedactReport rep;
        fs::path backup = redact_session_in_place(log.path(), "maic sessions redact " + log.path().stem().string() + " --in-place", rep);
        fs::path dir = sessions_dir() / ".backups" / log.path().stem();
        std::string name = backup.filename().string();
        bool stamp = name.size() == 22 && name.substr(8, 1) == "T" && name.substr(15) == "Z.jsonl";
        for (size_t i : {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14}) stamp = stamp && std::isdigit(static_cast<unsigned char>(name[i]));
        expect(backup.parent_path() == dir && stamp, "the copy is <sessions>/.backups/<id>/<UTC stamp>.jsonl, as trans-fairy-write names its own: " + backup.string());
        struct stat d1{}, d2{};
        expect(read_whole(backup) == before && mode_is_0600(backup) && stat(dir.c_str(), &d1) == 0 && (d1.st_mode & 0777) == 0700 &&
                   stat(dir.parent_path().c_str(), &d2) == 0 && (d2.st_mode & 0777) == 0700,
               "it is the original byte for byte, 0600 inside 0700 directories");
        auto recs = records(log.path());
        expect(recs.size() == 4 && recs[1]["content"] == "here is my key [REDACTED:api-key]" && recs[2]["text"] == "here is my key [REDACTED:api-key]", "the session itself is redacted");
        expect(recs[3]["type"] == "rewritten" && recs[3]["backup"] == backup.string() && recs[3]["tool"] == "maic sessions redact" &&
                   recs[3]["invocation"] == "maic sessions redact " + log.path().stem().string() + " --in-place" && recs[3].contains("time"),
               "and ends with a rewritten record naming the copy and the command");
        fs::path again = backup_session(log.path());
        expect(again != backup && again.parent_path() == dir && fs::exists(backup), "a second copy never replaces the first (a -N suffix within the same second)");

        // A file that is not a MAIC session (Claude Code's shape) gets the copy but no record of MAIC's.
        fs::path cc = ws / "cc-transcript.jsonl";
        {
            std::ofstream f(cc);
            f << json{{"type", "user"}, {"uuid", "u1"}, {"parentUuid", nullptr}, {"sessionId", "s"}, {"message", {{"role", "user"}, {"content", "token sk-abcdefghijklmnopqrstuvwxyz"}}}}.dump() << "\n";
        }
        expect(is_maic_session(log.path()) && !is_maic_session(cc), "is_maic_session tells a MAIC session from a Claude Code transcript");
        std::string cc_before = read_whole(cc);
        fs::path cc_backup = redact_session_in_place(cc, "maic sessions redact " + cc.string() + " --in-place", rep);
        expect(read_whole(cc_backup) == cc_before && records(cc).size() == 1 && read_whole(cc).find("[REDACTED:api-key]") != std::string::npos &&
                   cc_backup.parent_path() == sessions_dir() / ".backups" / "cc-transcript",
               "a Claude Code transcript gets the copy and the redaction, and no rewritten record");
    }

    section("fork-at");
    {
        SessionLog parent("session-test");
        parent.write("start", {{"workspace", "/w"}, {"model", "m"}, {"mode", "manual"}});
        parent.write("msg", message_to_json({"system", "sys"}));
        parent.write("msg", message_to_json({"user", "one"}));
        parent.write("user", {{"text", "one"}});
        parent.write("msg", message_to_json({"assistant", "first answer"}));
        parent.write("assistant", {{"text", "first answer"}});
        parent.write("msg", message_to_json({"user", "two"}));
        parent.write("user", {{"text", "two"}});
        parent.write("msg", message_to_json({"assistant", "second answer"}));
        parent.write("assistant", {{"text", "second answer"}});
        std::string before = read_whole(parent.path());
        expect(count_records(parent.path()) == 10, "ten records to fork from");
        LoadedSession cut = load_session(parent.path(), 6);
        expect(cut.messages.size() == 3 && cut.messages.back().content == "first answer" && cut.transcript.size() == 2 && cut.records == 6,
               "load_session with a record count stops there");

        SessionLog child = SessionLog::fork(parent.path(), 6, "session-test");
        child.write("msg", message_to_json({"user", "a different two"}));
        child.write("user", {{"text", "a different two"}});
        auto recs = records(child.path());
        expect(recs[0]["type"] == "resumed_from" && recs[0]["records"] == 6 && recs[0]["id"] == parent.path().stem().string(),
               "the fork's first record points at the parent and the record count");
        LoadedSession forked = load_session(child.path());
        bool past_the_cut = false;
        for (const auto& m : forked.messages) past_the_cut = past_the_cut || m.content == "two" || m.content == "second answer";
        expect(forked.messages.size() == 4 && forked.messages[2].content == "first answer" && forked.messages[3].content == "a different two" && !past_the_cut,
               "the fork loads the parent's first six records and then its own, nothing past the cut");
        expect(forked.transcript.size() == 3 && forked.transcript[2].text == "a different two", "the transcript is cut the same way");
        expect(read_whole(parent.path()) == before && load_session(parent.path()).messages.size() == 5, "the parent is byte for byte unchanged");
        auto info = find_session(child.path().stem().string());
        expect(info && info->parent == parent.path().stem().string() && info->parent_records == 6 && info->workspace == "/w",
               "maic sessions shows the fork point and inherits the parent's workspace");
    }

    // Two sessions of two turns each; a's records: 0 start, 1 system, 2-3 "a one", 4-5 "a first", 6-7 "a two", 8-9 "a second",
    // 10 title, 11 a usage record with its own time.
    auto two_turns = [](SessionLog& log, const std::string& ws_name, const std::string& tag) {
        log.write("start", {{"workspace", ws_name}, {"model", "m"}, {"mode", "manual"}});
        log.write("msg", message_to_json({"system", "sys " + tag}));
        for (const char* turn : {"one", "two"}) {
            log.write("msg", message_to_json({"user", tag + " " + turn}));
            log.write("user", {{"text", tag + " " + turn}});
            std::string reply = tag + (std::string(turn) == "one" ? " first" : " second");
            log.write("msg", message_to_json({"assistant", reply}));
            log.write("assistant", {{"text", reply}});
        }
        log.write("title", {{"text", tag}});
    };

    section("compose");
    {
        SessionLog a("session-test");
        two_turns(a, "/w", "a");
        a.write("usage", {{"input", 1}, {"output", 2}, {"context", 10}, {"time", "2020-01-01T00:00:00+0000"}});
        std::string a_before = read_whole(a.path());
        std::string a_id = a.path().stem().string();

        fs::path c = compose_session(a.path(), 6, "the root text", sessions_home("general"));
        auto recs = records(c);
        expect(recs[0]["type"] == "start" && recs[0]["workspace"] == "/w" && recs[0]["model"] == "m" && recs[0]["mode"] == "manual",
               "a composed session starts with the source's workspace, model and mode");
        expect(recs[1]["type"] == "compose" && recs[1]["id"] == a_id && recs[1]["from"] == 6 && recs[1]["records"] == 5,
               "the compose record names the source, the cut and how many records were copied");
        expect(recs[2]["type"] == "msg" && recs[2]["role"] == "system" && recs[2]["content"] == "sys a", "the system prompt from before the cut is kept");
        expect(recs[3]["type"] == "msg" && recs[3]["role"] == "user" && recs[3]["content"] == "the root text" && recs[4]["type"] == "context" &&
                   recs[4]["text"].get<std::string>().find("root (composed, not typed)") == 0,
               "the root is a user message shown as a notice");
        expect(recs[5]["type"] == "msg" && recs[5]["role"] == "system" && recs[5]["content"].get<std::string>().find("from its record 6") != std::string::npos &&
                   recs[6]["type"] == "context",
               "then a note that the earlier part is missing");
        expect(recs.size() == 12 && recs.back()["type"] == "usage" && recs.back()["time"] == "2020-01-01T00:00:00+0000",
               "copied records keep their own time; the title is not copied");
        LoadedSession lc = load_session(c);
        bool before_cut = false;
        for (const auto& m : lc.messages) before_cut = before_cut || m.content == "a one" || m.content == "a first";
        expect(lc.messages.size() == 5 && lc.messages[0].role == "system" && lc.messages.back().content == "a second" && !before_cut,
               "it loads as system prompt, root, note, then the suffix only");
        expect(lc.transcript.size() == 4 && lc.transcript[0].type == "notice" && lc.transcript[2].type == "user" && lc.transcript[2].text == "a two",
               "the transcript shows the two notices and the suffix");
        auto info = find_session(c.stem().string());
        expect(info && info->workspace == "/w" && info->turns == 1 && info->kind == "compose" && info->title.empty(), "it lists as a one-turn session in the workspace");
        bool threw = false;
        try {
            compose_session(a.path(), 50, "", sessions_home("general"));
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("nothing after 50") != std::string::npos;
        }
        expect(threw, "a cut past the end is refused");
        expect(read_whole(a.path()) == a_before, "the source is untouched");
    }

    section("graft");
    {
        SessionLog a("session-test");
        two_turns(a, "/w", "a");
        SessionLog b("session-test");
        two_turns(b, "/v", "b");
        std::string a_before = read_whole(a.path()), b_before = read_whole(b.path());
        std::string a_id = a.path().stem().string(), b_id = b.path().stem().string();

        fs::path g = graft_session(b.path(), 6, a.path(), sessions_home("general"));
        auto recs = records(g);
        expect(recs[0]["type"] == "resumed_from" && recs[0]["id"] == b_id && recs[0]["records"] == 6, "a graft is a fork of the target at the cut");
        expect(recs[1]["type"] == "graft" && recs[1]["id"] == a_id && recs[1]["messages"] == 4, "the graft record names the grafted session and its message count");
        expect(recs[2]["type"] == "msg" && recs[2]["role"] == "system" && recs[2]["content"].get<std::string>().find("4 messages were grafted from session " + a_id) == 9 &&
                   recs[3]["type"] == "context",
               "a note says where the messages came from");
        LoadedSession lg = load_session(g);
        std::vector<std::string> contents;
        for (const auto& m : lg.messages) contents.push_back(m.content);
        expect(contents.size() == 8 && contents[0] == "sys b" && contents[2] == "b first" && contents[3].find("grafted") != std::string::npos && contents[4] == "a one" &&
                   contents[7] == "a second",
               "it loads as the target's first six records, the note, then the grafted conversation without its system prompt");
        expect(lg.transcript.size() == 7 && lg.transcript[2].type == "notice" && lg.transcript[3].text == "a one", "the transcript is cut and joined the same way");
        auto info = find_session(g.stem().string());
        expect(info && info->parent == b_id && info->parent_records == 6 && info->workspace == "/v" && info->title.empty(), "it lists as a fork of the target");

        SessionLog f = SessionLog::fork(a.path(), 6, "session-test");
        f.write("msg", message_to_json({"user", "f three"}));
        f.write("user", {{"text", "f three"}});
        fs::path g2 = graft_session(b.path(), 10, f.path(), sessions_home("general"));
        LoadedSession lg2 = load_session(g2);
        contents.clear();
        for (const auto& m : lg2.messages) contents.push_back(m.content);
        expect(contents.size() == 9 && contents[4] == "b second" && contents[6] == "a one" && contents[7] == "a first" && contents[8] == "f three",
               "grafting a fork copies the conversation it holds: its parent's first records (system prompt aside), then its own");
        expect(records(g2).size() == 10 && records(g2)[4]["content"] == "a one", "the pointer itself is not copied");
        auto info2 = find_session(g2.stem().string());
        expect(info2 && info2->parent == b_id && info2->parent_records == 10, "the listing shows the target as the parent");
        SessionLog empty("session-test");
        empty.write("start", {{"workspace", "/w"}, {"model", "m"}, {"mode", "manual"}});
        bool threw = false;
        try {
            graft_session(b.path(), 10, empty.path(), sessions_home("general"));
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("no conversation to graft") != std::string::npos;
        }
        expect(threw, "a session with nothing to graft is refused");
        expect(read_whole(a.path()) == a_before && read_whole(b.path()) == b_before, "neither source is modified");
    }

    section("inject");
    {
        SessionLog a("session-test");
        two_turns(a, "/w", "a");
        std::string a_before = read_whole(a.path());
        fs::path i = inject_note(a.path(), 6, "system", "Remember: ears a third of her height.", sessions_home("general"));
        auto recs = records(i);
        expect(recs.size() == 4 && recs[0]["type"] == "resumed_from" && recs[0]["records"] == 6 && recs[1]["type"] == "inject" && recs[1]["role"] == "system",
               "an injection is a fork at the cut with an inject record");
        expect(recs[2]["type"] == "msg" && recs[2]["role"] == "system" && recs[2]["content"] == "Remember: ears a third of her height." && recs[3]["type"] == "context" &&
                   recs[3]["text"].get<std::string>().find("injected system note") == 0,
               "the note is a system message shown as a notice, not a typed turn");
        LoadedSession li = load_session(i);
        expect(li.messages.size() == 4 && li.messages[2].content == "a first" && li.messages[3].role == "system" && li.transcript.size() == 3 && li.transcript[2].type == "notice",
               "it loads after the parent's first six records");
        auto info = find_session(i.stem().string());
        expect(info && info->turns == 0 && info->parent_records == 6 && info->workspace == "/w", "the listing counts no turn for the note");
        fs::path u = inject_note(a.path(), 10, "user", "and a user note", sessions_home("general"));
        expect(load_session(u).messages.back().role == "user" && records(u)[1]["role"] == "user", "a user-role note");
        bool threw = false;
        try {
            inject_note(a.path(), 6, "assistant", "I said this", sessions_home("general"));
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "an assistant turn cannot be injected");
        expect(read_whole(a.path()) == a_before, "the parent is untouched");
    }

    section("state, time and read");
    {
        SessionLog s("session-test");
        auto at = [](const char* t) { return std::string("2026-10-01T10:") + t + "+0000"; };
        auto w = [&](const char* type, json data, const char* t) {
            data["time"] = at(t);
            s.write(type, data);
        };
        w("start", {{"workspace", "/w"}, {"model", "m"}, {"mode", "edit"}}, "00:00");
        w("msg", message_to_json({"system", "sys"}), "00:00");
        w("msg", message_to_json({"user", "fix the ears"}), "00:01");
        w("user", {{"text", "fix the ears"}}, "00:01");
        w("msg", message_to_json({"assistant", "", {{"c1", "read_file", {{"path", "ears.md"}}}}}), "00:05");
        w("msg", message_to_json({"tool", "text", {}, "read_file", "c1"}), "00:07");
        w("tool", {{"tool", "read_file"}, {"arguments", {{"path", "ears.md"}}}, {"result", "text"}, {"ok", true}}, "00:07");
        w("msg", message_to_json({"assistant", "", {{"c2", "edit_file", {{"path", "ears.md"}}}}}), "00:10");
        w("msg", message_to_json({"tool", "edited", {}, "edit_file", "c2"}), "00:40");
        w("tool", {{"tool", "edit_file"}, {"arguments", {{"path", "ears.md"}}}, {"result", "edited"}, {"ok", true}}, "00:40");
        w("msg", message_to_json({"assistant", "done"}), "00:45");
        w("assistant", {{"text", "done"}}, "00:45");
        w("usage", {{"input", 100}, {"output", 20}, {"context", 8192}}, "00:45");
        w("msg", message_to_json({"user", "and the tail"}), "02:00");
        w("user", {{"text", "and the tail"}}, "02:00");
        w("msg", message_to_json({"assistant", "", {{"c3", "run_shell", {{"command", "make"}}}}}), "02:03");
        w("msg", message_to_json({"tool", "boom", {}, "run_shell", "c3", true}), "02:13");
        w("tool", {{"tool", "run_shell"}, {"arguments", {{"command", "make"}}}, {"result", "boom"}, {"ok", false}}, "02:13");
        w("compact", {{"stage", "prune"}, {"bytes_before", 10}, {"bytes_after", 5}}, "02:13");
        w("msg", message_to_json({"assistant", "fixed"}), "02:20");
        w("assistant", {{"text", "fixed"}}, "02:20");
        w("usage", {{"input", 150}, {"output", 30}, {"context", 8192}}, "02:20");
        w("undo", {{"path", "/w/tail.md"}, {"summary", "restored"}}, "02:30");

        SessionStats st = session_stats(s.path());
        expect(st.records == 23 && st.turns == 2 && st.replies == 2 && st.tool_calls == 3 && st.tool_errors == 1, "records, turns, replies, tool calls and failures");
        expect(st.tools == std::map<std::string, size_t>{{"read_file", 1}, {"edit_file", 1}, {"run_shell", 1}}, "calls per tool");
        expect(st.files == std::vector<std::string>{"ears.md", "/w/tail.md"}, "files touched: written and restored paths, reads left out");
        expect(st.input_tokens == 250 && st.output_tokens == 50 && st.context == 8192, "usage totals and the last window");
        expect(st.compactions == std::map<std::string, size_t>{{"prune", 1}} && st.undos == 1 && st.clears == 0, "compactions by stage, undos");
        expect(st.first_time == at("00:00") && st.last_time == at("02:30"), "first and last record times");
        SessionLog child = SessionLog::fork(s.path(), 13, "session-test");
        SessionStats cs = session_stats(child.path());
        expect(cs.turns == 1 && cs.tool_calls == 2 && cs.input_tokens == 100 && cs.records == 1, "a fork's state covers the parent's first records and counts only its own lines");

        SessionTiming t = session_timing(s.path());
        expect(t.turns.size() == 2 && t.turns[0].seconds == 44 && t.turns[0].tool_calls == 2 && t.turns[0].prompt == "fix the ears",
               "a turn lasts from its prompt to the last record before the next one");
        expect(t.turns[1].seconds == 30 && t.turns[1].tool_calls == 1, "the last turn ends at its last record");
        expect(t.tools.size() == 3 && t.tools[0].seconds == 2 && t.tools[1].seconds == 30 && t.tools[2].seconds == 10 && !t.tools[2].ok,
               "a tool call lasts from the record before it to its result; its own tool message does not count");
        expect(t.tools[1].summary == "edit_file ears.md" && t.tools[2].summary == "$ make", "summaries as the transcript shows them");
        SessionLog z("session-test");
        z.write("user", {{"text", "zoned"}, {"time", "2026-10-01T12:00:00+0200"}});
        z.write("assistant", {{"text", "ok"}, {"time", "2026-10-01T10:00:30+0000"}});
        expect(session_timing(z.path()).turns[0].seconds == 30, "offsets are honoured when records come from different zones");

        LoadedSession ls = load_session(s.path());
        std::string text = render_text(ls, 2, 2, true);
        expect(text.find("[user]\nand the tail") == 0 && text.find("[tool] $ make") != std::string::npos && text.find("[result, error]\nboom") != std::string::npos &&
                   text.find("fix the ears") == std::string::npos,
               "render_text picks a range of turns and shows tool traffic when asked");
        std::string plain = render_text(ls);
        expect(plain.find("[tool]") == std::string::npos && plain.find("[notice] compacted (prune)") != std::string::npos && plain.find("[assistant]\ndone") != std::string::npos,
               "without tools: turns and notices only");
    }

    section(":init moving a session: who is eligible");
    {
        fs::path proj = ws / "proj", elsewhere = ws / "elsewhere";
        fs::create_directories(proj);
        fs::create_directories(elsewhere);
        fs::path in_general = sessions_home("general") / "20261001-120000-tui-1.jsonl";
        auto start = [](const fs::path& w) { return json{{"type", "start"}, {"workspace", w.string()}}; };
        auto tool = [](const std::string& name, json args, bool ok = true) { return json{{"type", "tool"}, {"tool", name}, {"arguments", args}, {"ok", ok}}; };
        auto read_of = [&](const std::string& p) { return tool("read_file", {{"path", p}}); };
        std::vector<json> clean = {start(proj), read_of("README.md"), tool("write_file", {{"path", "src/a.cpp"}, {"content", "x"}}), tool("run_shell", {{"command", "make"}})};

        InitMove m = init_move_check(in_general, clean, proj, true, 3);
        expect(m.verdict == InitMove::Move && m.outside_reads == 0 && m.outside_writes == 0 && m.reason == "it worked here throughout",
               "work inside the workspace only: it moves");
        m = init_move_check(in_general, clean, proj, false, 3);
        expect(m.verdict == InitMove::Stay && m.reason.find("not recorded") != std::string::npos, "an unrecorded session stays, and says why");
        m = init_move_check(sessions_home("project:" + proj.string()) / "x.jsonl", clean, proj, true, 3);
        expect(m.verdict == InitMove::Stay && m.reason.find("already in projects/") != std::string::npos, "a session already in the project home stays");

        std::vector<json> wrote = clean;
        wrote.push_back(tool("edit_file", {{"path", (elsewhere / "b.txt").string()}, {"old_string", "a"}, {"new_string", "b"}}));
        m = init_move_check(in_general, wrote, proj, true, 3);
        expect(m.verdict == InitMove::Ask && m.outside_writes == 1 && m.reason == "it read 0 and wrote 1 files outside the project", "one write outside: it asks");
        std::vector<json> denied = clean;
        denied.push_back(tool("delete_file", {{"path", (elsewhere / "b.txt").string()}}, false));
        expect(init_move_check(in_general, denied, proj, true, 3).verdict == InitMove::Move, "a write that was refused or failed is not work done");
        std::vector<json> moved = clean;
        moved.push_back(tool("move_file", {{"from", "src/a.cpp"}, {"to", (elsewhere / "a.cpp").string()}}));
        m = init_move_check(in_general, moved, proj, true, 3);
        expect(m.verdict == InitMove::Ask && m.outside_writes == 1, "a move out of the workspace counts its outside end");
        std::vector<json> shelled = clean;
        shelled.push_back(tool("run_shell", {{"command", "make"}, {"workdir", elsewhere.string()}}));
        expect(init_move_check(in_general, shelled, proj, true, 3).outside_writes == 1, "a command run in a directory outside counts as a write there");
        std::vector<json> lua = clean;
        lua.push_back({{"type", "tool"}, {"tool", "my_lua"}, {"arguments", json::object()}, {"ok", true},
                       {"actions", {{{"action", "write_file " + (elsewhere / "c.txt").string()}, {"decision", "allow"}},
                                    {{"action", "read_file notes.md"}, {"decision", "allow"}}}}});
        m = init_move_check(in_general, lua, proj, true, 3);
        expect(m.outside_writes == 1 && m.outside_reads == 0, "a Lua tool's actions are judged one by one");

        std::vector<json> reads = clean;
        for (const char* f : {"1", "2", "3"}) reads.push_back(read_of((elsewhere / f).string()));
        reads.push_back(read_of((elsewhere / "1").string()));
        m = init_move_check(in_general, reads, proj, true, 3);
        expect(m.verdict == InitMove::Move && m.outside_reads == 3, "three files read outside (one of them twice) is under the threshold");
        reads.push_back(tool("list_dir", {{"path", (elsewhere / "4").string()}}));
        m = init_move_check(in_general, reads, proj, true, 3);
        expect(m.verdict == InitMove::Ask && m.outside_reads == 4 && m.outside_writes == 0, "a fourth is over it: it asks");
        expect(init_move_check(in_general, reads, proj, true, 10).verdict == InitMove::Move, "the threshold is the setting's");

        // :cd: judged against the current workspace, whatever was in effect when the work was done.
        json cd = {{"type", "workspace"}, {"from", elsewhere.string()}, {"to", proj.string()}};
        m = init_move_check(in_general, {start(elsewhere), cd, read_of("README.md")}, proj, true, 3);
        expect(m.verdict == InitMove::Move && m.outside_reads == 0, "started elsewhere, :cd here before any work: it moves");
        m = init_move_check(in_general, {start(elsewhere), tool("write_file", {{"path", "notes.md"}, {"content", "x"}}), read_of("a"), read_of("b"), cd, read_of("README.md")},
                            proj, true, 3);
        expect(m.verdict == InitMove::Ask && m.outside_writes == 1 && m.outside_reads == 2, "work in A, :cd to B: the work in A counts as outside, with the counts");
        m = init_move_check(in_general, {start(proj), start(elsewhere), read_of("x"), start(proj)}, proj, true, 3);
        expect(m.verdict == InitMove::Move && m.outside_reads == 1, "a resume elsewhere is judged the same way: its relative paths resolve there");
        m = init_move_check(in_general, {start(ws), read_of("proj/README.md"), json{{"type", "workspace"}, {"to", proj.string()}}}, proj, true, 3);
        expect(m.verdict == InitMove::Move && m.outside_reads == 0, "work before a :cd still counts as inside when its paths fall inside this workspace");
    }

    section(":init moving a session: the move");
    {
        fs::path proj = ws / "proj2";
        fs::create_directories(proj);
        fs::path dest = sessions_home("project:" + proj.string());
        auto seqs = [](const fs::path& p) {
            std::vector<int> out;
            for (const auto& j : records(p)) {
                if (j.value("type", "") == "n") out.push_back(j.value("seq", -1));
            }
            return out;
        };
        auto in_order = [](const std::vector<int>& v, int n) {
            if (static_cast<int>(v.size()) != n) return false;
            for (int i = 0; i < n; ++i) {
                if (v[static_cast<size_t>(i)] != i) return false;
            }
            return true;
        };

        // The copy path, with a writer appending all the while: records written during the move go to the pending
        // file and come back in order, once each.
        auto log = std::make_unique<SessionLog>("copied");
        fs::path from = log->path();
        log->write("start", {{"workspace", proj.string()}});
        constexpr int total = 400;
        std::atomic<int> written{0};
        std::thread writer([&] {
            for (int i = 0; i < total; ++i) {
                log->write("n", {{"seq", i}});
                written = i + 1;
                if (i % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        while (written < 50) std::this_thread::yield();
        bool pending_used = false;
        fs::path pending = dest / (from.filename().string() + ".pending");
        log->relocate_by_copy = true;
        log->while_relocating = [&] {
            int at = written;
            while (written < std::min(total, at + 30)) std::this_thread::yield();
            pending_used = fs::file_size(pending) > 0;
        };
        fs::path to = log->relocate(dest, "init");
        writer.join();
        log->write("after", {});
        expect(pending_used, "records written while it moved went to <id>.jsonl.pending");
        expect(to == dest / from.filename() && log->path() == to && fs::exists(to), "the session is in the project home, and the log writes there");
        expect(!fs::exists(from) && !fs::exists(pending) && !fs::exists(to.string() + ".moving"), "the source, the pending file and the copy's temporary name are gone");
        expect(in_order(seqs(to), total), "every record is in the moved file exactly once, in order");
        json rehomed;
        for (const auto& j : records(to)) {
            if (j.value("type", "") == "rehomed") rehomed = j;
        }
        expect(rehomed.value("from", "") == from.string() && rehomed.value("to", "") == to.string() && rehomed.value("reason", "") == "init",
               "a rehomed record names where it came from, where it went and why");
        expect(records(to).back().value("type", "") == "after" && mode_is_0600(to), "regular writes continue on the new path; the copy is 0600");
        bool listed = false;
        for (const auto& info : list_sessions()) {
            if (info.id == from.stem().string()) listed = info.home == "projects/" + project_home_name(proj) && info.path == to;
        }
        expect(listed, "maic sessions lists it in its new home");
        log.reset();

        // The rename path: same filesystem, the same inode, nothing to buffer for long.
        SessionLog r("renamed");
        r.write("start", {{"workspace", proj.string()}});
        r.write("n", {{"seq", 0}});
        struct stat before{}, after{};
        stat(r.path().c_str(), &before);
        fs::path rfrom = r.path(), rto = r.relocate(dest, "init");
        r.write("n", {{"seq", 1}});
        stat(rto.c_str(), &after);
        expect(before.st_ino == after.st_ino && !fs::exists(rfrom) && in_order(seqs(rto), 2), "on one filesystem it is a rename: the same file, no copy");
        bool refused = false;
        try {
            r.relocate(dest, "init");
        } catch (const std::exception& e) {
            refused = std::string(e.what()).find("already exists") != std::string::npos;
        }
        expect(refused && r.path() == rto && fs::exists(rto), "a move onto an existing file is refused and the session stays put");

        // Subagent sessions it started move with it; another session's do not.
        SessionLog parent("parent");
        parent.write("start", {{"workspace", proj.string()}});
        SessionLog child("sub", parent.path().parent_path()), stranger("sub", parent.path().parent_path());
        child.write("start", {{"workspace", proj.string()}, {"parent", parent.path().stem().string()}, {"agent", "explore"}});
        stranger.write("start", {{"workspace", proj.string()}, {"parent", "someone-else"}, {"agent", "explore"}});
        expect(sub_sessions_of(parent.path()) == std::vector<fs::path>{child.path()}, "sub_sessions_of finds the subagent sessions it started");
        fs::path cfrom = child.path(), sfrom = stranger.path();
        parent.relocate(dest, "init");
        expect(fs::exists(dest / cfrom.filename()) && !fs::exists(cfrom) && fs::exists(sfrom), "its subagents move along; another session's stay");

        // A fork in another home keeps loading its parent after the parent moved (by id).
        SessionLog base("base");
        base.write("start", {{"workspace", proj.string()}});
        base.write("msg", message_to_json({"user", "the fennec's ears"}));
        base.write("msg", message_to_json({"assistant", "a third of her height"}));
        SessionLog fork = SessionLog::fork(base.path(), 3, "tui", sessions_home("elsewhere"));
        fork.write("msg", message_to_json({"user", "and the tail?"}));
        base.relocate(dest, "init");
        LoadedSession ls = load_session(fork.path());
        expect(ls.messages.size() == 3 && ls.messages[0].content == "the fennec's ears", "a fork in another home still loads its moved parent");
    }

    section(":init moving a session: recovery after a crash");
    {
        fs::path proj = ws / "proj3";
        fs::create_directories(proj);
        fs::path dest = sessions_home("project:" + proj.string());
        fs::create_directories(dest);
        auto line = [](int seq) { return json{{"type", "n"}, {"seq", seq}, {"time", "2026-10-01T12:00:00+0000"}}.dump() + "\n"; };
        auto write_text = [](const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; };

        // Moved, but the pending records were never appended.
        fs::path target = dest / "20261001-120001-tui-2.jsonl";
        write_text(target, line(0) + line(1));
        write_text(target.string() + ".pending", line(2) + line(3));
        SessionLog a = SessionLog::reopen(target);
        expect(a.recovered().size() == 1 && a.recovered()[0].find("2 records") != std::string::npos, "reopening finds the leftover pending file and says so");
        expect(read_whole(target) == line(0) + line(1) + line(2) + line(3) && !fs::exists(target.string() + ".pending"), "its records are appended, the pending file removed");

        // Appended already, but the pending file was never removed: nothing is doubled.
        fs::path twice = dest / "20261001-120002-tui-3.jsonl";
        write_text(twice, line(0) + line(1) + line(2));
        write_text(twice.string() + ".pending", line(1) + line(2));
        SessionLog::reopen(twice);
        expect(read_whole(twice) == line(0) + line(1) + line(2) && !fs::exists(twice.string() + ".pending"), "records the file already ends with are not added twice");

        // The move never finished: the source is still in general/, the pending records go there.
        fs::path source = sessions_home("general") / "20261001-120003-tui-4.jsonl";
        write_text(source, line(0));
        fs::path moved_name = dest / source.filename();
        write_text(moved_name.string() + ".pending", line(1));
        write_text(moved_name.string() + ".moving", line(0).substr(0, 7));
        SessionLog b = SessionLog::reopen(source);
        expect(read_whole(source) == line(0) + line(1) && !fs::exists(moved_name.string() + ".pending"), "an unfinished move: the records go back to the source");
        expect(!fs::exists(moved_name.string() + ".moving") && !fs::exists(moved_name), "a leftover .moving beside an intact source is discarded");
        bool said = false;
        for (const auto& n : b.recovered()) said = said || n.find("discarded an unfinished copy") != std::string::npos;
        expect(said, "with a notice");

        // Copied and renamed into place, but the source was not yet removed: the copy goes, the source keeps the records.
        fs::path both = sessions_home("general") / "20261001-120004-tui-5.jsonl";
        write_text(both, line(0));
        write_text(dest / both.filename(), line(0));
        write_text((dest / both.filename()).string() + ".pending", line(1));
        std::vector<std::string> notices = recover_relocations();
        expect(!notices.empty() && read_whole(both) == line(0) + line(1) && !fs::exists(dest / both.filename()), "a complete copy beside its source is dropped, the source finished");
        expect(recover_relocations().empty(), "nothing is left to recover");
    }

    fs::remove_all(ws);
    return finish();
}
