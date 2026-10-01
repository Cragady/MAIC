#include <unistd.h>
#include <cstdlib>
// The agent loop against a fake OpenAI-compatible server: mid-turn messages, deliver-now, cancellation, resume.
#include "check.hpp"

#include "maic/agent.hpp"
#include "maic/settings.hpp"
#include "maic/tripwire.hpp"
#include "maic/session.hpp"

#include "maic/http.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace maic;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// An OpenAI-compatible /v1/chat/completions that answers every chat with a slow SSE stream of the last user
// message's text, echoed four characters at a time. mid_system keeps later system messages as system turns, so
// the tests can look for them; context_window matches the shipped llamacpp provider.
struct FakeServer {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::vector<json> requests;
    std::mutex mu;
    int delay_ms = 100;
    int usage_input = 0;  // reported as prompt_tokens in the final usage chunk when set
    json tool_call;       // when set and calls_left > 0, the reply is this one tool call ({"name", "arguments"})
    int calls_left = 0;
    std::function<std::string(const json&)> reply;  // when set and non-empty for a request, replaces the echo
    int fail_left = 0;      // answer this many requests with fail_status / fail_body first
    int fail_status = 400;
    std::string fail_body;

    static std::string text_of(const json& content) {
        if (content.is_string()) return content;
        std::string out;
        for (const auto& part : content) {
            if (part.value("type", "") == "text") out += part.value("text", "");
        }
        return out;
    }
    static std::string event(const json& j) { return "data: " + j.dump() + "\n\n"; }

    FakeServer() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            {
                std::lock_guard lock(mu);
                requests.push_back(body);
            }
            std::string last;
            for (const auto& m : body["messages"]) {
                if (m["role"] == "user") last = text_of(m["content"]);
            }
            if (fail_left > 0) {
                --fail_left;
                res.status = fail_status;
                res.set_content(fail_body, "application/json");
                return;
            }
            std::string echo = "echo: " + last;
            if (reply) {
                std::string r = reply(body);
                if (!r.empty()) echo = r;
            }
            int delay = delay_ms;
            int usage = usage_input;
            json call;
            if (!tool_call.is_null() && calls_left > 0) {
                --calls_left;
                call = tool_call;
            }
            res.set_chunked_content_provider("text/event-stream", [echo, delay, usage, call](size_t, httplib::DataSink& sink) {
                auto write = [&](const std::string& s) { return sink.write(s.data(), s.size()); };
                if (!call.is_null()) {
                    json tc = {{"index", 0}, {"id", "call_1"}, {"type", "function"},
                               {"function", {{"name", call["name"]}, {"arguments", call["arguments"].dump()}}}};
                    write(event({{"choices", {{{"index", 0}, {"delta", {{"content", ""}, {"tool_calls", {tc}}}}}}}}));
                    write(event({{"choices", {{{"index", 0}, {"delta", json::object()}, {"finish_reason", "tool_calls"}}}}}));
                    write("data: [DONE]\n\n");
                    sink.done();
                    return true;
                }
                for (size_t i = 0; i < echo.size(); i += 4) {
                    if (!write(event({{"choices", {{{"index", 0}, {"delta", {{"content", echo.substr(i, 4)}}}}}}}))) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                json done = {{"choices", {{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}}}};
                if (usage) done["usage"] = {{"prompt_tokens", usage}, {"completion_tokens", 5}};
                write(event(done));
                write("data: [DONE]\n\n");
                sink.done();
                return true;
            });
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeServer() {
        srv.stop();
        thread.join();
    }
    Provider provider() const {
        return {"fake", "openai", "http://127.0.0.1:" + std::to_string(port) + "/v1", "", "", {{"mid_system", true}, {"context_window", 16384}}};
    }
};

struct Recorder : AgentEvents {
    std::string text;
    std::vector<std::string> notices;
    void on_text(std::string_view d, bool) override { text += d; }
    void on_tool_call(const std::string&) override {}
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

}  // namespace

int main() {
    setenv("MAIC_TRIPWIRE_FILE", ("/tmp/maic-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);  // never the machine's lock
    fs::path ws = fs::temp_directory_path() / "maic-agent-test";
    fs::remove_all(ws);
    fs::create_directories(ws);
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 150;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        std::thread poster([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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

    section("undo points and nested instructions");
    {
        FakeServer fake;
        fake.delay_ms = 1;
        fs::create_directories(ws / "svc");
        std::ofstream(ws / "svc" / "AGENTS.md") << "svc rules: use tabs";
        std::ofstream(ws / "svc" / "a.txt") << "old\n";
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        agent.mode = Mode::Auto;
        agent.review_with_model = false;  // this section tests undo, not the reviewer
        Recorder r;
        fake.tool_call = json{{"name", "read_file"}, {"arguments", {{"path", "svc/a.txt"}}}};
        fake.calls_left = 1;
        agent.submit("read it", Origin::Local, r, no_cancel);
        expect(!r.results.empty() && r.results[0].find("svc rules: use tabs") != std::string::npos && r.results[0].find("Instructions from") != std::string::npos,
               "reading a file attaches the AGENTS.md above it");
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
        fake.tool_call = json{{"name", "move_file"}, {"arguments", {{"from", "svc/gone.txt"}, {"to", (fs::temp_directory_path() / "maic-agent-test-out.txt").string()}}}};
        fake.calls_left = 1;
        r.asked.clear();
        r.results.clear();
        agent.submit("move out", Origin::Local, r, no_cancel);
        expect(r.asked.size() == 1 && r.asked[0].reason == "outside the workspace" && r.asked[0].summary.find("move_file svc/gone.txt -> ") == 0 && fs::exists(ws / "svc" / "gone.txt") &&
                   !fs::exists(fs::temp_directory_path() / "maic-agent-test-out.txt") && r.results.size() == 1 && r.results[0].find("DENIED") == 0 && agent.undo_points().empty(),
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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

        fs::path outside = fs::temp_directory_path() / "maic-agent-test-escape.txt";
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

    section("operator instructions set mid-conversation");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
    }

    section("forbidden terms halt a call under any harness");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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

    section("a request over the context window");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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
    }

    section("string and token bans");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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

    section("the reviewer (smart harness) and the dumb harness");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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
        side.delay_ms = 1;
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

    section("context files");
    {
        FakeServer fake;
        fake.delay_ms = 1;
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
        fake.delay_ms = 200;
        Agent agent(ws, "test");
        agent.providers = {fake.provider()};
        Recorder r;
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            cancel = true;
        });
        auto t0 = std::chrono::steady_clock::now();
        agent.submit("slow", Origin::Local, r, cancel);
        canceller.join();
        expect(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2) && has_notice(r, "interrupted"), "Ctrl-C stops the turn quickly");
        cancel = false;
        fake.delay_ms = 1;
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
        fake.delay_ms = 1;
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
            fs::path moved = rehome_session(*parent_info, "project");
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

    fs::remove_all(ws);
    return finish();
}
