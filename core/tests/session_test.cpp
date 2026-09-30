// Import, redact and fork-at, against fixture files and a throwaway state directory. The real
// ~/.local/state/maic is never touched: XDG_STATE_HOME points under ~/.cache for the whole run.
#include "check.hpp"

#include "maic/import.hpp"
#include "maic/redact.hpp"
#include "maic/session.hpp"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

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

    fs::remove_all(ws);
    return finish();
}
