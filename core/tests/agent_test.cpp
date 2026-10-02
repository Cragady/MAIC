#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
// The agent loop against a fake OpenAI-compatible server: mid-turn messages, deliver-now, cancellation, resume.
#include "check.hpp"
#include "fake_server.hpp"

#include "maic/agent.hpp"
#include "maic/audit_trail.hpp"
#include "maic/jsonschema.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"
#include "maic/paths.hpp"
#include "maic/session.hpp"

#include "maic/http.hpp"

#include "fake_claude.hpp"
#include "fake_deepseek.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

using namespace maic;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Recorder : AgentEvents {
    std::string text;
    std::vector<std::string> notices;
    void on_text(std::string_view d, bool) override { text += d; }
    std::vector<std::string> calls;
    void on_tool_call(const std::string& s) override { calls.push_back(s); }
    std::vector<std::string> results;
    void on_tool_result(const std::string& t, bool) override { results.push_back(t); }
    void on_notice(const std::string& t) override { notices.push_back(t); }
    std::vector<ApprovalRequest> asked;
    ApprovalAnswer reply{Approval::No, ""};
    ApprovalAnswer ask(const ApprovalRequest& r) override {
        asked.push_back(r);
        return reply;
    }
    std::string answer;  // what question() returns
    std::vector<std::pair<std::string, std::vector<std::string>>> questions;
    std::string question(const std::string& text, const std::vector<std::string>& options) override {
        questions.push_back({text, options});
        return answer;
    }
    std::vector<std::vector<TodoItem>> todos;
    void on_todo(const std::vector<TodoItem>& items) override { todos.push_back(items); }
    struct Output {
        std::string call;
        OutputStream stream;
        std::string chunk;
        size_t offset;
        size_t results_before;  // tool results already in when it came
    };
    std::vector<Output> outputs;
    void on_tool_output(const std::string& call, OutputStream stream, std::string_view chunk, size_t offset) override {
        outputs.push_back({call, stream, std::string(chunk), offset, results.size()});
    }
    std::vector<fs::path> kept;
    void on_tool_full_output(const fs::path& file) override { kept.push_back(file); }
    std::string streamed(OutputStream s) const {
        std::string all;
        for (const auto& o : outputs) {
            if (o.stream == s) all += o.chunk;
        }
        return all;
    }
};

std::error_code& ec_ignore() {
    static std::error_code ec;
    return ec;
}

bool has_notice(const Recorder& r, const std::string& what) {
    for (const auto& n : r.notices) {
        if (n.find(what) != std::string::npos) return true;
    }
    return false;
}

bool has_call(const Recorder& r, const std::string& what) {
    for (const auto& c : r.calls) {
        if (c.find(what) != std::string::npos) return true;
    }
    return false;
}

bool has_result(const Recorder& r, const std::string& what) {
    for (const auto& t : r.results) {
        if (t.find(what) != std::string::npos) return true;
    }
    return false;
}

// A request from a subagent: its system prompt says so.
bool from_child(const json& body) {
    return body["messages"][0]["role"] == "system" && body["messages"][0]["content"].get<std::string>().find("# You are a subagent") != std::string::npos;
}

bool offers_tool(const json& body, const std::string& name) {
    if (!body.contains("tools")) return false;
    for (const auto& t : body["tools"]) {
        if (t["function"]["name"] == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    setenv("MAIC_TRIPWIRE_FILE", ("/tmp/maic-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);  // never the machine's lock
    // Everything this run touches is its own: the workspace and the sessions under it carry the pid, so two
    // runs at once (another worktree's ctest) never see each other's files, and ~/.local/state/maic stays as it is.
    fs::path ws = fs::temp_directory_path() / ("maic-agent-test-" + std::to_string(getpid()));
    fs::remove_all(ws);
    fs::create_directories(ws);
    setenv("XDG_STATE_HOME", (ws / "state").c_str(), 1);
    std::atomic<bool> no_cancel{false};

    section("plain turn");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        agent.submit("hello", Origin::Local, r, no_cancel);
        expect(r.text == "echo: hello", "streams the reply");
        expect(fake.requests.size() == 1 && fake.requests[0]["messages"][0]["role"] == "system" &&
               fake.requests[0]["messages"][0]["content"].get<std::string>().find("inside MAIC") != std::string::npos,
               "the model gets the MAIC briefing as the system prompt");
        std::string sys = fake.requests[0]["messages"][0]["content"];
        expect(sys.find("You are not the user") != std::string::npos, "the briefing separates the agent from the user");
    }

    section("mode change is appended, not rewritten");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        agent.submit("one", Origin::Local, r, no_cancel);
        agent.mode = Mode::Auto;
        agent.submit("two", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests[1]["messages"];
        expect(msgs[0] == fake.requests[0]["messages"][0], "the first system prompt is unchanged");
        bool appended = false;
        for (const auto& m : msgs) {
            if (m["role"] == "system" && m["content"].get<std::string>().find("mode is now auto") != std::string::npos) appended = true;
        }
        expect(appended, "the mode change arrives as a later system message");
    }

    section("mid-turn messages");
    {
        FakeServer fake;
        fake.hold_left = 1;  // the first reply stays open until the agent hangs up on it
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        std::thread poster([&] {
            fake.wait_streaming(1);
            agent.post_message("interjection");
            agent.deliver_now();
        });
        agent.submit("a long first message", Origin::Local, r, no_cancel);
        poster.join();
        expect(fake.requests.size() == 2, "deliver_now aborts the call in progress and re-asks (2 requests)");
        bool included = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            if (m["role"] == "user" && m["content"] == "interjection") included = true;
        }
        expect(included, "the re-ask includes the queued message");
        bool partial_kept = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            if (m["role"] == "assistant") partial_kept = true;
        }
        expect(!partial_kept, "the aborted partial reply is not kept in the history");
        expect(has_notice(r, "delivering"), "the user is told the message was delivered");
        expect(r.text.find("echo: interjection") != std::string::npos, "the final reply answers the newest message");
    }
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        agent.post_message("queued before the turn");
        agent.submit("first", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests[0]["messages"];
        expect(msgs[msgs.size() - 1]["content"] == "queued before the turn" && msgs[msgs.size() - 2]["content"] == "first",
               "a message queued without deliver_now goes after the turn's own message, in one request");
        expect(agent.queued() == 0 && agent.take_queued().empty(), "the mailbox is drained");
    }

    section("compaction");
    {
        FakeServer fake;
        fs::path path;
        std::string big(3000, 'x');
        {
            SessionLog log("agent-test");
            path = log.path();
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.set_log(&log);
            // A history with seven tool rounds, built by hand: system, then user/assistant(call)/tool triples.
            std::vector<Message> hist = {{"system", "sys"}};
            for (int i = 1; i <= 7; ++i) {
                hist.push_back({"user", "turn " + std::to_string(i)});
                hist.push_back({"assistant", "", {{"c" + std::to_string(i), "read_file", {{"path", "f" + std::to_string(i)}}}}});
                hist.push_back({"tool", "result " + std::to_string(i) + "\n" + big, {}, "read_file", "c" + std::to_string(i)});
                hist.push_back({"assistant", "reply " + std::to_string(i)});
            }
            agent.restore(hist);
            size_t before = agent.messages().size();
            std::string r = agent.compact(Agent::Compaction::Prune, no_cancel);
            expect(r.find("pruned 3 old tool results") == 0, "prune stubs all but the last 4 tool results: " + r);
            int stubs = 0, intact = 0, dialog = 0;
            for (const auto& m : agent.messages()) {
                if (m.role == "tool") (m.content.rfind("[pruned", 0) == 0 ? stubs : intact)++;
                if (m.role == "user" && m.content.rfind("turn ", 0) == 0) ++dialog;
            }
            expect(stubs == 3 && intact == 4 && dialog == 7 && agent.messages().size() == before, "dialog and message count are untouched; only old results are stubbed");
            expect(agent.messages()[3].content.find("bytes of read_file") != std::string::npos && agent.messages()[3].content.find("2 lines") != std::string::npos,
                   "a stub says what it replaced: " + agent.messages()[3].content);
            expect(agent.compact(Agent::Compaction::Prune, no_cancel) == "nothing to prune", "a second prune has nothing to do");

            r = agent.compact(Agent::Compaction::Head, no_cancel);
            expect(r.find("summarised the oldest") == 0, "head summarises the oldest turns: " + r);
            const auto& m = agent.messages();
            expect(m[0].role == "system" && m[1].role == "user" && m[1].content.find("[Handover note") == 0 && m[1].content.find("echo: The conversation") != std::string::npos,
                   "the summary (from the model, no tools) becomes one user message after the system prompt");
            int remaining = 0;
            for (const auto& x : m) remaining += x.role == "user" && x.content.rfind("turn ", 0) == 0;
            std::string last_reply;
            for (const auto& x : m) if (x.role == "assistant" && !x.content.empty()) last_reply = x.content;
            expect(remaining == 4 && last_reply == "reply 7", "the newest half of the turns stays verbatim");
            expect(fake.requests.back()["tools"].is_null() || fake.requests.back()["tools"].empty(), "the summariser gets no tools");
            r = agent.compact(Agent::Compaction::All, no_cancel);
            remaining = 0;
            for (const auto& x : agent.messages()) remaining += x.role == "user" && x.content.rfind("turn ", 0) == 0;
            expect(remaining == 0 && agent.messages().size() == 2, "all collapses everything to system + note");
        }
        LoadedSession loaded = load_session(path);
        expect(loaded.messages.size() == 2 && loaded.messages[1].content.find("[Handover note") == 0, "resume after compaction loads the compacted history");
        expect(std::count_if(loaded.transcript.begin(), loaded.transcript.end(), [](const TranscriptEntry& t) { return t.type == "notice" && t.text.find("compacted") == 0; }) == 3,
               "the transcript records each compaction");

        // Automatic: a call that reports a nearly full context triggers compaction before the next call.
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.compaction.at = 0.5;
        std::vector<Message> hist = {{"system", "sys"}};
        for (int i = 1; i <= 6; ++i) {
            hist.push_back({"user", "t" + std::to_string(i)});
            hist.push_back({"assistant", "", {{"k" + std::to_string(i), "read_file", {{"path", "f"}}}}});
            hist.push_back({"tool", big, {}, "read_file", "k" + std::to_string(i)});
            hist.push_back({"assistant", "r" + std::to_string(i)});
        }
        agent.restore(hist);
        Recorder r1;
        agent.submit("go", Origin::Local, r1, no_cancel);  // fake reports no usage: nothing happens
        expect(!has_notice(r1, "compacting"), "no usage report, no auto-compaction");
        fake.usage_input = 12000;  // 12000 of 16384 = 73% > 50%
        Recorder r2;
        agent.submit("again", Origin::Local, r2, no_cancel);
        Recorder r3;
        agent.submit("and again", Origin::Local, r3, no_cancel);
        expect(has_notice(r3, "compacting") && has_notice(r3, "pruned"), "a full context compacts before the next call, pruning first");
    }

    section("denial with feedback");
    {
        FakeServer fake;
        fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", "note.txt"}, {"content", "hello"}}}};
        fake.calls_left = 1;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Manual;
        Recorder r;
        r.reply = {Approval::No, "write to docs/ instead"};
        agent.submit("make a note", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].preview.find("new file, 1 lines") == 0, "a write asks with a preview of the new file");
        bool fed_back = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            if (m["role"] == "tool" && m["content"].get<std::string>().find("write to docs/ instead") != std::string::npos) fed_back = true;
        }
        expect(fed_back && !fs::exists(ws / "note.txt"), "the reason reaches the model as the tool result and nothing was written");
        std::string briefing = fake.requests[0]["messages"][0]["content"];
        expect(briefing.find("maic-workflow-edit inspect FILE --json") != std::string::npos, "the briefing names the workflow editor and how to start with it");
        expect(briefing.find("maic-storyboard start STORY TEMPLATE --out DEST") != std::string::npos && briefing.find("ask the user for the story file") != std::string::npos,
               "and the storyboard driver, beginning by asking the user for the files");
    }

    section("audit trail");
    {
        // Every entry of this run in id order, read from the containers (not index.json or seq).
        auto trail = [&] {
            std::vector<json> lines;
            for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec_ignore())) {
                if (e.path().extension() != ".jsonl") continue;
                std::ifstream in(e.path());
                for (std::string l; std::getline(in, l);) lines.push_back(json::parse(l));
            }
            std::sort(lines.begin(), lines.end(), [](const json& a, const json& b) { return a["id"] < b["id"]; });
            return lines;
        };
        auto raw_trail = [&] {
            std::string all;
            for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec_ignore())) {
                std::ifstream in(e.path());
                all += std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            return all;
        };
        const char* old_rt = std::getenv("XDG_RUNTIME_DIR");
        std::string saved_rt = old_rt ? old_rt : "";
        setenv("XDG_RUNTIME_DIR", (ws / "run").c_str(), 1);
        std::ofstream(ws / "trail-note.txt") << "file-content-marker\n";
        // read_file, a command, write_file; in edit mode the command (rm) is asked and the rest allowed.
        auto session = [&](SessionLog& log, bool on, Mode mode, Recorder& r) {
            FakeServer fake;
            int n = 0;
            fake.tool_call_for = [&](const json&) {
                switch (++n) {
                    case 1: return json{{"name", "read_file"}, {"arguments", {{"path", "trail-note.txt"}}}};
                    case 2: return json{{"name", "run_shell"}, {"arguments", {{"command", mode == Mode::Auto ? "echo $((6*7))" : "rm -f trail-nothing.txt"}}}};
                    case 3: return json{{"name", "write_file"}, {"arguments", {{"path", "trail-out.txt"}, {"content", "x"}}}};
                    default: return json();
                }
            };
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = mode;
            agent.review_with_model = false;
            agent.audit.enabled = on;
            agent.set_log(&log);
            agent.submit("prompt-text-marker: look at the note", Origin::Local, r, no_cancel);
        };

        {
            SessionLog log("agent-test");
            Recorder r;
            session(log, false, Mode::Auto, r);
            expect(!fs::exists(audit_trail_dir()), "off (the default): a session's tool calls write no trail at all");
            fs::remove(log.path());
            fs::remove(ws / "trail-out.txt");
        }

        SessionLog recorded("agent-test");
        Recorder r1;
        session(recorded, true, Mode::Auto, r1);
        std::vector<json> lines = trail();
        expect(lines.size() == 3, "on: one entry per tool call of a recorded session (" + std::to_string(lines.size()) + ")");
        std::string id = recorded.path().stem().string();
        bool shaped = lines.size() == 3;
        for (size_t k = 0; k < lines.size(); ++k) {
            const json& l = lines[k];
            shaped = shaped && l.value("id", 0) == static_cast<int>(k + 1) && l.value("session", "") == id && l.value("recorded", false) &&
                     l.value("workspace", "") == ws.string() && l.value("time", "").size() == 20 && l.value("decision", "") == "allow" &&
                     l.value("judged_by", "") == "harness" && l.value("ran", false) && l.value("ok", false) && !l.contains("result");
        }
        expect(shaped, "each has a monotonic id, the UTC time, session, recorded, workspace, the harness's decision, judged_by, ran and ok, and no result");
        expect(lines.size() == 3 && lines[0]["tool"] == "read_file" && lines[0]["arguments"]["path"] == "trail-note.txt" && lines[1]["arguments"]["command"] == "echo $((6*7))" &&
                   lines[1].value("exit", -1) == 0 && lines[2]["tool"] == "write_file",
               "the tool and its arguments as given, and a command's exit code");
        std::string raw = raw_trail();
        expect(raw.find("prompt-text-marker") == std::string::npos && raw.find("file-content-marker") == std::string::npos && raw.find("echo: ") == std::string::npos,
               "no message text and no tool output in the trail");
        struct stat st {};
        bool private_files = ::stat(audit_trail_dir().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700;
        size_t containers = 0;
        for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec_ignore())) {
            private_files = private_files && ::stat(e.path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600;
            containers += e.path().extension() == ".jsonl" && e.path().filename().string().size() == 14;
        }
        expect(private_files && containers == 1, "one <YYYYMMDD>.jsonl container beside seq, 0600 in a 0700 directory");
        std::ifstream seq_in(audit_trail_dir() / "seq");
        long long last = 0;
        seq_in >> last;
        expect(last == 3, "seq holds the last id given (" + std::to_string(last) + ")");
        fs::remove(recorded.path());
        fs::remove(ws / "trail-out.txt");

        SessionLog unrecorded("agent-test", runtime_sessions_dir());
        Recorder r2;
        r2.reply = {Approval::No, "typed-feedback-marker"};
        session(unrecorded, true, Mode::Edit, r2);
        // The two sessions can share a stem (same second, same name): this one's entries are the ones after id 3.
        std::vector<json> mine;
        for (const auto& l : trail()) {
            if (l.value("id", 0) > 3 && l.value("session", "") == unrecorded.path().stem().string()) mine.push_back(l);
        }
        expect(!unrecorded.recorded() && recorded.recorded() && mine.size() == 3 && !mine[0].value("recorded", true) && mine[0].value("id", 0) == 4,
               "an unrecorded session (the runtime directory) writes its entries too, marked unrecorded, the ids going on (" + std::to_string(mine.size()) + ")");
        bool denied = false;
        for (const auto& l : mine) {
            if (l["tool"] == "run_shell") {
                denied = l.value("decision", "") == "ask" && l.value("approval", "") == "no" && l.value("judged_by", "") == "user" && !l.value("ran", true) && !l.value("ok", true);
            }
        }
        expect(denied, "a call the user refused: the decision, the user's answer, judged_by user, not run");
        raw = raw_trail();
        expect(raw.find("typed-feedback-marker") == std::string::npos && raw.find("prompt-text-marker") == std::string::npos,
               "what the user typed with the refusal stays out of it");

        // A container past file_mb continues in a numbered part, the ids going on.
        append_audit_trail({{"tool", "read_file"}, {"arguments", {{"path", std::string(700 * 1024, 'x')}}}}, 1);
        append_audit_trail({{"tool", "read_file"}, {"arguments", {{"path", std::string(700 * 1024, 'y')}}}}, 1);
        append_audit_trail({{"tool", "read_file"}, {"arguments", {{"path", "small"}}}}, 1);
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(audit_trail_dir(), ec_ignore())) {
            if (e.path().extension() == ".jsonl") names.push_back(e.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        lines = trail();
        expect(names.size() == 2 && names[0].substr(8) == ".2.jsonl" && names[1].substr(8) == ".jsonl" && lines.size() == 9 && lines.back().value("id", 0) == 9,
               "file_mb: the next part is <YYYYMMDD>.2.jsonl once a line would pass the cap");
        fs::remove(unrecorded.path());
        fs::remove_all(audit_trail_dir());
        fs::remove(ws / "trail-note.txt");
        fs::remove(ws / "trail-out.txt");
        if (old_rt) setenv("XDG_RUNTIME_DIR", saved_rt.c_str(), 1);
        else unsetenv("XDG_RUNTIME_DIR");
    }

    section("undo points and nested instructions");
    {
        FakeServer fake;
        fs::create_directories(ws / "svc");
        std::ofstream(ws / "svc" / "AGENTS.md") << "svc rules: use tabs";
        std::ofstream(ws / "svc" / "a.txt") << "old\n";
        not_now(ws);
        {
            Agent untrusted(ws, "test");
            untrusted.providers = {fake.provider()};
            untrusted.mode = Mode::Auto;
            untrusted.review_with_model = false;
            Recorder r0;
            fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "svc/a.txt"}}}};
            fake.calls_left = 1;
            untrusted.submit("read it", Origin::Local, r0, no_cancel);
            expect(!r0.results.empty() && r0.results[0].find("svc rules") == std::string::npos, "an untrusted workspace's nested AGENTS.md is not attached");
        }
        trust_for_session(ws);
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;  // this section tests undo, not the reviewer
        Recorder r;
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "svc/a.txt"}}}};
        fake.calls_left = 1;
        agent.submit("read it", Origin::Local, r, no_cancel);
        expect(!r.results.empty() && r.results[0].find("svc rules: use tabs") != std::string::npos && r.results[0].find("[MAIC system note: standing instructions from " + (ws / "svc" / "AGENTS.md").string()) != std::string::npos,
               "reading a file attaches the AGENTS.md above it, as a marked system note naming the file");
        fake.calls_left = 1;
        r.results.clear();
        agent.submit("read again", Origin::Local, r, no_cancel);
        expect(!r.results.empty() && r.results[0].find("svc rules") == std::string::npos, "the same instructions are attached only once");

        fake.tool_call = json{{"name", "edit_file"}, {"arguments", {{"path", "svc/a.txt"}, {"old_string", "old"}, {"new_string", "new"}}}};
        fake.calls_left = 1;
        agent.submit("edit", Origin::Local, r, no_cancel);
        fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", "svc/b.txt"}, {"content", "made"}}}};
        fake.calls_left = 1;
        agent.submit("write", Origin::Local, r, no_cancel);
        std::ifstream a1(ws / "svc" / "a.txt");
        std::string a1s((std::istreambuf_iterator<char>(a1)), std::istreambuf_iterator<char>());
        expect(a1s == "new\n" && fs::exists(ws / "svc" / "b.txt") && agent.undo_points().size() == 2, "two changes, two undo points");
        std::string report = agent.undo(2);
        std::ifstream a2(ws / "svc" / "a.txt");
        std::string a2s((std::istreambuf_iterator<char>(a2)), std::istreambuf_iterator<char>());
        expect(a2s == "old\n" && !fs::exists(ws / "svc" / "b.txt") && report.find("restored") != std::string::npos && report.find("removed") != std::string::npos,
               "undo restores the edited file and removes the created one");
        expect(agent.undo() == "nothing to undo", "nothing left to undo");

        // delete_file keeps the content; move_file keeps the reverse move; each is one undo point.
        std::ofstream(ws / "svc" / "gone.txt") << "keep me\n";
        fake.tool_call = json{{"name", "delete_file"}, {"arguments", {{"path", "svc/gone.txt"}}}};
        fake.calls_left = 1;
        agent.submit("delete", Origin::Local, r, no_cancel);
        expect(!fs::exists(ws / "svc" / "gone.txt") && agent.undo_points().size() == 1, "a delete runs in auto mode and leaves one undo point");
        report = agent.undo();
        std::ifstream g(ws / "svc" / "gone.txt");
        std::string gs((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
        expect(gs == "keep me\n" && report.find("restored") == 0, "undo of a delete brings the file back: " + report);
        fake.tool_call = json{{"name", "move_file"}, {"arguments", {{"from", "svc/gone.txt"}, {"to", "svc/deep/moved.txt"}}}};
        fake.calls_left = 1;
        r.results.clear();
        agent.submit("move", Origin::Local, r, no_cancel);
        expect(!fs::exists(ws / "svc" / "gone.txt") && fs::exists(ws / "svc" / "deep" / "moved.txt") && agent.undo_points().size() == 1 && !agent.undo_points()[0].moved_to.empty(),
               "a move runs and leaves one reverse-move undo point");
        report = agent.undo();
        expect(fs::exists(ws / "svc" / "gone.txt") && !fs::exists(ws / "svc" / "deep" / "moved.txt") && report.find("moved ") == 0 && report.find("back to") != std::string::npos,
               "undo of a move moves the file back: " + report);
        // A move whose destination is outside the workspace asks; No leaves both ends untouched and no undo point.
        fs::path out_of_ws = ws.string() + "-out.txt";
        fake.tool_call = json{{"name", "move_file"}, {"arguments", {{"from", "svc/gone.txt"}, {"to", out_of_ws.string()}}}};
        fake.calls_left = 1;
        r.asked.clear();
        r.results.clear();
        agent.submit("move out", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].reason == "outside the workspace" && r.asked[0].summary.find("move_file svc/gone.txt -> ") == 0 && fs::exists(ws / "svc" / "gone.txt") &&
                   !fs::exists(out_of_ws) && r.results.size() == 1 && r.results[0].find("DENIED") == 0 && agent.undo_points().empty(),
               "a move out of the workspace asks on its destination; No moves nothing and saves nothing");
        // A patch touching /etc trips at that file before any file is written; the workspace file in the same patch stays as it was.
        std::ofstream(ws / "svc" / "ok.txt") << "same\n";
        fake.tool_call = json{{"name", "apply_patch"}, {"arguments", {{"patch", "--- svc/ok.txt\n+++ svc/ok.txt\n@@ -1 +1 @@\n-same\n+changed\n--- /etc/hosts\n+++ /etc/hosts\n@@ -1 +1 @@\n-a\n+b\n"}}}};
        fake.calls_left = 1;
        r.results.clear();
        agent.submit("patch", Origin::Local, r, no_cancel);
        std::ifstream ok(ws / "svc" / "ok.txt");
        std::string oks((std::istreambuf_iterator<char>(ok)), std::istreambuf_iterator<char>());
        expect(r.results.size() == 1 && r.results[0].find("BLOCKED") == 0 && oks == "same\n" && tripwire_state(), "a patch with a file under /etc is blocked before anything is written, and trips");
        fs::remove(std::getenv("MAIC_TRIPWIRE_FILE"));
        expect(!tripwire_state(), "the test lock is cleared again");
    }

    section("repeated calls and budgets");
    {
        FakeServer fake;
        std::ofstream(ws / "same.txt") << "x";
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.repeat_trip = 100;  // never trip the real lock from a test
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "same.txt"}}}};
        fake.calls_left = 4;
        Recorder r;
        agent.submit("loop", Origin::Local, r, no_cancel);
        int refused = 0;
        for (const auto& t : r.results) refused += t.find("REFUSED") == 0;
        expect(r.results.size() == 4 && refused == 2, "the third and fourth identical calls are refused (" + std::to_string(refused) + ")");
        // Five harmless repeats end the turn instead of tripping the lock.
        Agent h(ws, "test");
        h.providers = {fake.provider()};
        h.mode = Mode::Auto;
        h.review_with_model = false;
        fake.calls_left = 8;
        Recorder rh;
        h.submit("loop", Origin::Local, rh, no_cancel);
        bool ended = false;
        for (const auto& n : rh.notices) ended = ended || n.find("kept repeating the same harmless call") != std::string::npos;
        expect(ended && rh.results.size() == 5 && rh.results.back().find("The turn ends here") != std::string::npos && !tripwire_state(), "a fifth harmless repeat ends the turn with a note and never trips");

        Agent b(ws, "test");
        b.providers = {fake.provider()};
        b.budget_tokens = 100;
        fake.usage_input = 100;
        Recorder rb;
        b.submit("one", Origin::Local, rb, no_cancel);
        b.submit("two", Origin::Local, rb, no_cancel);
        bool stopped = false;
        for (const auto& n : rb.notices) stopped = stopped || n.find("token budget reached") == 0;
        expect(stopped && b.messages().back().content.find("budget is used up") != std::string::npos, "a used-up budget stops the next turn with a notice");
    }

    section("question and todo");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        r.answer = "the second one";
        fake.tool_call = json{{"name", "question"}, {"arguments", {{"question", "Which config?"}, {"options", {"dev", "prod"}}}}};
        fake.calls_left = 1;
        agent.submit("ask me", Origin::Local, r, no_cancel);
        expect(r.questions.size() == 1 && r.questions[0].first == "Which config?" && r.questions[0].second == std::vector<std::string>{"dev", "prod"},
               "the question reaches the front end with its options");
        expect(r.results.size() == 1 && r.results[0] == "the second one" && r.asked.empty(), "the answer is the tool result and no approval was involved");
        std::string sys = fake.requests[0]["messages"][0]["content"];
        expect(sys.find("question asks the user") != std::string::npos && sys.find("todo is your plan") != std::string::npos, "the briefing explains both tools");
        r.answer.clear();
        r.results.clear();
        fake.calls_left = 1;
        agent.submit("ask again", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "(the user gave no answer)", "no answer is reported as such");

        fake.tool_call = json{{"name", "todo"}, {"arguments", {{"items", {{{"text", "read the code"}, {"done", true}}, {{"text", "edit it"}}}}}}};
        fake.calls_left = 1;
        r.results.clear();
        agent.submit("plan", Origin::Local, r, no_cancel);
        expect(r.todos.size() == 1 && r.todos[0].size() == 2 && r.todos[0][0].done && r.todos[0][0].text == "read the code" && !r.todos[0][1].done,
               "on_todo gets the list with its done flags");
        expect(agent.todo().size() == 2 && r.results.size() == 1 && r.results[0].rfind("todo: 1/2 done", 0) == 0 && r.results[0].find("[ ] edit it") != std::string::npos,
               "the agent keeps the plan and the result counts it: " + (r.results.empty() ? "" : r.results[0]));
        fake.tool_call = json{{"name", "todo"}, {"arguments", {{"items", {"only this"}}}}};
        fake.calls_left = 1;
        agent.submit("replan", Origin::Local, r, no_cancel);
        expect(agent.todo().size() == 1 && agent.todo()[0].text == "only this" && !agent.todo()[0].done, "a new list replaces the old one (plain strings are accepted)");
        fake.tool_call = json{{"name", "todo"}, {"arguments", {{"text", "no items"}}}};
        fake.calls_left = 1;
        r.results.clear();
        agent.submit("bad", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0].rfind("error:", 0) == 0 && agent.todo().size() == 1, "a todo call without items is an error and keeps the old list");
        agent.clear();
        expect(agent.todo().empty(), ":clear drops the plan");
    }

    section("subagents");
    {
        FakeServer fake;
        fake.usage_input = 10;
        std::ofstream(ws / "facts.txt") << "the river is wide\n";
        auto task_call = [](const char* agent, const char* task) {
            return json{{"name", "task"}, {"arguments", {{"agent", agent}, {"prompt", task}, {"context", "look in facts.txt"}}}};
        };
        // explore: the parent hands over one job; the child reads, is refused a write tool, a writing command, and answers.
        {
            int parent_calls = 0, child_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json {
                if (!from_child(b)) return ++parent_calls == 1 ? task_call("explore", "find the river line") : json();
                switch (++child_calls) {
                    case 1: return json{{"name", "read_file"}, {"arguments", {{"path", "facts.txt"}}}};
                    case 2: return json{{"name", "write_file"}, {"arguments", {{"path", "out.txt"}, {"content", "x"}}}};
                    case 3: return json{{"name", "run_shell"}, {"arguments", {{"command", "touch out.txt"}}}};
                    default: return json();
                }
            };
            SessionLog log("agent-test");
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = false;
            agent.set_log(&log);
            Recorder r;
            agent.submit("what does facts.txt say about the river?", Origin::Local, r, no_cancel);
            expect(has_call(r, "task explore: find the river line") && has_call(r, "↳ explore: read_file facts.txt") && has_call(r, "↳ explore: write_file out.txt"),
                   "the child's tool calls reach the parent's front end with the agent prefix");
            expect(has_call(r, "↳ explore on test (the session's model)"), "the task call shows the child's model and why");
            expect(has_result(r, "the river is wide") && r.asked.empty(), "explore read the file in auto-read without asking");
            expect(has_result(r, "DENIED: the explore agent has no write_file tool") && has_result(r, "DENIED: the explore agent runs only read-only commands") && !fs::exists(ws / "out.txt"),
                   "a write tool and a writing command are denied by the agent, with its name");
            std::string final = r.results.empty() ? "" : r.results.back();
            expect(final.find("echo: find the river line") == 0 && final.find("Context from the parent agent:\nlook in facts.txt") != std::string::npos && final.find("\n\n(the subagent used 4 steps, 15 tokens)") != std::string::npos,
                   "the child's final answer is the tool result, task and context included, ending with its usage: " + final);
            expect(agent.usage().total_input == 20 && agent.usage().total_output == 10, "the child's tokens count against the parent's totals");
            json child_req, parent_req;
            for (const auto& q : fake.requests) (from_child(q) ? child_req : parent_req) = q;
            expect(offers_tool(parent_req, "task") && parent_req["messages"][0]["content"].get<std::string>().find("task hands one job to a subagent") != std::string::npos,
                   "the parent is offered task and briefed on when to use it");
            expect(!offers_tool(child_req, "task") && !offers_tool(child_req, "question") && !offers_tool(child_req, "todo") && !offers_tool(child_req, "write_file") && offers_tool(child_req, "read_file"),
                   "the child is offered only the agent's tools, never task, question or todo");
            std::string child_sys = child_req["messages"][0]["content"];
            expect(child_sys.find("as the explore agent, which is read-only") != std::string::npos && child_sys.find("Mode: auto-read") != std::string::npos && child_sys.find("question, todo and task are not available") != std::string::npos,
                   "the child's briefing names its agent, its mode and what it lacks");
            bool listed = false;
            for (const auto& s : list_sessions(ws)) {
                if (s.kind == "sub" && s.delegated_from == log.path().stem().string() && s.agent == "explore") {
                    listed = true;
                    expect(s.path.parent_path() == log.path().parent_path() && s.first_prompt.find("find the river line") == 0, "the child's transcript sits in the parent's home with the task as its first prompt");
                    fs::remove(s.path);
                }
            }
            expect(listed, "the child has a transcript of kind sub naming its parent and agent");
            fs::remove(log.path());
        }
        // A child cannot run task, and the parent in manual mode gives a child in an auto agent no more than manual.
        {
            int parent_calls = 0, child_calls = 0;
            size_t requests_before = fake.requests.size();
            fake.tool_call_for = [&](const json& b) -> json {
                if (!from_child(b)) return ++parent_calls == 1 ? task_call("fast", "run echo") : json();
                switch (++child_calls) {
                    case 1: return task_call("explore", "go deeper");
                    case 2: return json{{"name", "run_shell"}, {"arguments", {{"command", "echo hi"}}}};
                    default: return json();
                }
            };
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Manual;
            agent.review_with_model = false;
            agent.agents.push_back({"fast", Mode::Auto});
            agent.agents.push_back({"boss", Mode::Auto, Role::Primary});
            Recorder r;
            r.reply = {Approval::Yes, ""};
            agent.submit("echo something", Origin::Local, r, no_cancel);
            expect(has_result(r, "DENIED: a subagent has no task tool"), "a child's task call is refused: one level only");
            int children = 0;
            for (size_t i = requests_before; i < fake.requests.size(); ++i) children += from_child(fake.requests[i]);
            expect(children == 3, "no grandchild was started (" + std::to_string(children) + " child requests)");
            expect(r.asked.size() == 1 && r.asked[0].summary == "fast: $ echo hi" && r.asked[0].tool == "run_shell", "the auto agent's command is asked through the parent, under a manual session, with the agent named");
            expect(has_result(r, "hi"), "and runs once approved");
            json child_req;
            for (const auto& q : fake.requests) if (from_child(q)) child_req = q;
            expect(child_req["messages"][0]["content"].get<std::string>().find("Mode: manual") != std::string::npos, "the child was told its mode is manual");
            bool unknown = false;
            parent_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json { return from_child(b) || ++parent_calls > 1 ? json() : task_call("nobody", "x"); };
            Recorder r2;
            agent.submit("again", Origin::Local, r2, no_cancel);
            for (const auto& t : r2.results) unknown = unknown || t.find("error: no agent named 'nobody'. The agents task can run are: plan, general, explore, fast.") == 0;
            expect(unknown, "an unknown agent is an error naming the ones task can run");
            bool primary = false;
            parent_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json { return from_child(b) || ++parent_calls > 1 ? json() : task_call("build", "x"); };
            Recorder r3;
            agent.submit("again", Origin::Local, r3, no_cancel);
            for (const auto& t : r3.results) primary = primary || t.find("error: the build agent is a primary agent, which task cannot run. The agents task can run are: plan, general, explore, fast.") == 0;
            expect(primary, "a primary agent is refused, with the eligible ones listed");
            parent_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json { return from_child(b) || ++parent_calls > 1 ? json() : task_call("scout", "look"); };
            Recorder r4;
            agent.submit("again", Origin::Local, r4, no_cancel);
            expect(has_call(r4, "↳ explore on test") && !r4.results.empty() && r4.results.back().find("echo: look") == 0, "an older name (scout) runs the agent it became (explore)");
        }
        // Budgets stop a runaway child: steps from the agent, tokens from the agent.
        {
            int parent_calls = 0, child_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json {
                if (!from_child(b)) return ++parent_calls == 1 ? task_call("tiny", "read everything") : json();
                return json{{"name", "read_file"}, {"arguments", {{"path", "facts.txt"}, {"offset", ++child_calls}}}};
            };
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = false;
            AgentDef tiny{"tiny", Mode::AutoRead};
            tiny.max_steps = 3;
            tiny.tools = {"read_file"};
            agent.agents.push_back(tiny);
            Recorder r;
            agent.submit("go", Origin::Local, r, no_cancel);
            expect(child_calls == 3 && has_notice(r, "tiny: stopped after 3 steps (the agent's limit)"), "the child stops at the agent's step limit, and the user is told (" + std::to_string(child_calls) + " calls)");
            expect(!r.results.empty() && r.results.back() == "the subagent gave no final answer (the subagent used 3 steps, 0 tokens)", "the result says it never answered: " + r.results.back());
            child_calls = 0;
            fake.usage_on_calls = true;
            AgentDef spend{"spend", Mode::AutoRead};
            spend.budget_tokens = 20;
            spend.tools = {"read_file"};
            agent.agents.push_back(spend);
            fake.tool_call_for = [&](const json& b) -> json {
                if (!from_child(b)) return parent_calls++ == 2 ? task_call("spend", "read everything") : json();
                return json{{"name", "read_file"}, {"arguments", {{"path", "facts.txt"}, {"offset", ++child_calls}}}};
            };
            Recorder r2;
            agent.submit("again", Origin::Local, r2, no_cancel);
            expect(child_calls == 2 && has_notice(r2, "spend: token budget reached (30 of 20)"), "the child stops at the agent's token budget (" + std::to_string(child_calls) + " calls)");
            fake.usage_on_calls = false;
        }
        // A tripped tripwire stops the child, and the parent is told.
        {
            int parent_calls = 0, child_calls = 0;
            fake.tool_call_for = [&](const json& b) -> json {
                if (!from_child(b)) return ++parent_calls == 1 ? task_call("explore", "list things") : json();
                return ++child_calls == 1 ? json{{"name", "run_shell"}, {"arguments", {{"command", "sudo ls"}}}} : json{{"name", "read_file"}, {"arguments", {{"path", "facts.txt"}}}};
            };
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = false;
            Recorder r;
            agent.submit("look around", Origin::Local, r, no_cancel);
            expect(tripwire_state() && child_calls == 1 && has_notice(r, "explore: the harness is tripped; the subagent stops here"), "the child's trip ends its turn at once (" + std::to_string(child_calls) + " child calls)");
            expect(!r.results.empty() && r.results.back().find("BLOCKED and the harness was tripped during the subagent's work") == 0, "the parent's result says so: " + r.results.back());
            fs::remove(std::getenv("MAIC_TRIPWIRE_FILE"));
            expect(!tripwire_state(), "the test lock is cleared again");
        }
        fake.tool_call_for = nullptr;
        fake.usage_input = 0;
        fs::remove(ws / "facts.txt");
    }

    section("subagent models and usage limits");
    {
        // Two providers: fa serves Fable (limited), fb serves Opus, Sonnet and Haiku. The presets are the
        // shipped ones with their models pointed at the fakes.
        FakeServer fa, fb;
        Provider pa = fa.provider(), pb = fb.provider();
        pa.name = "fa";
        pb.name = "fb";
        std::vector<ModelPreset> presets = default_presets();
        for (auto& p : presets) {
            if (p.name == "fable-5.1") p.model = "fa/fable";
            if (p.name == "opus-5.5") p.model = "fb/opus";
            if (p.name == "sonnet-5") p.model = "fb/sonnet";
            if (p.name == "haiku-4.5") p.model = "fb/haiku";
        }
        const std::string fable_body = R"({"type":"error","error":{"type":"rate_limit_error","message":"You've reached your Fable limit. Run /usage-credits to continue or switch models with /model."}})";
        auto task_call = [](const char* agent, const char* prompt, const char* model = nullptr) {
            json args = {{"agent", agent}, {"prompt", prompt}};
            if (model) args["model"] = model;
            return json{{"name", "task"}, {"arguments", args}};
        };
        auto models_of = [](const FakeServer& f, bool child) {
            std::vector<std::string> out;
            for (const auto& q : f.requests) if (from_child(q) == child) out.push_back(q.value("model", ""));
            return out;
        };
        auto records = [](const fs::path& file, const std::string& type) {
            std::vector<json> out;
            std::ifstream in(file);
            for (std::string l; std::getline(in, l);) {
                json j = json::parse(l, nullptr, false);
                if (j.is_object() && j.value("type", "") == type) out.push_back(j);
            }
            return out;
        };
        auto make = [&](const std::string& model) {
            auto a = std::make_unique<Agent>(ws, model);
            a->providers = {pa, pb};
            a->presets = presets;
            a->mode = Mode::Auto;
            a->review_with_model = false;
            return a;
        };
        auto reset = [&] {
            fa.requests.clear();
            fb.requests.clear();
            fa.fail_when = fb.fail_when = nullptr;
        };

        // A parent on the limited Fable hands its subagent to Opus, and is told so.
        {
            reset();
            int parent_calls = 0;
            fa.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent_calls == 1 ? task_call("explore", "map the repo") : json(); };
            fb.tool_call_for = nullptr;
            SessionLog log("agent-test");
            auto agent = make("fa/fable");
            agent->set_log(&log);
            Recorder r;
            agent->submit("look around", Origin::Local, r, no_cancel);
            expect(models_of(fb, true) == std::vector<std::string>{"opus"} && models_of(fa, true).empty(), "the child of a limited Fable runs on Opus, on its own provider");
            expect(has_call(r, "↳ explore on opus-5.5 (fable-5.1 is limited)"), "the task line shows the child's model and why");
            std::string desc;
            for (const auto& t : fa.requests[0]["tools"]) if (t["function"]["name"] == "task") desc = t["function"]["description"];
            expect(desc.find("opus-5.5 (tier 40; the default: fable-5.1 is limited), fable-5.1 (tier 50, limited), sonnet-5 (tier 30), haiku-4.5 (tier 20)") != std::string::npos &&
                       desc.find("lower tier for wide reads") != std::string::npos && desc.find("explore (Fast agent") != std::string::npos,
                   "the task tool lists the presets with tiers, the default first and why, and the agents: " + desc);
            std::string brief = fa.requests[0]["messages"][0]["content"];
            expect(brief.find("task's model: Its `model` may name one of these presets, the default first: opus-5.5") != std::string::npos, "and the briefing says the same");
            auto tools = records(log.path(), "tool");
            expect(!tools.empty() && tools[0].value("agent", "") == "explore" && tools[0].value("model", "") == "fb/opus" && tools[0].value("model_reason", "") == "fable-5.1 is limited",
                   "the parent's tool record has the agent, the model and the reason");
            fs::path child = tools.empty() ? fs::path() : fs::path(tools[0].value("child", ""));
            auto start = records(child, "start");
            expect(!start.empty() && start[0].value("agent", "") == "explore" && start[0].value("model", "") == "fb/opus" && start[0].value("model_reason", "") == "fable-5.1 is limited",
                   "and the child's start record");
            fs::remove(child);
            fs::remove(log.path());
        }
        // The model argument: anything on the list, higher or lower; anything else is an error listing them.
        {
            reset();
            int parent_calls = 0;
            const char* want = "haiku-4.5";
            fa.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent_calls == 1 ? task_call("explore", "count files", want) : json(); };
            auto agent = make("fa/fable");
            Recorder r;
            agent->submit("count", Origin::Local, r, no_cancel);
            expect(models_of(fb, true) == std::vector<std::string>{"haiku"} && has_call(r, "↳ explore on haiku-4.5 (asked for by the parent)"), "the parent may ask for a lower tier");
            reset();
            parent_calls = 0;
            want = "qwen-4b";
            Recorder r2;
            agent->submit("count again", Origin::Local, r2, no_cancel);
            expect(has_result(r2, "error: 'qwen-4b' is not a model this session may hand a task to. The allowed models are: fable-5.1 (tier 50, limited), opus-5.5 (tier 40), sonnet-5 (tier 30), haiku-4.5 (tier 20).") &&
                       models_of(fa, true).empty() && models_of(fb, true).empty(),
                   "a model off the list is an error naming the allowed ones with their tiers, and no child runs");
            // An agent's model is the user's pin: the argument does not move it.
            reset();
            parent_calls = 0;
            want = "haiku-4.5";
            AgentDef pinned{"pinned", Mode::AutoRead, Role::Subagent};
            pinned.model = "fb/sonnet";
            agent->agents.push_back(pinned);
            fa.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent_calls == 1 ? task_call("pinned", "count", want) : json(); };
            Recorder r3;
            agent->submit("pinned", Origin::Local, r3, no_cancel);
            expect(models_of(fb, true) == std::vector<std::string>{"sonnet"} && has_call(r3, "↳ pinned on sonnet-5 (the pinned agent's model; the model argument does not override it)"),
                   "an agent's model wins over the call's model argument");
        }
        // A usage limit mid-task: the child continues on the preset's on_limit model, once.
        {
            reset();
            int parent_calls = 0, child_calls = 0;
            std::ofstream(ws / "limits.txt") << "a line\n";
            fa.tool_call_for = [&](const json& b) -> json {
                if (from_child(b)) return ++child_calls == 1 ? json{{"name", "read_file"}, {"arguments", {{"path", "limits.txt"}}}} : json();
                return ++parent_calls == 1 ? task_call("explore", "read limits.txt", "fable-5.1") : json();
            };
            fb.tool_call_for = nullptr;
            // Fable answers the child's first call, then is out: every later child request gets the 429.
            fa.fail_when = [&](const json& b) { return from_child(b) && child_calls >= 1; };
            fa.fail_status = 429;
            fa.fail_body = fable_body;
            SessionLog log("agent-test");
            auto agent = make("fa/fable");
            agent->set_log(&log);
            Recorder r;
            agent->submit("read it", Origin::Local, r, no_cancel);
            expect(has_notice(r, "explore: fable-5.1 hit its usage limit; continuing on opus-5.5"), "the user is told the child switched");
            json after;
            for (const auto& q : fb.requests) if (from_child(q)) after = q;
            bool kept = false;
            for (const auto& m : after.value("messages", json::array())) kept = kept || (m["role"] == "tool" && FakeServer::text_of(m["content"]).find("a line") != std::string::npos);
            expect(models_of(fb, true) == std::vector<std::string>{"opus"} && kept, "Opus continues the same turn with the conversation so far (the read's result included)");
            expect(!r.results.empty() && r.results.back().find("echo: ") == 0, "and the parent gets the child's answer: " + (r.results.empty() ? "" : r.results.back()));
            auto tools = records(log.path(), "tool");
            fs::path child = tools.empty() ? fs::path() : fs::path(tools.back().value("child", ""));
            auto sw = records(child, "model");
            expect(sw.size() == 1 && sw[0].value("from", "") == "fable-5.1" && sw[0].value("to", "") == "opus-5.5" && sw[0].value("model", "") == "fb/opus" && sw[0].value("reason", "") == "usage limit",
                   "the child's transcript records the switch");
            fs::remove(child);
            fs::remove(log.path());

            // A second limit ends the child, naming both.
            reset();
            parent_calls = child_calls = 0;
            fa.fail_when = [&](const json& b) { return from_child(b) && child_calls >= 1; };
            fb.fail_when = [&](const json& b) { return from_child(b); };
            fb.fail_status = 429;
            fb.fail_body = R"({"type":"error","error":{"type":"rate_limit_error","message":"You have reached your specified workspace API usage limits."}})";
            Recorder r2;
            agent->submit("read it again", Origin::Local, r2, no_cancel);
            expect(!r2.results.empty() && r2.results.back().find("error: the subagent failed: opus-5.5 hit its usage limit after fable-5.1 did; the subagent stops here") == 0,
                   "a second limit ends the child with an error naming both models: " + (r2.results.empty() ? "" : r2.results.back()));
            fs::remove(ws / "limits.txt");
        }
        // The session itself never switches: the limit is its error.
        {
            reset();
            fa.tool_call_for = nullptr;
            fa.fail_when = [](const json&) { return true; };
            auto agent = make("fa/fable");
            Recorder r;
            std::string err;
            try {
                agent->submit("hello", Origin::Local, r, no_cancel);
            } catch (const ApiError& e) {
                err = is_usage_limit(e) ? "limit" : e.what();
            }
            expect(err == "limit" && agent->model == "fa/fable" && fa.requests.size() == 1, "a usage limit on the session's own model is thrown once, unretried, and the model stays");
        }
        // A subagent's reviewer follows the child's own model.
        {
            reset();
            int parent_calls = 0, child_calls = 0;
            auto is_review = [](const json& b) { return b["messages"][0]["content"].get<std::string>().rfind("You review one action", 0) == 0; };
            fb.tool_call_for = [&](const json& b) -> json {
                if (is_review(b)) return json();
                if (from_child(b)) return ++child_calls == 1 ? json{{"name", "write_file"}, {"arguments", {{"path", "child-review.txt"}, {"content", "x"}}}} : json();
                return ++parent_calls == 1 ? task_call("general", "write the file", "haiku-4.5") : json();
            };
            fb.reply = [&](const json& b) { return is_review(b) ? std::string("ALLOW: fine") : std::string(); };
            auto agent = make("fb/opus");
            agent->review_with_model = true;
            Recorder r;
            agent->submit("write it", Origin::Local, r, no_cancel);
            std::string review_model;
            for (const auto& q : fb.requests) if (is_review(q)) review_model = q.value("model", "");
            expect(fs::exists(ws / "child-review.txt") && review_model == "haiku", "a child on Haiku reviews on Haiku (" + review_model + ")");
            fs::remove(ws / "child-review.txt");
            fb.reply = nullptr;
        }
        fa.tool_call_for = fb.tool_call_for = nullptr;
        reset();
    }

    section("deepseek: the agent, a subagent and a metered pick");
    {
        FakeDeepSeek ds;
        Provider dsp;
        for (const auto& p : default_providers()) if (p.name == "deepseek") dsp = p;
        dsp.base_url = ds.url();
        setenv("DEEPSEEK_API_KEY", ds.key.c_str(), 1);
        FakeServer lab;
        Provider labp = lab.provider();
        labp.name = "lab";
        std::vector<ModelPreset> presets = default_presets();
        ModelPreset local{"lab-model", "lab/m", 16384, "same", 0, 10, false, {"deepseek-flash"}};
        presets.push_back(local);
        auto make = [&](const std::string& preset) {
            auto a = std::make_unique<Agent>(ws, find_preset(presets, preset)->model);
            a->providers = {dsp, labp};
            a->presets = presets;
            a->think = find_preset(presets, preset)->think == 1;
            set_preset_window(a->providers, *find_preset(presets, preset));
            a->mode = Mode::Auto;
            a->review_with_model = false;
            return a;
        };
        auto task_call = [](const char* model) { return json{{"name", "task"}, {"arguments", {{"agent", "explore"}, {"prompt", "look"}, {"model", model}}}}; };
        bool bwrap = false;
        for (const auto& d : {"/usr/bin/bwrap", "/usr/local/bin/bwrap", "/run/current-system/sw/bin/bwrap"}) bwrap = bwrap || fs::exists(d);

        // The main agent on deepseek-pro, thinking, over two turns of tool use: every request carries the earlier
        // turns' reasoning (the fake refuses one without), and the key reaches neither a command nor the transcript.
        {
            SessionLog log("agent-test-deepseek");
            auto a = make("deepseek-pro");
            a->set_log(&log);
            int n = 0;
            ds.tool_call_for = [&](const json&) -> json {
                ++n;
                if (n == 1) return bwrap ? json{{"name", "run_shell"}, {"arguments", {{"command", "env; echo key=$DEEPSEEK_API_KEY"}}}} : json{{"name", "list_dir"}, {"arguments", {{"path", "."}}}};
                if (n == 3) return json{{"name", "list_dir"}, {"arguments", {{"path", "."}}}};
                return json();
            };
            Recorder r;
            a->submit("look at the environment", Origin::Local, r, no_cancel);
            a->submit("and once more", Origin::Local, r, no_cancel);
            expect(ds.refused.empty() && ds.served == 4 && ds.last()["model"] == "deepseek-v4-pro" && ds.last()["thinking"] == json{{"type", "enabled"}},
                   "two turns with tool calls on deepseek-pro, thinking on, nothing refused: " + (ds.refused.empty() ? std::string() : ds.refused[0]));
            int replayed = 0;
            const json last = ds.last();
            for (const auto& m : last["messages"]) if (m["role"] == "assistant" && m.value("reasoning_content", "").rfind("thought ", 0) == 0) ++replayed;
            expect(replayed == 3, "the last request replays all three earlier assistant turns' reasoning");
            if (bwrap) expect(has_result(r, "key="), "the command ran");
            bool leaked = false;
            for (const auto& t : r.results) leaked = leaked || t.find(ds.key) != std::string::npos;
            std::ifstream in(log.path());
            std::string transcript((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            expect(!leaked && transcript.find(ds.key) == std::string::npos && transcript.find("thought 1") != std::string::npos,
                   "the key is in no tool result and not in the transcript, which keeps the reasoning to replay on resume");
            fs::remove(log.path());
        }
        // A subagent from deepseek-pro on deepseek-flash: the same account, so no question.
        {
            ds.requests.clear();
            int parent = 0;
            ds.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent == 1 ? task_call("deepseek-flash") : json(); };
            auto a = make("deepseek-pro");
            Recorder r;
            a->submit("delegate", Origin::Local, r, no_cancel);
            std::vector<std::string> child;
            for (const auto& q : ds.requests) if (from_child(q)) child.push_back(q.value("model", ""));
            expect(child == std::vector<std::string>{"deepseek-flash"} && r.questions.empty() && has_call(r, "explore on deepseek-flash (asked for by the parent)"),
                   "the parent may hand a task to deepseek-flash on its own account without a question");
            std::string desc;
            for (const auto& t : ds.requests[0]["tools"]) if (t["function"]["name"] == "task") desc = t["function"]["description"];
            expect(desc.find("deepseek-flash (tier 25)") != std::string::npos && desc.find("metered") == std::string::npos, "and the list shows no meter for its own account: " + desc);
        }
        // From a local session the parent model asking for deepseek-flash is not Micaiah asking: she is asked.
        {
            ds.requests.clear();
            ds.tool_call_for = nullptr;
            int parent = 0;
            lab.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent == 1 ? task_call("deepseek-flash") : json(); };
            auto a = make("lab-model");
            Recorder r;
            a->submit("delegate", Origin::Local, r, no_cancel);
            expect(r.questions.size() == 1 && r.questions[0].first.find("billed per token to your deepseek account") != std::string::npos && ds.requests.empty() &&
                       has_call(r, "the same model"),
                   "unanswered, the subagent stays on the local model and nothing reaches DeepSeek");
            std::string desc;
            for (const auto& t : lab.requests[0]["tools"]) if (t["function"]["name"] == "task") desc = t["function"]["description"];
            expect(desc.find("deepseek-flash (tier 25, metered: the user is asked first)") != std::string::npos, "the task tool says a metered model asks first: " + desc);
            parent = 0;
            Recorder yes;
            yes.answer = "yes";
            a->submit("delegate again", Origin::Local, yes, no_cancel);
            expect(yes.questions.size() == 1 && ds.requests.size() == 1 && ds.requests[0]["model"] == "deepseek-flash", "approved, it runs there");
            lab.tool_call_for = nullptr;
        }
        // No rule steps onto a metered model of another provider: a limited local preset with only DeepSeek on its list stays.
        {
            ModelPreset limited = local;
            limited.limited = true;
            ModelPick pick = subagent_pick(presets, limited);
            expect(pick.model == "lab/m" && !on_limit_pick(presets, limited) && default_small_model(presets, default_providers(), find_preset(presets, "deepseek-pro").value()).model == "deepseek/deepseek-flash",
                   "the step-aside and on_limit rules skip a metered preset on another account; within DeepSeek's own list they pick freely");
        }
        unsetenv("DEEPSEEK_API_KEY");
    }

    section("lua tools through the agent");
    {
        fs::path cfg = ws / "cfg";
        fs::create_directories(cfg);
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        fs::create_directories(ws / ".maic" / "tools");
        std::ofstream(ws / ".maic" / "tools" / "read_note.lua") << "return {\n"
                                                                    "  name = 'read_note', description = 'reads note.txt',\n"
                                                                    "  parameters = { type = 'object', properties = {} },\n"
                                                                    "  run = function(args) return maic.read('note.txt') end,\n"
                                                                    "}\n";
        std::ofstream(ws / ".maic" / "tools" / "escape.lua") << "return {\n"
                                                                 "  name = 'escape', description = 'writes a file wherever it is told',\n"
                                                                 "  parameters = { type = 'object', properties = { where = { type = 'string' } }, required = { 'where' } },\n"
                                                                 "  run = function(args) maic.write(args.where, 'x') return 'wrote ' .. args.where end,\n"
                                                                 "}\n";
        std::ofstream(ws / "note.txt") << "the note says heron\n";
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        expect(agent.tools().size() == 2 && agent.tool_notices().empty(), "both tool files load");
        Recorder r;
        fake.tool_call = json{{"name", "read_note"}, {"arguments", json::object()}};
        fake.calls_left = 1;
        agent.submit("read", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "the note says heron\n" && r.asked.empty(), "a Lua tool reads a workspace file in auto mode without asking");
        bool listed = false;
        for (const auto& t : fake.requests[0]["tools"]) listed = listed || t["function"]["name"] == "read_note";
        std::string sys = fake.requests[0]["messages"][0]["content"];
        expect(listed && sys.find("read_note") != std::string::npos, "the model is offered the Lua tool beside the built-ins and the briefing names it");

        fs::path outside = ws.string() + "-escape.txt";
        fs::remove(outside);
        r.reply = {Approval::No, "keep it in the workspace"};
        r.results.clear();
        fake.tool_call = json{{"name", "escape"}, {"arguments", {{"where", outside.string()}}}};
        fake.calls_left = 1;
        agent.submit("escape", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].tool == "escape" && r.asked[0].summary.rfind("write_file", 0) == 0 && r.asked[0].preview.find("new file") == 0,
               "the write inside the tool is asked about like a built-in, with a preview");
        expect(r.results.size() == 1 && r.results[0] == "DENIED by the user, who says: keep it in the workspace" && !fs::exists(outside),
               "the denial is the Lua error and the tool result, and nothing was written: " + (r.results.empty() ? "" : r.results[0]));
        const Message* last_tool = nullptr;
        for (const auto& m : agent.messages()) if (m.role == "tool") last_tool = &m;
        expect(last_tool && last_tool->is_error && last_tool->tool_name == "escape", "the model sees it as a failed call of the tool");
        fs::remove_all(ws / ".maic");
        unsetenv("XDG_CONFIG_HOME");
    }

    section("script tools through the agent: the two shipped examples");
    {
        fs::path cfg = ws / "cfg";
        fs::create_directories(cfg);
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        fs::create_directories(ws / ".maic" / "tools");
        fs::copy(fs::path(MAIC_EXAMPLES) / "word_count", ws / ".maic" / "tools" / "word_count", fs::copy_options::recursive);
        fs::copy(fs::path(MAIC_EXAMPLES) / "json_pick", ws / ".maic" / "tools" / "json_pick", fs::copy_options::recursive);
        fs::create_directories(ws / ".maic" / "tools" / "writer");
        std::ofstream(ws / ".maic" / "tools" / "writer" / "tool.json")
            << R"({"name": "writer", "description": "writes out/result.txt", "parameters": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
                   "run": ["sh", "main.sh"], "writes": ["out/*.txt"]})";
        std::ofstream(ws / ".maic" / "tools" / "writer" / "main.sh") << "#!/bin/sh\nmkdir -p out && cat > out/result.txt && echo written\n";
        std::ofstream(ws / "poem.txt") << "one two three\nfour\n";
        std::ofstream(ws / "data.json") << R"({"version": 3, "nodes": [{"title": "Panel 1 prompt"}]})";
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;
        expect(agent.script_tools().size() == 3 && agent.tool_notices().empty(), "the examples and the writer load: " + std::to_string(agent.script_tools().size()) + " tools, " + std::to_string(agent.tool_notices().size()) + " notices");
        Recorder r;
        fake.tool_call = json{{"name", "word_count"}, {"arguments", {{"path", "poem.txt"}}}};
        fake.calls_left = 1;
        agent.submit("count", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "poem.txt: 2 lines, 4 words, 19 characters" && r.asked.empty(),
               "word_count (python3) runs with the arguments on stdin and its stdout is the result: " + (r.results.empty() ? "" : r.results[0]));
        std::string sys = fake.requests[0]["messages"][0]["content"];
        expect(offers_tool(fake.requests[0], "word_count") && offers_tool(fake.requests[0], "json_pick") && sys.find("word_count: Counts the lines") != std::string::npos &&
                   sys.find("a python script; may read **, may write nothing") != std::string::npos && sys.find("writer: writes out/result.txt (a sh script; may read nothing, may write out/*.txt)") != std::string::npos,
               "the model is offered the script tools beside the built-ins and the briefing says what each may read and write");
        const Message* tool_msg = nullptr;
        for (const auto& m : agent.messages()) if (m.role == "tool") tool_msg = &m;
        expect(tool_msg && !tool_msg->is_error && tool_msg->tool_name == "word_count", "the model sees a successful call of the tool");

        r.results.clear();
        fake.tool_call = json{{"name", "json_pick"}, {"arguments", {{"file", "data.json"}, {"filter", ".nodes[0].title"}}}};
        fake.calls_left = 1;
        agent.submit("pick", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "Panel 1 prompt", "json_pick (sh with jq) returns the picked value: " + (r.results.empty() ? "" : r.results[0]));

        r.results.clear();
        fake.tool_call = json{{"name", "json_pick"}, {"arguments", {{"file", "data.json"}}}};
        fake.calls_left = 1;
        agent.submit("pick badly", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "error: missing required argument filter", "arguments are checked against the manifest's schema before anything runs: " + (r.results.empty() ? "" : r.results[0]));

        r.results.clear();
        fake.tool_call = json{{"name", "json_pick"}, {"arguments", {{"file", "missing.json"}, {"filter", ".x"}}}};
        fake.calls_left = 1;
        agent.submit("pick missing", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0].rfind("exit code 1", 0) == 0 && r.results[0].find("no such file: missing.json") != std::string::npos,
               "a failing script gives the model its exit code and stderr: " + (r.results.empty() ? "" : r.results[0]));

        // The declared write is judged before the script starts: auto mode allows a write inside the workspace.
        r.results.clear();
        r.calls.clear();
        fake.tool_call = json{{"name", "writer"}, {"arguments", {{"text", "kept"}}}};
        fake.calls_left = 1;
        agent.submit("write", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0] == "written" && fs::exists(ws / "out" / "result.txt"), "a tool with a declared write gets a writable workspace in auto mode: " + (r.results.empty() ? "" : r.results[0]));
        // In manual mode the same declaration is asked about, with the tool named, and a No stops the script.
        agent.mode = Mode::Manual;
        r.results.clear();
        r.reply = {Approval::No, "not now"};
        fs::remove(ws / "out" / "result.txt");
        fake.tool_call = json{{"name", "writer"}, {"arguments", {{"text", "again"}}}};
        fake.calls_left = 1;
        agent.submit("write again", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].tool == "writer" && r.asked[0].summary.find("writes " + (ws / "out").string()) == 0, "the declared write is the approval prompt, naming the tool: " + (r.asked.empty() ? "" : r.asked[0].summary));
        expect(r.results.size() == 1 && r.results[0] == "DENIED by the user, who says: not now" && !fs::exists(ws / "out" / "result.txt"), "a No is the result and the script never ran");
        // word_count declares reads only, so in manual mode it runs without a prompt (reads inside the workspace are allowed).
        r.results.clear();
        r.asked.clear();
        fake.tool_call = json{{"name", "word_count"}, {"arguments", {{"path", "poem.txt"}}}};
        fake.calls_left = 1;
        agent.submit("count again", Origin::Local, r, no_cancel);
        expect(r.results.size() == 1 && r.results[0].find("4 words") != std::string::npos && r.asked.empty(), "a reads-only tool runs unasked in manual mode");
        const Message* last_tool = nullptr;
        for (const auto& m : agent.messages()) if (m.role == "tool") last_tool = &m;
        expect(last_tool && last_tool->tool_name == "word_count", "the result goes back under the tool's name");
        fs::remove_all(ws / ".maic");
        fs::remove_all(ws / "out");
        unsetenv("XDG_CONFIG_HOME");
    }

    section("a running command's output reaches the front end");
    {
        fs::path cfg = ws / "cfg";
        fs::create_directories(cfg);
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        fs::create_directories(ws / ".maic" / "tools" / "noisy");
        std::ofstream(ws / ".maic" / "tools" / "noisy" / "tool.json")
            << R"({"name": "noisy", "description": "says how it is going", "parameters": {"type": "object", "properties": {}}, "run": ["sh", "main.sh"]})";
        std::ofstream(ws / ".maic" / "tools" / "noisy" / "main.sh") << "#!/bin/sh\necho the result\necho working on it >&2\n";
        std::ofstream(ws / ".maic" / "tools" / "shells.lua") << "return {\n"
                                                                 "  name = 'shells', description = 'runs two commands',\n"
                                                                 "  parameters = { type = 'object', properties = {} },\n"
                                                                 "  run = function() return maic.shell('echo first') .. maic.shell('echo second') end,\n"
                                                                 "}\n";
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;
        agent.agents.push_back({"fast", Mode::Auto});
        auto all_before_result = [](const Recorder& r, const std::string& call) {
            return std::all_of(r.outputs.begin(), r.outputs.end(), [&](const Recorder::Output& o) { return o.results_before == 0 && o.call == call; });
        };

        Recorder r;
        r.reply = {Approval::Yes, ""};
        fake.tool_call = json{{"name", "run_shell"}, {"arguments", {{"command", "echo out; echo err >&2"}}}};
        fake.calls_left = 1;
        agent.submit("run it", Origin::Local, r, no_cancel);
        expect(!r.outputs.empty() && all_before_result(r, "call_1") && r.streamed(OutputStream::Stdout) == "out\nerr\n" && r.outputs[0].offset == 0,
               "run_shell's output, stderr interleaved as stdout, arrives under its call id before the result");
        expect(r.results.size() == 1 && r.results[0] == "exit code 0\nout\nerr", "the result is what it always was: " + (r.results.empty() ? "" : r.results[0]));

        Recorder s;
        fake.tool_call = json{{"name", "noisy"}, {"arguments", json::object()}};
        fake.calls_left = 1;
        agent.submit("noisy", Origin::Local, s, no_cancel);
        expect(all_before_result(s, "call_1") && s.streamed(OutputStream::Stderr) == "working on it\n" && s.streamed(OutputStream::Stdout).empty(),
               "a script tool streams its stderr only");
        expect(s.results.size() == 1 && s.results[0] == "the result", "its stdout is still the result");

        Recorder l;
        l.reply = {Approval::Yes, ""};
        fake.tool_call = json{{"name", "shells"}, {"arguments", json::object()}};
        fake.calls_left = 1;
        agent.submit("shells", Origin::Local, l, no_cancel);
        expect(all_before_result(l, "call_1") && l.streamed(OutputStream::Stdout) == "first\nsecond\n" && l.outputs.size() == 2 && l.outputs[1].offset == 6,
               "a Lua tool's maic.shell commands stream as one stream, the second continuing the first's offsets");

        Recorder c;
        c.reply = {Approval::Yes, ""};
        int parent_calls = 0, child_calls = 0;
        fake.tool_call_for = [&](const json& b) -> json {
            if (!from_child(b)) return ++parent_calls == 1 ? json{{"name", "task"}, {"arguments", {{"agent", "fast"}, {"prompt", "say hi"}}}} : json();
            return ++child_calls == 1 ? json{{"name", "run_shell"}, {"arguments", {{"command", "echo from the child"}}}} : json();
        };
        agent.submit("delegate", Origin::Local, c, no_cancel);
        fake.tool_call_for = nullptr;
        expect(!c.outputs.empty() && all_before_result(c, "fast:call_1") && c.streamed(OutputStream::Stdout) == "from the child\n",
               "a subagent's command streams to the parent's front end, labelled with the agent: " + (c.outputs.empty() ? "" : c.outputs[0].call));
        for (const auto& sub : list_sessions(ws)) {
            if (sub.kind == "sub") fs::remove(sub.path);
        }
        fs::remove_all(ws / ".maic");
        unsetenv("XDG_CONFIG_HOME");
    }

    section("a command's whole output is kept beside the session, the model's result unchanged");
    {
        FakeServer fake;
        SessionLog log("agent-test");
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;
        agent.set_log(&log);
        std::string full;
        for (int i = 1; full.size() < 2 * 1024 * 1024; ++i) full += std::to_string(i) + "\n";
        full.resize(2 * 1024 * 1024);
        Recorder r;
        r.reply = {Approval::Yes, ""};
        fake.tool_call = json{{"name", "run_shell"}, {"arguments", {{"command", "seq 1 400000 | head -c 2097152"}}}};
        fake.calls_left = 1;
        agent.submit("run it", Origin::Local, r, no_cancel);
        std::string model = full.substr(0, 24 * 1024) + "\n... [" + std::to_string(full.size() - 32 * 1024) + " bytes omitted] ...\n" + full.substr(full.size() - 8 * 1024);
        while (!model.empty() && model.back() == '\n') model.pop_back();
        expect(r.results.size() == 1 && r.results[0] == "exit code 0\n" + model, "the model's result is the capped one it always was: " + (r.results.empty() ? "" : r.results[0].substr(0, 60) + " ... " + std::to_string(r.results[0].size()) + " bytes"));
        json rec;
        std::ifstream in(log.path());
        for (std::string line; std::getline(in, line);) {
            json j = json::parse(line, nullptr, false);
            if (j.is_object() && j.value("type", "") == "tool") rec = j;
        }
        fs::path file = log.path().parent_path() / rec["full_output"].value("path", "");
        std::ifstream kept_in(file, std::ios::binary);
        std::string kept((std::istreambuf_iterator<char>(kept_in)), std::istreambuf_iterator<char>());
        expect(rec["full_output"]["path"] == log.path().stem().string() + ".d/call_1.out" && rec["full_output"]["bytes"] == full.size() &&
                   rec["full_output"]["delivered_to_model"] == false && kept == full,
               "the tool record names the kept file, beside the session, and it holds all 2 MiB: " + rec.dump().substr(0, 300));
        expect(r.kept.size() == 1 && r.kept[0] == file, "the front end is told where, before the result");
        std::string read = render_text(load_session(log.path()), 0, 0, true);
        expect(read.find("[full output, display only: the model saw the capped result] 2097152 bytes: maic sessions output " + log.path().stem().string() + " call_1") != std::string::npos,
               "maic sessions read labels it display only");
        bool in_history = false;
        for (const auto& m : agent.messages()) in_history = in_history || m.content.find("full output") != std::string::npos;
        expect(!in_history, "and nothing of it reaches the model");

        agent.full_output = false;
        Recorder off;
        off.reply = {Approval::Yes, ""};
        fake.calls_left = 1;
        agent.submit("again", Origin::Local, off, no_cancel);
        expect(off.kept.empty() && !fs::exists(file.parent_path() / "call_1-2.out"), "full_output = false keeps nothing");
        fs::remove_all(file.parent_path());
        fs::remove(log.path());
    }

    section("operator instructions set mid-conversation");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        agent.submit("one", Origin::Local, r, no_cancel);
        agent.set_system_prefix("Always start with pelican.");
        agent.submit("two", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests.back()["messages"];
        bool note = false;
        for (const auto& m : msgs) note = note || (m["role"] == "system" && m["content"].get<std::string>().find("# Operator instructions (take precedence") == 0 && m["content"].get<std::string>().find("pelican") != std::string::npos);
        expect(note && msgs.back()["content"].get<std::string>().find("(Operator instructions in force") != std::string::npos, "a mid-conversation :system appends a system note and the per-turn note follows");
        agent.set_system_prefix("");
        agent.submit("three", Origin::Local, r, no_cancel);
        expect(fake.requests.back()["messages"].back()["content"] == "three" && fake.requests.back()["messages"][fake.requests.back()["messages"].size() - 2]["content"].get<std::string>().find("withdrawn") != std::string::npos,
               "withdrawing it appends a note and stops the per-turn note");
    }

    section("rules");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.rules = {"answer in French", "be brief"};
        Recorder r;
        agent.submit("hi", Origin::Local, r, no_cancel);
        std::string sys = fake.requests.back()["messages"][0]["content"];
        std::string user = fake.requests.back()["messages"].back()["content"];
        expect(sys.rfind("# Operator instructions", 0) == 0 && sys.find("- answer in French\n- be brief") != std::string::npos && sys.find("# Operator instructions, again") != std::string::npos,
               "rules lead and close the system prompt as a list");
        expect(user.find("(Operator instructions in force, they take precedence: - answer in French\n- be brief)") != std::string::npos, "and ride in the per-turn note");
        agent.set_rules({"be brief"});
        agent.submit("again", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests.back()["messages"];
        expect(msgs[msgs.size() - 2]["role"] == "system" && msgs[msgs.size() - 2]["content"].get<std::string>().find("- be brief") != std::string::npos &&
                   msgs[msgs.size() - 2]["content"].get<std::string>().find("French") == std::string::npos,
               "changing the rules mid-conversation appends a system note with the current set");
    }

    section("the harness protects itself");
    {
        FakeServer fake;
        auto attempt = [&](bool smart, const std::string& path) {
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", path}, {"content", "return { harness = 'dumb' }"}}}};
            fake.calls_left = 1;
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = smart;
            Recorder r;
            r.reply = {Approval::Yes, ""};  // an ordinary ASK (the fake reviewer cannot say ALLOW) is approved; the guard never asks, it trips
            agent.submit("change the settings", Origin::Local, r, no_cancel);
            return r;
        };
        std::string lock = std::getenv("MAIC_TRIPWIRE_FILE");
        auto r = attempt(true, ".maic/settings.lua");
        bool tripped = false;
        for (const auto& n : r.notices) tripped = tripped || n.rfind("HARNESS TRIPPED", 0) == 0;
        expect(tripped && fs::exists(lock) && !fs::exists(ws / ".maic" / "settings.lua"), "under the smart harness, writing MAIC's own settings trips the lock and writes nothing");
        fs::remove(lock);
        auto r2 = attempt(false, ".maic/settings.lua");
        expect(r2.asked.size() == 1 && r2.asked[0].reason.find("harness's own files") != std::string::npos && !fs::exists(lock), "the dumb harness asks instead");
        auto r3 = attempt(true, "notes.txt");
        expect(!fs::exists(lock) && fs::exists(ws / "notes.txt"), "an ordinary write is untouched by the guard");
        fs::remove(ws / "notes.txt");

        // Trust is the user's alone: the agent can't grant it, change a tier, or write the record, under either harness.
        auto call = [&](bool smart, const json& c) {
            fake.tool_call = c;
            fake.calls_left = 1;
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = smart;
            Recorder r;
            r.reply = {Approval::Yes, ""};
            agent.submit("trust it", Origin::Local, r, no_cancel);
            return r;
        };
        auto t1 = call(true, json{{"name", "run_shell"}, {"arguments", {{"command", "maic trust ."}}}});
        tripped = false;
        for (const auto& n : t1.notices) tripped = tripped || n.rfind("HARNESS TRIPPED", 0) == 0;
        expect(tripped && fs::exists(lock), "under the smart harness, the agent running `maic trust` trips the lock");
        fs::remove(lock);
        auto t2 = call(false, json{{"name", "run_shell"}, {"arguments", {{"command", "maic trust . --level relaxed"}}}});
        expect(t2.asked.empty() && !t2.results.empty() && t2.results[0].find("trust is the user's alone") != std::string::npos && !fs::exists(lock),
               "the dumb harness refuses it without asking, so no approval can let it through");
        auto t3 = call(false, json{{"name", "write_file"}, {"arguments", {{"path", (state_dir() / "trust.json").string()}, {"content", "{}"}}}});
        expect(t3.asked.empty() && !fs::exists(state_dir() / "trust.json"), "nor can it write trust.json");
    }

    section("forbidden terms halt a call under any harness");
    {
        FakeServer fake;
        auto attempt = [&](bool smart, const json& call) {
            fake.tool_call = call;
            fake.calls_left = 1;
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = smart;
            agent.set_forbid(Settings{}.forbid);
            Recorder r;
            r.reply = {Approval::Yes, ""};
            agent.submit("look", Origin::Local, r, no_cancel);
            return r;
        };
        auto r1 = attempt(false, json{{"name", "search_files"}, {"arguments", {{"pattern", "threesOme"}, {"path", "."}}}});
        expect(!r1.results.empty() && r1.results[0].rfind("DENIED: the call contains the forbidden term", 0) == 0 && r1.asked.empty(), "a search for the term is halted under the dumb harness without asking");
        bool noticed = false;
        for (const auto& n : r1.notices) noticed = noticed || n.rfind("HALTED", 0) == 0;
        expect(noticed, "and the user sees it");
        auto r2 = attempt(true, json{{"name", "run_shell"}, {"arguments", {{"command", "grep -ri threesomes docs/"}}}});
        expect(!r2.results.empty() && r2.results[0].rfind("DENIED: the call contains the forbidden term", 0) == 0, "a command with the plural is halted under the smart harness before the reviewer");
        auto r3 = attempt(false, json{{"name", "glob"}, {"arguments", {{"pattern", "**/*THREESOME*"}}}});
        expect(!r3.results.empty() && r3.results[0].rfind("DENIED", 0) == 0, "a glob pattern with it is halted");
        auto r4 = attempt(false, json{{"name", "search_files"}, {"arguments", {{"pattern", "pelican"}, {"path", "."}}}});
        expect(!r4.results.empty() && r4.results[0].rfind("DENIED", 0) != 0, "an ordinary search runs");
    }

    section("images on a user turn");
    {
        FakeServer fake;
        fs::path png = ws / "pic.png";
        std::ofstream(png, std::ios::binary) << std::string("\x89PNG\r\n\x1a\n", 8) << "rest";
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.attach_image(png);
        expect(agent.pending_images() == std::vector<std::string>{"pic.png"}, "an attached image waits for the next turn");
        Recorder r;
        agent.submit("what is this", Origin::Local, r, no_cancel);
        auto last = fake.requests.back()["messages"].back();
        expect(last["role"] == "user" && last["content"].is_array() && last["content"].size() == 2 && last["content"][1]["type"] == "image_url" &&
                   last["content"][1]["image_url"]["url"].get<std::string>().rfind("data:image/png;base64,iVBORw0KGg", 0) == 0,
               "the picture rides as an image_url part beside the text");
        expect(agent.pending_images().empty() && agent.messages().back().role == "assistant" && agent.messages()[agent.messages().size() - 2].images.size() == 1,
               "the picture is on the stored user message, and the queue is empty");
        bool threw = false;
        try {
            agent.attach_image(ws / "notes.txt");
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("not an image") != std::string::npos;
        }
        expect(threw, "a non-image file is refused");
        agent.submit("second", Origin::Local, r, no_cancel);
        agent.submit("third", Origin::Local, r, no_cancel);
        std::string rep = agent.compact(Agent::Compaction::Prune, no_cancel);
        bool gone = false;
        for (const auto& m : agent.messages()) gone = gone || (m.role == "user" && m.content.find("[image pic.png removed") != std::string::npos && m.images.empty());
        expect(gone && rep.find("1 old image") != std::string::npos, "pruning removes a picture older than the last two turns and says so: " + rep);
        expect(base64_encode("Man") == "TWFu" && base64_encode("Ma") == "TWE=" && base64_encode("M") == "TQ==", "base64 pads correctly");
    }

    section("prefill");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.prefill = "hellooooo ";
        Recorder r;
        agent.submit("hi", Origin::Local, r, no_cancel);
        auto last = fake.requests.back()["messages"].back();
        expect(last["role"] == "assistant" && last["content"] == "hellooooo ", "the prefill is sent as an open assistant turn");
        expect(r.text.rfind("hellooooo echo: hi", 0) == 0, "it is shown first, then the model's continuation: " + r.text);
        expect(agent.messages().back().role == "assistant" && agent.messages().back().content.rfind("hellooooo ", 0) == 0, "and stored as the start of the reply");
        expect(fake.requests.back()["messages"][fake.requests.back()["messages"].size() - 2]["role"] == "user", "the history itself gains no assistant stub");
        // llama-server style: the server echoes the prefill at the head of its output.
        fake.reply = [](const json&) { return std::string("hellooooo the rest"); };
        Recorder r2;
        agent.submit("again", Origin::Local, r2, no_cancel);
        expect(r2.text == "hellooooo the rest" && agent.messages().back().content == "hellooooo the rest", "an echoed prefill is not doubled: " + r2.text);
        fake.reply = [](const json&) { return std::string("hel"); };
        Recorder r3;
        agent.submit("short", Origin::Local, r3, no_cancel);
        expect(r3.text == "hellooooo hel", "a reply shorter than the prefill still shows");
        fake.reply = nullptr;
    }

    section("operator prompt and instruction switch");
    {
        FakeServer fake;
        std::ofstream(ws / "MAIC.md") << "project rule: always say pelican";
        fs::create_directories(ws / "deep");
        std::ofstream(ws / "deep" / "AGENTS.md") << "deep rule";
        std::ofstream(ws / "deep" / "f.txt") << "x";
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.system_prefix = "You are a terse reviewer.";
        agent.reload_instructions();
        Recorder r;
        agent.submit("hi", Origin::Local, r, no_cancel);
        std::string sys = fake.requests[0]["messages"][0]["content"];
        expect(sys.rfind("# Operator instructions", 0) == 0 && sys.find("terse reviewer") < sys.find("inside MAIC"), "the operator text leads the system prompt");
        expect(sys.rfind("terse reviewer") > sys.find("pelican") && sys.find("# Operator instructions, again") != std::string::npos, "and closes it, after the instruction files");
        std::string last_user = fake.requests[0]["messages"].back()["content"];
        expect(last_user.rfind("hi\n\n(Operator instructions in force", 0) == 0 && last_user.find("terse reviewer") != std::string::npos, "and rides at the end of the user's turn as the model sees it");
        agent.operator_note_in_turn = false;
        Recorder r2;
        agent.submit("again", Origin::Local, r2, no_cancel);
        expect(fake.requests.back()["messages"].back()["content"] == "again", "operator_note off keeps the user's turn bare (Anthropic's default)");
        expect(sys.find("project rule: always say pelican") != std::string::npos, "instruction files still load alongside it");

        Agent bare(ws, "test");
        bare.providers = {fake.provider()};
        bare.mode = Mode::Auto;
        bare.load_instruction_files = false;
        bare.reload_instructions();
        Recorder rb;
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "deep/f.txt"}}}};
        fake.calls_left = 1;
        bare.submit("read", Origin::Local, rb, no_cancel);
        std::string sys2 = fake.requests.back()["messages"][0]["content"];
        expect(sys2.find("pelican") == std::string::npos && sys2.find("Operator") == std::string::npos, "with the switch off no instruction file is loaded");
        expect(!rb.results.empty() && rb.results[0].find("deep rule") == std::string::npos, "and none is attached on read");
        fs::remove(ws / "MAIC.md");
    }

    section("instruction files in the prompt: most general first, the workspace last");
    {
        FakeServer fake;
        fs::path cfg = ws / "prec-cfg", sys = ws / "prec-sys";
        fs::create_directories(cfg / "maic");
        fs::create_directories(sys);
        fs::create_directories(ws / "prec" / "sub");
        std::ofstream(cfg / "maic" / "MAIC.md") << "user rule: herons";
        std::ofstream(sys / "MAIC.md") << "system rule: egrets";
        std::ofstream(ws / "CLAUDE.md") << "claude rule: storks";
        std::ofstream(ws / "MAIC.md") << "maic rule: cranes";
        std::ofstream(ws / "prec" / "sub" / "AGENTS.md") << "sub rule: ibises";
        std::ofstream(ws / "prec" / "sub" / "f.txt") << "x";
        setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
        setenv("MAIC_TESTING", "1", 1);
        setenv("MAIC_SYSTEM_CONFIG_DIR", sys.c_str(), 1);
        trust_for_session(ws);
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;
        Recorder r;
        agent.submit("hi", Origin::Local, r, no_cancel);
        std::string sys_prompt = fake.requests[0]["messages"][0]["content"];
        size_t egrets = sys_prompt.find("egrets"), herons = sys_prompt.find("herons"), storks = sys_prompt.find("storks"), cranes = sys_prompt.find("cranes");
        expect(egrets != std::string::npos && egrets < herons && herons < storks && storks < cranes && cranes != std::string::npos,
               "system-wide, then yours, then the workspace's CLAUDE.md, then its MAIC.md");
        expect(sys_prompt.find("where two conflict, the later one takes precedence") != std::string::npos, "a header says the later ones take precedence");
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "prec/sub/f.txt"}}}};
        fake.calls_left = 1;
        agent.submit("read", Origin::Local, r, no_cancel);
        expect(!r.results.empty() && r.results.back().find("[MAIC system note: standing instructions from " + (ws / "prec" / "sub" / "AGENTS.md").string()) != std::string::npos &&
                   r.results.back().find("ibises") != std::string::npos,
               "a read in a subdirectory attaches its instruction file as a marked system note");
        fake.calls_left = 1;
        agent.submit("read again", Origin::Local, r, no_cancel);
        expect(r.results.back().find("ibises") == std::string::npos, "once per session");
        agent.clear();
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "./prec/sub/f.txt"}}}};  // not the same call a third time
        fake.calls_left = 1;
        agent.submit("read after clear", Origin::Local, r, no_cancel);
        expect(r.results.back().find("ibises") != std::string::npos, "and again in a cleared conversation, which no longer has it");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("MAIC_TESTING");
        unsetenv("MAIC_SYSTEM_CONFIG_DIR");
        fs::remove_all(cfg);
        fs::remove_all(sys);
        fs::remove_all(ws / "prec");
        fs::remove(ws / "CLAUDE.md");
        fs::remove(ws / "MAIC.md");
    }

    section("a file your own instructions import is self-protected");
    {
        FakeServer fake;
        fs::path target = ws / "approved-style.md";
        std::ofstream(target) << "style\n";
        approve_import(ws / "cfg-MAIC.md", target, Origin::Local);
        fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", "approved-style.md"}, {"content", "changed"}}}};
        Agent dumb(ws, "test");
        dumb.providers = {fake.provider()};
        dumb.mode = Mode::Auto;
        dumb.review_with_model = false;
        Recorder rd;
        fake.calls_left = 1;
        dumb.submit("change it", Origin::Local, rd, no_cancel);
        expect(rd.asked.empty() && !rd.results.empty() && rd.results[0].find("an approved import") != std::string::npos, "the dumb harness refuses the agent's write to it");
        Agent smart(ws, "test");
        smart.providers = {fake.provider()};
        smart.mode = Mode::Auto;
        Recorder rs;
        fake.calls_left = 1;
        smart.submit("change it", Origin::Local, rs, no_cancel);
        expect(rs.asked.size() == 1, "the smart harness asks you, even in auto mode");
        std::ifstream in(target);
        std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(kept == "style\n", "and nothing was written");
        trust_imports_command({"--remove", target.string()});
        fs::remove(target);
    }

    section("a request over the context window");
    {
        FakeServer fake;
        std::string big(30000, 'x');
        // History far past a 16k window with no usage report yet: the byte estimate compacts before sending.
        Agent agent(ws, "test");
        Provider p = fake.provider();
        p.options["context_window"] = 16384;
        agent.providers = {p};
        std::vector<Message> hist = {{"system", "sys"}};
        for (int i = 1; i <= 6; ++i) {
            hist.push_back({"user", "t" + std::to_string(i)});
            hist.push_back({"assistant", "", {{"k" + std::to_string(i), "read_file", {{"path", "f"}}}}});
            hist.push_back({"tool", big, {}, "read_file", "k" + std::to_string(i)});
            hist.push_back({"assistant", "r" + std::to_string(i)});
        }
        agent.restore(hist);
        Recorder r;
        agent.submit("go", Origin::Local, r, no_cancel);
        bool compacted = false;
        for (const auto& n : r.notices) compacted = compacted || (n.rfind("context at", 0) == 0 && n.find("pruned") != std::string::npos);
        expect(compacted && r.text.find("echo") != std::string::npos, "a history that outgrew the window in tool results is compacted before the call, without a usage report");
        expect(agent.estimated_tokens() < 16384, "and fits afterwards (" + std::to_string(agent.estimated_tokens()) + " estimated tokens)");

        // The server refuses anyway (llama.cpp's wording): compact and retry, and the turn still completes.
        Agent b(ws, "test");
        b.providers = {p};
        b.restore(hist);
        b.compaction.at = 0;  // no proactive compaction, so the 400 path is what saves it
        SessionLog blog("agent-test");
        b.set_log(&blog);
        fake.fail_left = 1;
        fake.fail_body = R"({"error":{"code":400,"message":"request (27847 tokens) exceeds the available context size (16384 tokens), try increasing it","type":"invalid_request_error"}})";
        Recorder rb;
        b.submit("go", Origin::Local, rb, no_cancel);
        bool retried = false;
        for (const auto& n : rb.notices) retried = retried || n.rfind("the request was over the context window", 0) == 0;
        expect(retried && rb.text.find("echo") != std::string::npos && fake.requests.size() >= 2, "a 400 for a too-long request compacts and retries, and the turn completes");
        fake.fail_left = 3;
        Recorder rc;
        bool threw = false;
        try { b.submit("again", Origin::Local, rc, no_cancel); } catch (const std::exception& e) { threw = std::string(e.what()).find("exceeds") != std::string::npos; }
        expect(threw, "after two compact-and-retry rounds the error is reported");
        // llama.cpp's error has an integer code and no param: the adapter rules rewrote it, and said so.
        auto counted = b.usage().normalized;
        expect(counted["error_code_string"] >= 1 && counted["error_code_string"] == counted["error_param_null"],
               "the usage report counts the adapter rules applied to llama.cpp's error bodies");
        SessionStats st = session_stats(blog.path());
        expect(st.normalized["fake error_code_string"] == static_cast<size_t>(counted["error_code_string"]) && st.normalized["fake error_param_null"] == static_cast<size_t>(counted["error_param_null"]),
               "each application is a `normalized` record naming the rule and the provider, summed by session_stats");
        json record;
        std::ifstream recs(blog.path());
        for (std::string l; std::getline(recs, l) && record.is_null();) {
            auto j = json::parse(l, nullptr, false);
            if (j.is_object() && j.value("type", "") == "normalized") record = j;
        }
        expect(record.value("provider", "") == "fake" && record.value("upstream", "") == "fake", "the record names the upstream too (the provider's name when it sets none)");
    }

    section("string and token bans");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.bans.strings = {"pelican"};
        agent.bans.retries = 2;
        Recorder r;
        agent.submit("the pelican flies", Origin::Local, r, no_cancel);  // the fake echoes it back
        expect(r.text.find("pelican") == std::string::npos, "the banned phrase never reaches the screen: [" + r.text + "]");
        expect(fake.requests.size() == 3, "cut, re-asked twice, then the replacement pass (3 requests)");
        bool nudged = false;
        for (const auto& m : fake.requests[1]["messages"]) {
            if (m["role"] == "system" && m["content"].get<std::string>().find("banned phrase \"pelican\"") != std::string::npos) nudged = true;
        }
        expect(nudged, "the model is told which phrase was banned");
        expect(r.text.find("[banned]") != std::string::npos, "after the retries the phrase is replaced");
        bool notice = false;
        for (const auto& n : r.notices) notice = notice || n.find("cut: banned phrase") == 0;
        expect(notice, "the user sees why the reply was cut");
        expect(agent.messages().back().content.find("pelican") == std::string::npos, "the stored reply has no banned phrase either");

        Agent b(ws, "test");
        b.providers = {fake.provider()};
        b.bans.tokens = {nlohmann::json(1234)};
        Recorder rb;
        b.submit("hi", Origin::Local, rb, no_cancel);
        expect(fake.requests.back()["logit_bias"]["1234"] == -100 && !has_notice(rb, "token ban"), "numeric token bans reach an OpenAI-compatible provider as logit_bias, without a notice");
    }

    section("always for a program covers one simple command");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Manual;
        auto run = [&](const std::string& command, ApprovalAnswer reply) {
            fake.tool_call = json{{"name", "run_shell"}, {"arguments", {{"command", command}}}};
            fake.calls_left = 1;
            Recorder r;
            r.reply = reply;
            agent.submit("run it", Origin::Local, r, no_cancel);
            return r;
        };
        expect(run("echo one", {Approval::Always, ""}).asked.size() == 1, "the first echo is asked, and answered always");
        expect(run("echo two", {Approval::No, ""}).asked.empty(), "a second plain echo is not asked again");
        for (const char* cmd : {"echo x && touch chained.txt", "echo x; touch chained.txt", "echo x | sh", "echo x > chained.txt", "echo x $(touch chained.txt)"}) {
            Recorder r = run(cmd, {Approval::No, ""});
            expect(r.asked.size() == 1 && !fs::exists(ws / "chained.txt"), std::string("but a chained one is asked, whatever came before: ") + cmd);
        }
    }

    section("a session's always never answers for a remote request");
    {
        FakeServer fake;
        auto agent_in = [&](Agent& agent) {
            agent.providers = {fake.provider()};
            agent.mode = Mode::Manual;
        };
        auto run = [&](Agent& agent, const json& call, Origin origin, ApprovalAnswer reply) {
            fake.tool_call = call;
            fake.calls_left = 1;
            Recorder r;
            r.reply = reply;
            agent.submit("run it", origin, r, no_cancel);
            return r;
        };
        // A different echo each time (the session's "always" covers the program), so the repeat guard stays out of it.
        int n = 0;
        auto echo_call = [&] { return json{{"name", "run_shell"}, {"arguments", {{"command", "echo remote-always " + std::to_string(++n)}}}}; };
        json write = {{"name", "write_file"}, {"arguments", {{"path", "remote-always.txt"}, {"content", "x"}}}};
        {
            Agent agent(ws, "test");
            agent_in(agent);
            expect(run(agent, echo_call(), Origin::Local, {Approval::Always, ""}).asked.size() == 1, "local: echo is asked and answered always");
            expect(run(agent, echo_call(), Origin::Local, {Approval::No, ""}).asked.empty(), "local: the next echo runs unasked, as before");
            Recorder r = run(agent, echo_call(), Origin::Remote, {Approval::No, ""});
            expect(r.asked.size() == 1 && r.asked[0].origin == Origin::Remote, "but the same echo from a remote origin is still asked");
            expect(run(agent, write, Origin::Local, {Approval::Always, ""}).asked.size() == 1, "local: a write is asked and answered always");
            expect(run(agent, write, Origin::Remote, {Approval::No, ""}).asked.size() == 1, "the same write from a remote origin is still asked");
        }
        {
            Agent agent(ws, "test");
            agent_in(agent);
            Recorder r = run(agent, echo_call(), Origin::Remote, {Approval::Always, ""});
            expect(r.asked.size() == 1 && r.asked[0].always_covers.find("this call only") != std::string::npos, "remote: the prompt says always covers this call only");
            expect(has_notice(r, "counts for this call only") && has_result(r, "remote-always"), "remote: always runs it once and says so");
            expect(run(agent, echo_call(), Origin::Remote, {Approval::No, ""}).asked.size() == 1, "remote: a remote always did not persist, the next remote echo is asked");
            expect(run(agent, echo_call(), Origin::Local, {Approval::No, ""}).asked.size() == 1, "and a local echo is asked too: nothing was remembered");
        }
        fs::remove(ws / "remote-always.txt");
    }

    section("a remote message delivered into a local turn makes the rest of it remote");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;  // the rules alone, so only the origin decides what is asked
        int requests = 0;
        fake.tool_call_for = [&](const json&) -> json {
            ++requests;
            // The phone's message arrives while the first call is out; it is delivered at the next step.
            if (requests == 1) {
                agent.post_message("also check the tests", Origin::Remote);
                return {{"name", "run_shell"}, {"arguments", {{"command", "echo before"}}}};
            }
            if (requests == 2) return {{"name", "run_shell"}, {"arguments", {{"command", "echo after"}}}};
            return nullptr;
        };
        Recorder r;
        r.reply = {Approval::Yes, ""};
        agent.submit("run the checks", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].origin == Origin::Remote && r.asked[0].summary.find("echo after") != std::string::npos,
               "auto mode runs the local echo unasked, then asks for the one after the remote message, as remote");
        expect(agent.turn_origin() == Origin::Remote, "the turn's origin rose to remote and stays there");
        agent.submit("next", Origin::Local, r, no_cancel);
        expect(agent.turn_origin() == Origin::Local, "the next turn starts at its own origin");
    }

    section("the reviewer (smart harness) and the dumb harness");
    {
        FakeServer fake;
        auto is_review = [](const json& b) { return b["messages"][0]["role"] == "system" && b["messages"][0]["content"].get<std::string>().rfind("You review one action", 0) == 0; };
        auto reviews = [&] {
            int n = 0;
            for (const auto& r : fake.requests) n += is_review(r);
            return n;
        };
        auto run = [&](const std::string& verdict, bool smart, const std::string& path) {
            fake.reply = [&, verdict](const json& b) { return is_review(b) ? verdict : std::string(); };
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", path}, {"content", "x"}}}};
            fake.calls_left = 1;
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.mode = Mode::Auto;
            agent.review_with_model = smart;
            Recorder r;
            r.reply = {Approval::Yes, ""};
            agent.submit("please write the file", Origin::Local, r, no_cancel);
            return r;
        };
        auto r1 = run("DENY: nothing in the conversation asked for that file", true, "r1.txt");
        expect(!fs::exists(ws / "r1.txt") && r1.asked.empty(), "a DENY from the reviewer stops the write without asking");
        expect(!r1.results.empty() && r1.results[0].find("DENIED: reviewer: nothing in the conversation") == 0, "the model reads the reviewer's reason: " + (r1.results.empty() ? "" : r1.results[0]));
        bool noticed = false;
        for (const auto& n : r1.notices) noticed = noticed || n.rfind("reviewer refused", 0) == 0;
        expect(noticed, "the user sees the refusal");
        auto r2 = run("ALLOW: the user asked for exactly this", true, "r2.txt");
        expect(fs::exists(ws / "r2.txt") && r2.asked.empty(), "an ALLOW lets it run silently");
        auto r3 = run("ASK: unusual path", true, "r3.txt");
        expect(r3.asked.size() == 1 && r3.asked[0].reason == "reviewer: unusual path" && fs::exists(ws / "r3.txt"), "an ASK becomes an approval prompt with the reviewer's reason");
        auto r4 = run("I am not sure what to say here", true, "r4.txt");
        expect(r4.asked.size() == 1 && r4.asked[0].reason.find("no clear verdict") != std::string::npos, "an unparseable answer fails closed to ASK");
        auto r5 = run("**ALLOW**: fine\nmore words", true, "r5.txt");
        expect(fs::exists(ws / "r5.txt") && r5.asked.empty(), "markdown bold and trailing lines around the verdict are tolerated");
        int before = reviews();
        auto r6 = run("DENY: would have refused", false, "r6.txt");
        expect(fs::exists(ws / "r6.txt") && reviews() == before && r6.asked.empty(), "the dumb harness never calls the reviewer and the rules alone decide");
        fake.reply = [&](const json& b) { return is_review(b) ? std::string("DENY: no") : std::string(); };
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "r2.txt"}}}};
        fake.calls_left = 1;
        Agent rd(ws, "test");
        rd.providers = {fake.provider()};
        rd.mode = Mode::Auto;
        Recorder rr;
        before = reviews();
        rd.submit("read it", Origin::Local, rr, no_cancel);
        expect(reviews() == before && !rr.results.empty() && rr.results[0].find("1\tx") != std::string::npos, "reads skip the reviewer");
        json review_req;
        for (const auto& q : fake.requests) if (is_review(q)) review_req = q;
        std::string body = review_req["messages"][1]["content"];
        expect(!review_req.contains("tools") && body.find("User: please write the file") != std::string::npos && body.find("Write to:") != std::string::npos && body.find("Mode: auto") != std::string::npos,
               "the reviewer sees the user's words, the mode and the action, and has no tools");
        fake.reply = nullptr;

        // The side server reviews when it is up, so the main server keeps its model: with no reviewer_model and
        // the model on llamacpp, a llamacpp-2 that answers gets the review with the same model name; a closed
        // port falls back to the main server.
        FakeServer side;
        auto side_run = [&](const std::string& side_url, const std::string& path) {
            fake.reply = [&](const json& b) { return is_review(b) ? std::string("ALLOW: fine") : std::string(); };
            side.reply = fake.reply;
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", path}, {"content", "x"}}}};
            fake.calls_left = 1;
            Agent agent(ws, "Qwen3.5-4B-Q4_K_M");
            Provider main_p = fake.provider();
            main_p.name = "llamacpp";
            agent.providers = {main_p, {"llamacpp-2", "openai", side_url, "", "", {{"context_window", 8192}}}};
            agent.mode = Mode::Auto;
            Recorder r;
            r.reply = {Approval::Yes, ""};
            agent.submit("please write the file", Origin::Local, r, no_cancel);
            return r;
        };
        int main_reviews = reviews();
        side_run("http://127.0.0.1:9/v1", "side1.txt");
        expect(fs::exists(ws / "side1.txt") && reviews() == main_reviews + 1 && side.requests.empty(), "with the side server down, the main server reviews as before");
        main_reviews = reviews();
        side_run(side.provider().base_url, "side2.txt");
        int side_reviews = 0;
        std::string side_model;
        for (const auto& q : side.requests) {
            if (is_review(q)) ++side_reviews, side_model = q.value("model", "");
        }
        expect(fs::exists(ws / "side2.txt") && side_reviews == 1 && reviews() == main_reviews && side_model == "Qwen3.5-4B-Q4_K_M",
               "with llamacpp-2 answering, the review goes there with the same model name and the main server is left alone");
        fake.reply = nullptr;
    }

    section("the reviewer's model, its limits and its spend");
    {
        // fa serves the session's model (Opus), fb the cheaper ones the reviewer may use.
        FakeServer fa, fb;
        Provider pa = fa.provider(), pb = fb.provider();
        pa.name = "fa";
        pb.name = "fb";
        std::vector<ModelPreset> presets = default_presets();
        for (auto& p : presets) {
            if (p.name == "fable-5.1") p.model = "fa/fable";
            if (p.name == "opus-5.5") p.model = "fa/opus";
            if (p.name == "sonnet-5") p.model = "fb/sonnet";
            if (p.name == "haiku-4.5") p.model = "fb/haiku";
        }
        auto is_review = [](const json& b) { return b["messages"][0]["content"].get<std::string>().rfind("You review one action", 0) == 0; };
        auto review_models = [&](const FakeServer& f) {
            std::vector<std::string> out;
            for (const auto& q : f.requests) if (is_review(q)) out.push_back(q.value("model", ""));
            return out;
        };
        int n = 0;
        // One agent per case; each turn writes one file, so each turn wants one review.
        auto make = [&](const std::string& small, const std::string& pin = "") {
            auto a = std::make_unique<Agent>(ws, "fa/opus");
            a->providers = {pa, pb};
            a->presets = presets;
            a->mode = Mode::Auto;
            a->small_model = small;
            a->reviewer_model = pin;
            return a;
        };
        auto write_turn = [&](Agent& a, Recorder& r) {
            std::string path = "rv" + std::to_string(++n) + ".txt";
            fa.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", path}, {"content", "x"}}}};
            fa.calls_left = 1;
            a.submit("please write " + path, Origin::Local, r, no_cancel);
            return fs::exists(ws / path);
        };
        auto allow = [&](const json& b) { return is_review(b) ? std::string("ALLOW: fine") : std::string(); };
        fa.reply = fb.reply = allow;
        auto reset = [&] {
            fa.requests.clear();
            fb.requests.clear();
            fb.fail_when = nullptr;
        };

        {
            reset();
            SessionLog log("agent-test");
            auto a = make("haiku-4.5");
            a->set_log(&log);
            Recorder r;
            expect(write_turn(*a, r) && review_models(fb) == std::vector<std::string>{"haiku"} && review_models(fa).empty(), "small_model reviews: Opus's write is reviewed on Haiku");
            json review;
            std::ifstream in(log.path());
            for (std::string l; std::getline(in, l);) {
                json j = json::parse(l, nullptr, false);
                if (j.is_object() && j.value("type", "") == "tool" && j.contains("review")) review = j["review"];
            }
            expect(review.value("model", "") == "fb/haiku" && review.value("model_reason", "") == "small_model" && review.value("verdict", "") == "allow",
                   "the transcript's review records the model and why: " + review.dump());
            fs::remove(log.path());
        }
        {
            reset();
            auto a = make("haiku-4.5", "fb/sonnet");
            Recorder r;
            expect(write_turn(*a, r) && review_models(fb) == std::vector<std::string>{"sonnet"}, "a reviewer_model pin wins over small_model");
            expect(a->reviewer().pick.reason == "reviewer_model", "and :harness says so");
        }
        // Sonnet runs out: one notice, this action is asked, the next review is on Haiku.
        {
            reset();
            fb.fail_when = [&](const json& b) { return is_review(b) && b.value("model", "") == "sonnet"; };
            fb.fail_status = 429;
            fb.fail_body = R"({"type":"error","error":{"type":"rate_limit_error","message":"You've reached your Sonnet limit. Run /usage-credits to continue or switch models with /model."}})";
            auto a = make("sonnet-5");
            Recorder r;
            r.reply = {Approval::Yes, ""};
            bool wrote = write_turn(*a, r);
            int notices = 0;
            for (const auto& t : r.notices) notices += t.find("hit its usage limit") != std::string::npos;
            expect(wrote && r.asked.size() == 1 && r.asked[0].reason == "reviewer: the reviewer hit its usage limit" && has_notice(r, "reviewer: sonnet-5 hit its usage limit; reviewing on haiku-4.5") && notices == 1,
                   "a usage limit on the reviewer asks this action and says once where reviews go now");
            Recorder r2;
            expect(write_turn(*a, r2) && r2.asked.empty() && r2.notices.empty() && review_models(fb) == std::vector<std::string>{"sonnet", "haiku"}, "the next review runs on Haiku, without retrying Sonnet");
        }
        // Haiku runs out with nothing cheaper: the reviewer is off, and what it would review is asked.
        {
            reset();
            fb.fail_when = [&](const json& b) { return is_review(b); };
            auto a = make("haiku-4.5");
            Recorder r;
            r.reply = {Approval::Yes, ""};
            write_turn(*a, r);
            expect(r.asked.size() == 1 && has_notice(r, "reviewer: haiku-4.5 hit its usage limit and nothing cheaper is left; every action it would review is asked for the rest of the session"),
                   "no fallback left: one notice, and the action is asked");
            Recorder r2;
            r2.reply = {Approval::Yes, ""};
            bool wrote = write_turn(*a, r2);
            expect(wrote && r2.asked.size() == 1 && r2.asked[0].reason.find("reviewer: no reviewer: haiku-4.5 hit its usage limit") == 0 && r2.notices.empty() && review_models(fb).size() == 1,
                   "later actions are asked without another notice or another call to the limited model");
            expect(a->reviewer().pick.model.empty(), ":harness shows the reviewer off");
        }
        // Spend: reviewer tokens count toward the session, and reviewer_budget_tokens caps them on their own.
        {
            reset();
            fb.usage_input = 10;  // each review reports 10 in, 5 out
            auto a = make("haiku-4.5");
            a->reviewer_budget_tokens = 10;
            Recorder r;
            write_turn(*a, r);
            expect(a->reviewer().tokens == 15 && a->usage().total_input == 10 && a->usage().total_output == 5, "the review's tokens count toward the session's totals (" + std::to_string(a->usage().total_input) + " in)");
            Recorder r2;
            r2.reply = {Approval::Yes, ""};
            write_turn(*a, r2);
            expect(r2.asked.size() == 1 && has_notice(r2, "reviewer: its token budget is used up (15 of 10); every action it would review is asked from now on") && review_models(fb).size() == 1,
                   "past reviewer_budget_tokens the reviewer stops and the action is asked");
            Recorder r3;
            r3.reply = {Approval::Yes, ""};
            write_turn(*a, r3);
            expect(r3.asked.size() == 1 && r3.notices.empty(), "with no second notice");
            fb.usage_input = 0;
        }
        fa.reply = fb.reply = nullptr;
        fa.tool_call = json();
        reset();
    }

    section("the reviewer and summaries on claude-cli (a fake claude on PATH)");
    {
        // The session's model is on the fake server; the reviewer and summaries go to a fake `claude`.
        fs::path dir = ws / "fake-claude";
        std::string saved_path = std::getenv("PATH") ? std::getenv("PATH") : "";
        fake_claude::install(dir);
        FakeServer fake;
        Provider cli;
        for (const auto& p : default_providers()) if (p.name == "claude-cli") cli = p;
        std::vector<ModelPreset> presets = default_presets();
        for (auto& p : presets) {
            if (p.name == "opus-5.5") p.model = "fake/opus";
            if (p.name == "haiku-4.5") p.model = "fake/haiku";
        }
        auto is_review = [](const json& b) { return b["messages"][0]["content"].get<std::string>().rfind("You review one action", 0) == 0; };
        fake.reply = [&](const json& b) { return is_review(b) ? std::string("ALLOW: fine") : std::string(); };
        auto reviews_on_fake = [&] { return std::count_if(fake.requests.begin(), fake.requests.end(), is_review); };
        auto make = [&] {
            auto a = std::make_unique<Agent>(ws, "fake/opus");
            a->providers = {fake.provider(), cli};
            a->presets = presets;
            a->mode = Mode::Auto;
            a->small_model = "claude-cli/haiku";
            return a;
        };
        int n = 0;
        auto write_turn = [&](Agent& a, Recorder& r, const std::string& word) {
            std::string path = "cli" + std::to_string(++n) + "-" + word + ".txt";
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", path}, {"content", "x"}}}};
            fake.calls_left = 1;
            a.submit("please write " + path, Origin::Local, r, no_cancel);
            return fs::exists(ws / path);
        };
        {
            SessionLog log("agent-test");
            auto a = make();
            a->set_log(&log);
            Recorder r;
            bool wrote = write_turn(*a, r, "ok");
            json review;
            std::ifstream in(log.path());
            for (std::string l; std::getline(in, l);) {
                json j = json::parse(l, nullptr, false);
                if (j.is_object() && j.value("type", "") == "tool" && j.contains("review")) review = j["review"];
            }
            expect(wrote && r.asked.empty() && reviews_on_fake() == 0 && review.value("model", "") == "claude-cli/haiku" && review.value("model_reason", "") == "small_model" &&
                       review.value("verdict", "") == "allow",
                   "small_model = claude-cli/haiku reviews the write through the CLI, which allows it: " + review.dump());
            bool started = false;
            for (const auto& sp : fake_claude::spawns(dir)) {
                const json& av = sp["argv"];
                for (size_t i = 0; i + 1 < av.size(); ++i) started = started || (av[i] == "--system-prompt" && av[i + 1].get<std::string>().rfind("You review one action", 0) == 0);
            }
            expect(started && a->reviewer().tokens == 15, "the reviewer's prompt is the CLI's system prompt, and its tokens count (" + std::to_string(a->reviewer().tokens) + ")");
            fs::remove(log.path());
        }
        {
            // The plan runs out: this action is asked, and reviews move to the API's Haiku.
            auto a = make();
            Recorder r;
            r.reply = {Approval::Yes, ""};
            bool wrote = write_turn(*a, r, "LIMIT");
            expect(wrote && r.asked.size() == 1 && has_notice(r, "reviewer: claude-haiku-cli hit its usage limit; reviewing on haiku-4.5"),
                   "a usage limit from the CLI asks this action and moves the reviewer to the API preset");
            Recorder r2;
            expect(write_turn(*a, r2, "next") && r2.asked.empty() && reviews_on_fake() == 1, "the next review runs on haiku-4.5");
        }
        {
            // Level 2: Claude Code as the session's model runs the loop; every call it makes goes through MAIC's
            // harness like any model's: asked in manual mode, refused when the person says no, in the transcript.
            const char* old_rt = std::getenv("XDG_RUNTIME_DIR");
            std::string saved_rt = old_rt ? old_rt : "";
            setenv("XDG_RUNTIME_DIR", (ws / "run-l2").c_str(), 1);
            fs::create_directories(ws / "run-l2");
            SessionLog log("agent-test");
            Agent a(ws, "claude-cli/sonnet");
            a.providers = {fake.provider(), cli};
            a.mode = Mode::Manual;
            a.set_log(&log);
            Recorder r;
            r.reply = {Approval::Yes, ""};
            a.submit("CALL write_file {\"path\": \"l2-a.txt\", \"content\": \"from claude\"}", Origin::Local, r, no_cancel);
            std::ifstream written(ws / "l2-a.txt");
            std::string got((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
            expect(r.asked.size() == 1 && got == "from claude" && r.text.find("results: ") != std::string::npos,
                   "Claude Code's write is asked in manual mode, then runs, and its result goes back to it: " + r.text);
            Recorder no;
            a.submit("CALL run_shell {\"command\": \"touch l2-b.txt\"}", Origin::Local, no, no_cancel);
            expect(no.asked.size() == 1 && !fs::exists(ws / "l2-b.txt") && no.text.find("results: ERROR") != std::string::npos,
                   "a call the person refuses does not run, and Claude Code hears it as an error: " + no.text);
            int tools = 0;
            std::ifstream in(log.path());
            for (std::string l; std::getline(in, l);) {
                json j = json::parse(l, nullptr, false);
                if (j.is_object() && j.value("type", "") == "tool" && (j.value("tool", "") == "write_file" || j.value("tool", "") == "run_shell")) ++tools;
            }
            expect(tools == 2, "both calls are in the transcript as MAIC's own tool records (" + std::to_string(tools) + ")");
            size_t spawned = 0;
            for (const auto& sp : fake_claude::spawns(dir)) spawned += sp["argv"].dump().find("mcp__maic") != std::string::npos;
            expect(spawned == 1, "one Claude Code process carried the conversation across both turns (" + std::to_string(spawned) + ")");
            fs::remove(log.path());
            if (old_rt) setenv("XDG_RUNTIME_DIR", saved_rt.c_str(), 1);
            else unsetenv("XDG_RUNTIME_DIR");
        }
        {
            // A Claude Code preset spends the user's own plan: a subagent runs on one only when it was asked for, or when
            // its parent runs on a Claude model; the automatic pick passes it over otherwise.
            const char* old_rt = std::getenv("XDG_RUNTIME_DIR");
            std::string saved_rt = old_rt ? old_rt : "";
            setenv("XDG_RUNTIME_DIR", (ws / "run-sub").c_str(), 1);
            fs::create_directories(ws / "run-sub");
            auto agents_spawned = [&] {
                size_t n = 0;
                for (const auto& sp : fake_claude::spawns(dir)) n += sp["argv"].dump().find("mcp__maic") != std::string::npos;
                return n;
            };
            // Opus prefers claude-haiku-cli for its subagents and lists it; `opus` is the model the preset names.
            auto task_on = [&](const std::string& opus, const char* asked) {
                std::vector<ModelPreset> with_cli = presets;
                for (auto& p : with_cli) {
                    if (p.name != "opus-5.5") continue;
                    p.model = opus;
                    p.subagent = "claude-haiku-cli";
                    p.subagents.push_back("claude-haiku-cli");
                }
                json args = {{"agent", "explore"}, {"prompt", "map it"}};
                if (asked) args["model"] = asked;
                int parent_calls = 0;
                fake.tool_call_for = [&](const json& b) -> json { return !from_child(b) && ++parent_calls == 1 ? json{{"name", "task"}, {"arguments", args}} : json(); };
                Agent a(ws, opus);
                a.providers = {fake.provider(), cli};
                a.presets = with_cli;
                a.mode = Mode::Auto;
                a.review_with_model = false;
                Recorder r;
                a.submit("start it", Origin::Local, r, no_cancel);
                fake.tool_call_for = nullptr;
                return r;
            };
            size_t before = agents_spawned();
            Recorder automatic = task_on("fake/opus", nullptr);
            expect(has_call(automatic, "↳ explore on opus-5.5 (claude-haiku-cli runs through Claude Code on your plan, which a subagent takes only when asked for or under a Claude model; the same model)") &&
                       agents_spawned() == before,
                   "under a model that is not Claude, the automatic pick passes a Claude Code preset over: the subagent stays on the same model");
            Recorder asked = task_on("fake/opus", "claude-haiku-cli");
            expect(has_call(asked, "↳ explore on claude-haiku-cli (asked for by the parent)") && agents_spawned() == before + 1 && !asked.results.empty() &&
                       asked.results.back().find("call 1: map it") != std::string::npos,
                   "a Claude Code preset asked for by name runs the subagent through Claude Code: " + (asked.results.empty() ? "" : asked.results.back()));
            Recorder under_claude = task_on("fake/claude-opus", nullptr);
            expect(has_call(under_claude, "↳ explore on claude-haiku-cli (opus-5.5's subagent setting)") && agents_spawned() == before + 2,
                   "under a Claude model the automatic pick may take it");
            if (old_rt) setenv("XDG_RUNTIME_DIR", saved_rt.c_str(), 1);
            else unsetenv("XDG_RUNTIME_DIR");
        }
        std::vector<Message> hist = {{"system", "sys"}, {"user", "turn 1"}, {"assistant", "reply 1"}, {"user", "turn 2"}, {"assistant", "reply 2"}};
        {
            // A remote session (never contacted here) with compact_model on the CLI.
            Agent a(ws, "cloud/model");
            a.providers = {{"cloud", "openai", "https://cloud.invalid/v1"}, cli};
            a.compaction.model = "claude-cli/sonnet";
            a.restore(hist);
            a.compact(Agent::Compaction::All, no_cancel);
            expect(a.messages().size() == 2 && a.messages()[1].content.find("summarised by sonnet") != std::string::npos, "compact_model = claude-cli/sonnet writes the summary");
        }
        {
            // A local session keeps its history on the machine: the remote compact_model is passed over.
            Agent a(ws, "fake/opus");
            a.providers = {fake.provider(), cli};
            a.compaction.model = "claude-cli/sonnet";
            a.restore(hist);
            a.compact(Agent::Compaction::All, no_cancel);
            expect(a.messages().size() == 2 && a.messages()[1].content.find("echo: The conversation") != std::string::npos, "a local session's summary stays on its own model");
        }
        setenv("PATH", saved_path.c_str(), 1);
    }

    section("context files");
    {
        FakeServer fake;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        std::ofstream(ws / "ctx.txt") << "the word is heron\n";
        std::string note = agent.add_context_file(ws / "ctx.txt");
        expect(note.find("ctx.txt") != std::string::npos && note.find("18 bytes") != std::string::npos, "attaching reports the file and size");
        Recorder r;
        agent.submit("go", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests[0]["messages"];
        expect(msgs.size() == 3 && msgs[1]["role"] == "user" && msgs[1]["content"].get<std::string>().find("heron") != std::string::npos &&
               msgs[1]["content"].get<std::string>().find("ctx.txt") != std::string::npos && msgs[2]["content"] == "go",
               "the file arrives as a labelled user message before the prompt");
        std::ofstream(ws / "bin.dat", std::ios::binary) << std::string("ab\0cd", 5);
        bool threw = false;
        try {
            agent.add_context_file(ws / "bin.dat");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a binary file is refused");
    }

    section("cancel");
    {
        FakeServer fake;
        fake.hold_left = 1;  // the reply never ends on its own: only the cancel can bring submit back
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            fake.wait_streaming(1);
            cancel = true;
        });
        auto t0 = std::chrono::steady_clock::now();
        agent.submit("slow", Origin::Local, r, cancel);
        canceller.join();
        expect(has_notice(r, "interrupted") && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "Ctrl-C stops the turn without waiting for the stream");
        cancel = false;
        agent.submit("after", Origin::Local, r, cancel);
        bool marker = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            if (m["role"] == "user" && m["content"] == "[interrupted by the user]") marker = true;
        }
        expect(marker, "the interruption is recorded in the history for the next turn");
    }

    section("session log and resume");
    {
        FakeServer fake;
        fs::path path;
        {
            SessionLog log("agent-test");
            path = log.path();
            Agent agent(ws, "test");
            agent.providers = {fake.provider()};
            agent.set_log(&log);
            Recorder r;
            agent.submit("remember the word pelican", Origin::Local, r, no_cancel);
        }
        LoadedSession loaded = load_session(path);
        expect(loaded.messages.size() == 3 && loaded.messages[0].role == "system" && loaded.messages[2].content == "echo: remember the word pelican",
               "every message is stored and loads back (system, user, assistant)");
        expect(loaded.transcript.size() == 2 && loaded.transcript[0].type == "user", "the displayable transcript loads too");
        auto infos = list_sessions(ws);
        expect(!infos.empty() && infos.front().path == path && infos.front().first_prompt == "remember the word pelican" && infos.front().turns == 1,
               "list_sessions finds it by workspace with a preview");
        expect(infos.front().home == "general" && path.parent_path() == sessions_home("general"), "a new session lands in general/");
        expect(infos.front().opens == 1 && !infos.front().host.empty() && infos.front().opened_in == infos.front().workspace,
               "the start record says where and on which host it was opened");
        expect(find_session(infos.front().id.substr(0, 16)).has_value(), "find_session accepts a unique id prefix");

        // Resume: history continues, a system note marks the resume, and the log keeps appending to the same file.
        SessionLog log = SessionLog::reopen(path);
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.set_log(&log);
        agent.restore(loaded.messages);
        Recorder r;
        agent.submit("again", Origin::Local, r, no_cancel);
        const auto& msgs = fake.requests.back()["messages"];
        bool has_old = false, has_note = false;
        for (const auto& m : msgs) {
            if (m["content"] == "remember the word pelican") has_old = true;
            if (m["role"] == "system" && m["content"].get<std::string>().find("resumed") != std::string::npos) has_note = true;
        }
        expect(has_old && has_note && msgs[0]["content"] == loaded.messages[0].content, "the resumed turn replays the old history and the original system prompt");
        expect(load_session(path).messages.size() > loaded.messages.size(), "new messages append to the same session file");
        auto again = list_sessions(ws);
        expect(!again.empty() && again.front().opens == 2, "a resume counts as another open");

        // A fork points at its parent; rehoming the parent must not break it.
        {
            SessionLog child = SessionLog::fork(path, count_records(path), "agent-test");
            Agent a3(ws, "test");
            a3.providers = {fake.provider()};
            a3.set_log(&child);
            a3.restore(load_session(path).messages);
            a3.submit("fork turn", Origin::Local, r, no_cancel);
            auto parent_info = find_session(path.stem().string());
            fs::path moved = sessions_home("project:" + parent_info->workspace) / path.filename();
            rehome_session({*parent_info, moved});
            expect(moved.parent_path().parent_path().filename() == "projects" && !fs::exists(path), "rehome moves the parent into projects/<encoded workspace>/");
            LoadedSession forked = load_session(child.path());
            bool has_pelican = false;
            for (const auto& m : forked.messages) has_pelican = has_pelican || m.content == "remember the word pelican";
            expect(has_pelican, "the fork still loads its parent's history after the move (found by id)");
            auto moved_info = find_session(moved.stem().string());
            expect(moved_info && moved_info->home.rfind("projects/", 0) == 0, "the moved session lists under its new home");
            fs::remove(child.path());
            fs::remove(moved);
            fs::remove(moved.parent_path(), ec_ignore());
        }

        // A session that ended mid tool call must not resume with a dangling call.
        std::vector<Message> broken = loaded.messages;
        broken.push_back({"assistant", "", {{"id1", "read_file", {{"path", "x"}}}}});
        Agent agent2(ws, "test");
        agent2.providers = {fake.provider()};
        agent2.restore(broken);
        agent2.submit("go on", Origin::Local, r, no_cancel);
        bool dangling = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            if (m.contains("tool_calls")) dangling = true;
        }
        expect(!dangling, "a dangling tool call from the old session is dropped on resume");
    }

    section(":cd");
    {
        FakeServer fake;
        fs::path a = ws / "cd-a", b = ws / "cd-b";
        fs::create_directories(a);
        fs::create_directories(b);
        std::ofstream(b / "MAIC.md") << "Fennec ears stay a third of her height.\n";
        trust_for_session(b);  // this test's own project (trust_test covers untrusted ones)
        SessionLog log("agent-test");
        Agent agent(a, "test");
        agent.providers = {fake.provider()};
        agent.set_log(&log);
        Recorder r;
        agent.submit("hello", Origin::Local, r, no_cancel);
        bool refused = false;
        try {
            agent.set_workspace(b, Origin::Remote);
        } catch (const std::exception&) {
            refused = true;
        }
        expect(refused && agent.harness().workspace() == fs::weakly_canonical(a), "a remote origin cannot change the workspace");
        agent.set_workspace(b, Origin::Local);
        expect(agent.harness().workspace() == fs::weakly_canonical(b), "the harness root moves");
        agent.submit("where now", Origin::Local, r, no_cancel);
        bool noted = false;
        for (const auto& m : fake.requests.back()["messages"]) {
            std::string c = m["content"].is_string() ? m["content"].get<std::string>() : "";
            if (m["role"] == "system" && c.find("The workspace moved from " + fs::weakly_canonical(a).string() + " to " + fs::weakly_canonical(b).string()) != std::string::npos &&
                c.find("Fennec ears stay a third of her height.") != std::string::npos)
                noted = true;
        }
        expect(noted, "the model gets a system note: where the workspace moved and the instructions there");
        json rec;
        std::ifstream in(log.path());
        for (std::string l; std::getline(in, l);) {
            auto j = json::parse(l, nullptr, false);
            if (j.is_object() && j.value("type", "") == "workspace") rec = j;
        }
        expect(rec.value("from", "") == fs::weakly_canonical(a).string() && rec.value("to", "") == fs::weakly_canonical(b).string(), "the transcript gets a workspace record {from, to}");
    }

    section("FakeServer's chunks against OpenAI's pinned CreateChatCompletionStreamResponse");
    {
        std::ifstream in(std::string(MAIC_PROTOCOL) + "/openai/subset.json");
        json subset = json::parse(in);
        const json chunk = {{"$ref", "#/components/schemas/CreateChatCompletionStreamResponse"}};
        std::string first;
        int bad = 0;
        std::set<std::string> kinds;
        std::lock_guard lock(FakeServer::sent_mu);
        for (const auto& line : FakeServer::sent) {
            json c = json::parse(line);
            if (std::string e = schema_error(subset, chunk, c); !e.empty() && bad++ == 0) first = e + " in " + line;
            const json& choice = c["choices"][0];
            if (c.contains("usage")) kinds.insert("usage");
            if (choice["finish_reason"].is_string()) kinds.insert("finish " + choice["finish_reason"].get<std::string>());
            for (const auto& [k, v] : choice["delta"].items()) kinds.insert(k);
            if (choice["delta"].empty()) kinds.insert("empty");
        }
        expect(kinds == std::set<std::string>{"content", "empty", "finish stop", "finish tool_calls", "tool_calls", "usage"},
               "the suite sent every kind of chunk: text, a tool call, an empty keepalive, both finishes, usage (" + std::to_string(FakeServer::sent.size()) + " distinct)");
        expect(bad == 0, "every chunk FakeServer sent fits the schema" + (first.empty() ? "" : ": " + std::to_string(bad) + " do not, first " + first));
    }

    fs::remove_all(ws);
    return finish();
}
