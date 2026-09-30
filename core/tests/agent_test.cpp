// The agent loop against a fake Ollama: mid-turn messages, deliver-now, cancellation, resume.
#include "check.hpp"

#include "maic/agent.hpp"
#include "maic/session.hpp"

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace maic;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Answers every chat with a slow stream of the last user message's text, echoed word by word.
struct FakeOllama {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::vector<json> requests;
    std::mutex mu;
    int delay_ms = 100;

    FakeOllama() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            {
                std::lock_guard lock(mu);
                requests.push_back(body);
            }
            std::string last;
            for (const auto& m : body["messages"]) {
                if (m["role"] == "user") last = m["content"];
            }
            int delay = delay_ms;
            res.set_chunked_content_provider("application/x-ndjson", [last, delay](size_t, httplib::DataSink& sink) {
                std::string out = "echo: " + last;
                for (size_t i = 0; i < out.size(); i += 4) {
                    std::string line = json{{"message", {{"content", out.substr(i, 4)}}}}.dump() + "\n";
                    if (!sink.write(line.data(), line.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                std::string done = json{{"done", true}}.dump() + "\n";
                sink.write(done.data(), done.size());
                sink.done();
                return true;
            });
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeOllama() {
        srv.stop();
        thread.join();
    }
    Provider provider() const { return {"ollama", "ollama", "http://127.0.0.1:" + std::to_string(port)}; }
};

struct Recorder : AgentEvents {
    std::string text;
    std::vector<std::string> notices;
    void on_text(std::string_view d, bool) override { text += d; }
    void on_tool_call(const std::string&) override {}
    void on_tool_result(const std::string&, bool) override {}
    void on_notice(const std::string& t) override { notices.push_back(t); }
    Approval ask(const ApprovalRequest&) override { return Approval::No; }
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
    fs::path ws = fs::temp_directory_path() / "maic-agent-test";
    fs::remove_all(ws);
    fs::create_directories(ws);
    std::atomic<bool> no_cancel{false};

    section("plain turn");
    {
        FakeOllama fake;
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
        FakeOllama fake;
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
        FakeOllama fake;
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
        FakeOllama fake;
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

    section("context files");
    {
        FakeOllama fake;
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
        FakeOllama fake;
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
        FakeOllama fake;
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
