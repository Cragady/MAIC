// The server against a fake OpenAI-compatible server: tokens, streaming, the remote-origin approval round trip, interrupt, audit.
#include "check.hpp"

#include "artifacts.hpp"
#include "auth.hpp"
#include "maid/paths.hpp"
#include "maid/session.hpp"
#include "maid/trust.hpp"
#include "server.hpp"
#include "tls.hpp"

#include "maid/http.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace maid;
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// An OpenAI-compatible /v1/chat/completions that echoes the last user message four characters at a time, or
// replies with one tool call while calls_left > 0.
struct FakeServer {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::vector<json> requests;
    std::mutex mu;
    int delay_ms = 1;
    json tool_call;
    int calls_left = 0;

    static std::string event(const json& j) { return "data: " + j.dump() + "\n\n"; }

    FakeServer() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            std::string last;
            for (const auto& m : body["messages"]) {
                if (m["role"] == "user" && m["content"].is_string()) last = m["content"];
            }
            int delay;
            json call;
            {
                std::lock_guard lock(mu);
                requests.push_back(body);
                delay = delay_ms;
                if (!tool_call.is_null() && calls_left > 0) {
                    --calls_left;
                    call = tool_call;
                }
            }
            res.set_chunked_content_provider("text/event-stream", [last, delay, call](size_t, httplib::DataSink& sink) {
                auto write = [&](const std::string& s) { return sink.write(s.data(), s.size()); };
                std::string finish = "stop";
                if (!call.is_null()) {
                    json tc = {{"index", 0}, {"id", "call_1"}, {"type", "function"},
                               {"function", {{"name", call["name"]}, {"arguments", call["arguments"].dump()}}}};
                    write(event({{"choices", {{{"index", 0}, {"delta", {{"content", ""}, {"tool_calls", {tc}}}}}}}}));
                    finish = "tool_calls";
                } else {
                    std::string out = "echo: " + last;
                    for (size_t i = 0; i < out.size(); i += 4) {
                        if (!write(event({{"choices", {{{"index", 0}, {"delta", {{"content", out.substr(i, 4)}}}}}}}))) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    }
                }
                write(event({{"choices", {{{"index", 0}, {"delta", json::object()}, {"finish_reason", finish}}}}, {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}}}));
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
    Provider provider() const { return {"fake", "openai", "http://127.0.0.1:" + std::to_string(port) + "/v1"}; }
    json last_request() {
        std::lock_guard lock(mu);
        return requests.back();
    }
};

struct Api {
    std::string base;
    std::string token;

    httplib::Headers auth() const { return {{"Authorization", "Bearer " + token}}; }

    json get(const std::string& path, int* status = nullptr) {
        httplib::Client c(base);
        auto res = c.Get(path, auth());
        if (status) *status = res ? res->status : 0;
        return res ? json::parse(res->body, nullptr, false) : json();
    }
    json post(const std::string& path, const json& body, int* status = nullptr) {
        httplib::Client c(base);
        auto res = c.Post(path, auth(), body.dump(), "application/json");
        if (status) *status = res ? res->status : 0;
        return res ? json::parse(res->body, nullptr, false) : json();
    }
    // Reads a server-sent event stream to its end; returns the events in order.
    std::vector<json> stream(const std::string& method, const std::string& path, const json& body = json()) {
        httplib::Client c(base);
        c.set_read_timeout(60);
        httplib::Request req;
        req.method = method;
        req.path = path;
        req.set_header("Authorization", "Bearer " + token);
        if (!body.is_null()) {
            req.set_header("Content-Type", "application/json");
            req.body = body.dump();
        }
        std::string buf;
        req.content_receiver = [&](const char* d, size_t n, uint64_t, uint64_t) {
            buf.append(d, n);
            return true;
        };
        httplib::Response res;
        httplib::Error err = httplib::Error::Success;
        c.send(req, res, err);
        std::vector<json> events;
        size_t pos = 0;
        while (pos < buf.size()) {
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) nl = buf.size();
            std::string line = buf.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.rfind("data: ", 0) == 0) events.push_back(json::parse(line.substr(6)));
        }
        return events;
    }
};

std::string text_of(const std::vector<json>& events) {
    std::string out;
    for (const auto& e : events) {
        if (e["type"] == "response.output_text.delta") out += e["delta"].get<std::string>();
    }
    return out;
}

// The tool's output item, done: function_call_output or shell_call_output.
const json* tool_result(const std::vector<json>& events) {
    for (const auto& e : events) {
        if (e["type"] == "response.output_item.done" && (e["item"]["type"] == "function_call_output" || e["item"]["type"] == "shell_call_output")) return &e;
    }
    return nullptr;
}

const json* find_event(const std::vector<json>& events, const std::string& type) {
    for (const auto& e : events) {
        if (e["type"] == type) return &e;
    }
    return nullptr;
}

// Polls the session until an approval is waiting; returns it (null after the timeout).
json wait_for_approval(Api& api, const std::string& id) {
    for (int i = 0; i < 100; ++i) {
        json s = api.get("/api/sessions/" + id);
        if (s.is_object() && !s["pending_approval"].is_null()) return s["pending_approval"];
        std::this_thread::sleep_for(50ms);
    }
    return json();
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    fs::path root = fs::temp_directory_path() / "maid-server-test";
    fs::remove_all(root);
    fs::create_directories(root / "state");
    fs::create_directories(root / "ws");
    fs::create_directories(root / "outside");
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    fs::path state = state_dir() / "server";

    section("tokens");
    std::string token;
    {
        server::TokenStore store(state / "tokens.json");
        token = store.create("phone");
        expect(token.size() == 32, "a token is 32 url-safe characters");
        expect(store.verify(token) == std::optional<std::string>("phone"), "the token verifies to its name");
        expect(!store.verify(token.substr(0, 31) + "!") && !store.verify(""), "a wrong or empty token does not");
        std::string laptop = store.create("laptop");
        expect(store.list().size() == 2 && slurp(state / "tokens.json").find(laptop) == std::string::npos, "only hashes are on disk");
        struct stat st {};
        expect(stat((state / "tokens.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "tokens.json is 0600");
        bool threw = false;
        try {
            store.create("phone");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a name can't be reused without revoking it");
        expect(store.revoke("laptop") && !store.revoke("laptop") && !store.verify(laptop), "revoke removes the token");
        server::TokenStore again(state / "tokens.json");
        expect(again.verify(token).has_value(), "a fresh store reads the file back");
    }
    {
        server::RateLimit limit(3, 1s);
        for (int i = 0; i < 3; ++i) limit.failed("10.0.0.9");
        expect(limit.blocked("10.0.0.9") && !limit.blocked("10.0.0.8"), "three failures block that source only");
        std::this_thread::sleep_for(1100ms);
        expect(!limit.blocked("10.0.0.9"), "the block lifts after the window");
        limit.failed("10.0.0.9");
        limit.succeeded("10.0.0.9");
        limit.failed("10.0.0.9");
        limit.failed("10.0.0.9");
        expect(!limit.blocked("10.0.0.9"), "a success clears the count");
    }

    FakeServer fake;
    server::ServerOptions o;
    o.listen = "127.0.0.1:0";
    o.settings.model = "test";
    o.settings.mode = "manual";
    o.settings.providers = {fake.provider()};
    o.workspaces = {root / "ws"};
    o.state = state;
    o.web = fs::path(__FILE__).parent_path().parent_path() / "web" / "index.html";
    o.artifacts = root / "artifacts";
    o.vue = fs::path(__FILE__).parent_path().parent_path().parent_path() / "vendor" / "vue";
    server::Server server(o);
    int port = server.bind();
    std::thread serving([&] { server.run(); });
    Api api{"http://127.0.0.1:" + std::to_string(port), token};

    section("authentication");
    {
        int status = 0;
        Api none{api.base, ""};
        none.get("/api/status", &status);
        expect(status == 401, "no token: 401");
        Api wrong{api.base, "not-a-token-at-all-not-a-token-at"};
        wrong.get("/api/status", &status);
        expect(status == 401, "wrong token: 401");
        json s = api.get("/api/status", &status);
        expect(status == 200 && s.contains("harness") && s["tls"] == false && s["model"] == "test", "the right token reaches /api/status");
        httplib::Client c(api.base);
        auto page = c.Get("/");
        expect(page && page->status == 200 && page->body.find("<html") != std::string::npos, "the web client is served without a token");
        std::string audit = slurp(state / "audit.log");
        expect(audit.find("127.0.0.1 - GET /api/status 401") != std::string::npos, "a refused request is audited without a name");
        expect(audit.find("127.0.0.1 phone GET /api/status 200") != std::string::npos, "an accepted request is audited with the token's name");
        expect(!server.tls() && server.fingerprint().empty(), "loopback runs without TLS");
    }

    section("sessions");
    std::string id;
    {
        int status = 0;
        json s = api.post("/api/sessions", json::object(), &status);
        expect(status == 201 && s["mode"] == "manual" && s["workspace"] == fs::weakly_canonical(root / "ws").string(), "a session opens in the default workspace");
        id = s["id"];
        expect(id.find("-server-") != std::string::npos && find_session(id) && !s.contains("transcript"), "it is recorded as kind server, its path not shown remotely: " + id);
        api.post("/api/sessions", {{"workspace", (root / "outside").string()}}, &status);
        expect(status == 403, "a workspace outside the allowed roots is refused with 403");
        api.post("/api/sessions", {{"workspace", (root / "ws" / "missing").string()}}, &status);
        expect(status == 400, "a workspace that does not exist is a 400");
        api.post("/api/sessions", {{"mode", "bogus"}}, &status);
        expect(status == 400, "an unknown mode is a 400");
        api.get("/api/sessions/nope", &status);
        expect(status == 404, "an unknown session is a 404");
        json list = api.get("/api/sessions");
        expect(list["sessions"].size() == 1 && list["sessions"][0]["id"] == id, "the list has the one session");

        auto events = api.stream("POST", "/api/sessions/" + id + "/messages", {{"text", "hello there"}});
        expect(!events.empty() && events.front()["type"] == "maid.input.added" && events.front()["item"]["content"][0]["text"] == "hello there" &&
                   events.front()["by"]["origin"] == "remote",
               "the stream starts with the input, from a remote client");
        expect(text_of(events) == "echo: hello there", "text deltas stream in order, as response.output_text.delta");
        const json* done = find_event(events, "response.completed");
        const json* usage = find_event(events, "maid.usage.updated");
        expect(done && (*done)["response"]["maid"]["final"] == true && usage && (*usage)["calls"] == 1, "response.completed ends the turn, with usage");
        expect(events.back()["type"] == "maid.session.state" && events.back()["activity"] == "idle", "the stream ends when the session is idle");
        long first = events.front()["sequence_number"], last = events.back()["sequence_number"];
        expect(last - first + 1 == static_cast<long>(events.size()), "every event has its sequence_number, one more each time");
        json s2 = api.get("/api/sessions/" + id);
        expect(s2["running"] == false && s2["turns"] == 1 && s2["entries"].size() == 2 && s2["entries"][1]["text"] == "echo: hello there" && s2["sequence_number"] == last,
               "the transcript folds the reply into one entry, and the session says where its stream is");
        auto replay = api.stream("GET", "/api/sessions/" + id + "/events?starting_after=" + std::to_string(first - 1));
        expect(replay.size() == events.size() && replay.back() == events.back(), "the stream replays from any point (starting_after)");
        auto older = api.stream("GET", "/api/sessions/" + id + "/events?after=" + std::to_string(first));
        expect(older.size() == events.size(), "after=N, the older spelling, still means from N on");
        auto tail = api.stream("GET", "/api/sessions/" + id + "/events?starting_after=" + std::to_string(last - 1));
        expect(tail.size() == 1 && tail[0] == events.back(), "starting_after=N skips what was seen");
        auto none = api.stream("GET", "/api/sessions/" + id + "/events?starting_after=" + std::to_string(last));
        expect(none.empty(), "and an idle session with nothing new answers at once");
        api.post("/api/sessions/" + id + "/messages", {{"text", ""}}, &status);
        expect(status == 400, "an empty message is a 400");
        json origin_check = fake.last_request();
        expect(origin_check["messages"][0]["content"].get<std::string>().find("inside MAID") != std::string::npos, "the model gets the normal briefing");
    }

    section("approval round trip: a remote request is asked even in edit mode");
    {
        {
            std::lock_guard lock(fake.mu);
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", "note.txt"}, {"content", "hello"}}}};
            fake.calls_left = 1;
        }
        int refused = 0;
        api.post("/api/sessions", {{"mode", "auto"}}, &refused);
        expect(refused == 403, "a remote client cannot create a session in auto without a step-up");
        json s = api.post("/api/sessions", {{"mode", "edit"}});  // edit already applies a local write unasked
        std::string sid = s["id"];
        std::vector<json> events;
        std::thread streaming([&] { events = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "make a note"}}); });
        json approval = wait_for_approval(api, sid);
        expect(approval.is_object() && approval["tool"] == "write_file" && approval["origin"] == "remote", "edit mode still asks, because the origin is remote");
        expect(approval["preview"].get<std::string>().find("new file") != std::string::npos, "the request carries the write preview");
        int status = 0;
        api.post("/api/sessions/" + sid + "/approvals/wrong", {{"choice", "yes"}}, &status);
        expect(status == 404, "an unknown approval id is a 404");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "maybe"}}, &status);
        expect(status == 400, "a choice must be yes, no, always or trip");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "always"}}, &status);
        expect(status == 403, "a remote client cannot answer always");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "yes"}}, &status);
        expect(status == 200, "yes is accepted");
        streaming.join();
        const json* answered = find_event(events, "maid.approval.answered");
        const json* result = tool_result(events);
        expect(find_event(events, "maid.approval.requested") && answered && (*answered)["choice"] == "yes", "the stream shows the request and the answer");
        expect(result && (*result)["item"]["maid"]["ok"] == true && slurp(root / "ws" / "note.txt") == "hello", "the approved write happened");
        expect(api.get("/api/sessions/" + sid)["pending_approval"].is_null(), "nothing is pending afterwards");
        std::string transcript = slurp(find_session(sid)->path);
        expect(transcript.find("\"origin\":\"remote\"") != std::string::npos && transcript.find("\"approval\":\"yes\"") != std::string::npos,
               "the session file records the remote origin and the approval");
    }

    section("denial with a reason");
    {
        {
            std::lock_guard lock(fake.mu);
            fake.calls_left = 1;
        }
        json s = api.post("/api/sessions", json::object());
        std::string sid = s["id"];
        std::vector<json> events;
        std::thread streaming([&] { events = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "make a note"}}); });
        json approval = wait_for_approval(api, sid);
        expect(approval.is_object(), "manual mode asks");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "no"}, {"feedback", "use the docs folder"}});
        streaming.join();
        const json* result = tool_result(events);
        expect(result && (*result)["item"]["maid"]["ok"] == false && (*result)["item"]["output"].get<std::string>().find("use the docs folder") != std::string::npos,
               "the reason reaches the model as the tool result");
        bool fed_back = false;
        json last = fake.last_request();
        for (const auto& m : last["messages"]) {
            if (m["role"] == "tool" && m["content"].get<std::string>().find("use the docs folder") != std::string::npos) fed_back = true;
        }
        expect(fed_back, "the next model call carries it");
    }

    section("tool output reaches the client while it runs, at most 64 KiB/s");
    {
        {
            std::lock_guard lock(fake.mu);
            fake.tool_call = json{{"name", "run_shell"}, {"arguments", {{"command", "for i in 1 2 3; do echo tick$i; sleep 0.2; done; head -c 1048576 /dev/zero | tr '\\0' x"}}}};
            fake.calls_left = 1;
        }
        json s = api.post("/api/sessions", json::object());
        std::string sid = s["id"];
        std::vector<json> events;
        std::thread streaming([&] { events = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "run it"}}); });
        json approval = wait_for_approval(api, sid);
        auto t0 = std::chrono::steady_clock::now();
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "yes"}});
        streaming.join();
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string data;
        size_t skipped = 0, next = 0, result_at = 0, last_delta = 0;
        bool shaped = true, ordered = true;
        for (size_t i = 0; i < events.size(); ++i) {
            const json& e = events[i];
            if (e["type"] == "response.output_item.done" && e["item"]["type"] == "shell_call_output") result_at = i;
            if (e["type"] != "response.shell_call_output_content.delta") continue;
            last_delta = i;
            shaped = shaped && e["item_id"].get<std::string>().rfind("~", 0) == 0 && e["output_index"].is_number() && e["delta"]["stdout"].is_string() && e["delta"]["stderr"] == "" && e["maid"]["offset"].is_number();
            size_t offset = e["maid"]["offset"];
            ordered = ordered && offset >= next;
            if (e["maid"].contains("skipped")) {
                skipped += e["maid"]["skipped"].get<size_t>();
                next = offset + e["maid"]["skipped"].get<size_t>();
                shaped = shaped && e["delta"]["stdout"] == "";
            } else {
                data += e["delta"]["stdout"].get<std::string>();
                next = offset + e["delta"]["stdout"].get<std::string>().size();
            }
        }
        expect(shaped && ordered, "each chunk is a response.shell_call_output_content.delta with item_id, output_index, delta {stdout, stderr} and maid.offset, in order");
        expect(data.rfind("tick1\ntick2\ntick3\n", 0) == 0, "the ticks arrive as the command prints them");
        expect(data.size() + skipped == 18 + 1048576, "sent and skipped bytes add up to the whole output: " + std::to_string(data.size()) + " + " + std::to_string(skipped));
        expect(skipped > 0 && data.size() <= 64 * 1024 * (1 + seconds) + 16 * 1024,
               "the session's budget held: " + std::to_string(data.size()) + " bytes sent in " + std::to_string(seconds) + " s, the rest as skips");
        expect(result_at > last_delta && events[result_at]["item"]["output"][0]["stdout"].get<std::string>().rfind("exit code 0\ntick1", 0) == 0 &&
                   events[result_at]["item"]["output"][0]["outcome"]["exit_code"] == 0,
               "the output item follows the output, with the exit code");
        json entries = api.get("/api/sessions/" + sid)["entries"];
        bool folded = true;
        for (const auto& e : entries) folded = folded && e["type"] != "response.shell_call_output_content.delta";
        expect(folded, "the folded transcript leaves the deltas out");
        const json& kept = events[result_at]["item"]["maid"];
        expect(kept.contains("full_output") && kept["full_output"]["session"] == sid && kept["full_output"]["call"] == "call_1" &&
                   kept["full_output"]["bytes"] == 18 + 1048576 && kept["full_output"]["label"] == "full output, display only: the model saw the capped result",
               "the output item says the whole output was kept, labelled display only");
        int status = 0;
        json page = api.get("/api/sessions/" + sid + "/output/call_1?session=" + sid, &status);
        expect(status == 200 && page["label"] == kept["full_output"]["label"] && page["bytes"] == 18 + 1048576 && page["data"].get<std::string>().size() == 256 * 1024 &&
                   page["data"].get<std::string>().rfind("tick1\ntick2\ntick3\nxxx", 0) == 0 && page["done"] == false,
               "a client fetches it 256 KiB at a time, with the label");
        page = api.get("/api/sessions/" + sid + "/output/call_1?offset=1048576", &status);
        expect(status == 200 && page["data"] == std::string(18, 'x') && page["done"] == true, "to the end");
        api.get("/api/sessions/" + sid + "/output/call_1?session=elsewhere", &status);
        expect(status == 404, "only from the session itself or its subagents");
    }

    section("interrupt");
    {
        {
            std::lock_guard lock(fake.mu);
            fake.delay_ms = 200;
        }
        json s = api.post("/api/sessions", json::object());
        std::string sid = s["id"];
        int status = 0;
        json idle = api.post("/api/sessions/" + sid + "/interrupt", json::object(), &status);
        expect(status == 200 && idle["running"] == false, "interrupting an idle session is a no-op");
        std::vector<json> events;
        auto t0 = std::chrono::steady_clock::now();
        std::thread streaming([&] { events = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "a long slow reply please"}}); });
        std::this_thread::sleep_for(300ms);
        json r = api.post("/api/sessions/" + sid + "/interrupt", json::object(), &status);
        expect(status == 200 && r["interrupting"] == true, "interrupt reaches a running turn");
        streaming.join();
        const json* done = find_event(events, "maid.response.cancelled");
        expect(done && (*done)["response"]["status"] == "cancelled" && std::chrono::steady_clock::now() - t0 < 3s, "the turn stops quickly and says so");
        expect(api.get("/api/sessions/" + sid)["running"] == false, "the session is idle again");
        {
            std::lock_guard lock(fake.mu);
            fake.delay_ms = 1;
        }

        // An interrupt while an approval waits answers it with a denial.
        {
            std::lock_guard lock(fake.mu);
            fake.calls_left = 1;
        }
        std::vector<json> events2;
        std::thread streaming2([&] { events2 = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "note"}}); });
        json approval = wait_for_approval(api, sid);
        expect(approval.is_object(), "an approval is waiting");
        api.post("/api/sessions/" + sid + "/interrupt", json::object());
        streaming2.join();
        const json* answered = find_event(events2, "maid.approval.answered");
        expect(answered && (*answered)["choice"] == "no" && find_event(events2, "maid.response.cancelled"), "interrupt denies the pending approval and ends the turn");
    }

    section("mode and status");
    {
        int status = 0;
        json s = api.post("/api/sessions/" + id + "/mode", {{"mode", "plan"}}, &status);
        expect(status == 200 && s["mode"] == "plan", "the mode changes per session");
        api.post("/api/sessions/" + id + "/mode", {{"mode", "root"}}, &status);
        expect(status == 400, "an unknown mode is refused");
        json st = api.get("/api/status");
        expect(st["sessions"] == 5 && st["workspaces"][0] == fs::weakly_canonical(root / "ws").string() && st["listen"] == "127.0.0.1:" + std::to_string(port),
               "status counts sessions and shows the roots");
        expect(st["remote_model"] == false && st["harness"].contains("tripped"), "status says whether the model is remote and the harness state");
    }

    section("tls");
    {
        server::TlsPair pair = server::ensure_self_signed(root / "tls" / "cert.pem", root / "tls" / "key.pem", {"127.0.0.1", "maid-test"});
        expect(fs::exists(pair.cert) && fs::exists(pair.key) && pair.fingerprint.size() == 95, "a self-signed pair is generated: " + pair.fingerprint);
        struct stat st {};
        expect(stat(pair.key.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the private key is 0600");
        expect(server::ensure_self_signed(pair.cert, pair.key, {}).fingerprint == pair.fingerprint && server::cert_fingerprint(pair.cert) == pair.fingerprint,
               "an existing pair is reused, not replaced");
        server::ServerOptions t = o;
        t.settings.server.cert = pair.cert;
        t.settings.server.key = pair.key;
        server::Server secure(t);
        int tls_port = secure.bind();
        std::thread serving_tls([&] { secure.run(); });
        expect(secure.tls() && secure.fingerprint() == pair.fingerprint, "a configured pair turns TLS on, even on loopback");
        httplib::Client plain("http://127.0.0.1:" + std::to_string(tls_port));
        plain.set_connection_timeout(2);
        plain.set_read_timeout(2);
        auto no_tls = plain.Get("/api/status", api.auth());
        expect(!no_tls || no_tls->status != 200, "plain HTTP gets nothing from a TLS server");
        httplib::SSLClient ssl("127.0.0.1", tls_port);
        ssl.set_ca_cert_path(pair.cert.string());
        ssl.enable_server_certificate_verification(true);
        auto res = ssl.Get("/api/status", api.auth());
        expect(res && res->status == 200 && json::parse(res->body)["tls"] == true, "an HTTPS client that pins the certificate gets through");
        secure.stop();
        serving_tls.join();
    }

    section("trust from a remote device needs step-up");
    {
        fs::path dir = root / "ws" / "remote-proj";
        fs::create_directories(dir);
        std::ofstream(dir / "MAID.md") << "rules\n";
        int status = 0;
        json r = api.post("/api/trust", {{"path", dir.string()}, {"action", "trust"}}, &status);
        std::string unavailable = "step-up verification is not available until accounts land (docs/design/accounts.md)";
        expect(status == 403 && r.value("error", "") == unavailable && !trusted(dir), "with no verifier a request is refused");
        r = api.post("/api/trust", {{"path", dir.string()}, {"action", "trust"}, {"step_up", "123456"}}, &status);
        expect(status == 403 && r.value("error", "") == unavailable && !trusted(dir), "a proof changes nothing until accounts land");
        set_step_up_verifier([](const std::string& device, const std::string& proof) { return device == "phone" && proof == "123456"; });
        r = api.post("/api/trust", {{"path", dir.string()}, {"action", "trust"}, {"step_up", "999999"}}, &status);
        expect(status == 403 && r.value("error", "") == "step-up verification failed" && !trusted(dir), "a wrong proof is refused");
        r = api.post("/api/trust", {{"path", dir.string()}, {"action", "trust"}}, &status);
        expect(status == 403 && r.value("error", "").find("a step_up proof is required") != std::string::npos, "no proof is refused");
        r = api.post("/api/trust", {{"path", dir.string()}, {"action", "trust"}, {"level", "relaxed"}, {"lua", "sandbox"}, {"step_up", "123456"}}, &status);
        expect(status == 200 && r.value("done", "") == "trusted " + dir.string() + " (Lua sandbox)" && trusted(dir) && trust_status(project_dir(dir)).level == "relaxed" &&
                   trust_lua_tier(dir) == LuaTier::Sandbox,
               "a verified device trusts it, with a tier and a Lua level");

        r = api.post("/api/trust", {{"path", dir.string()}, {"action", "untrust"}, {"step_up", "123456"}}, &status);
        expect(status == 200 && !trusted(dir), "and can take it back");
        std::string audit = slurp(trust_audit_path());
        expect(audit.find("device=phone action=trust path=" + dir.string() + " refused: " + unavailable) != std::string::npos &&
                   audit.find("device=phone action=trust path=" + dir.string() + " level=relaxed lua=sandbox done") != std::string::npos &&

                   audit.find("device=phone action=untrust path=" + dir.string() + " done") != std::string::npos,
               "each request is an audit line naming the device");
        set_step_up_verifier({});
    }

    section("artifacts");
    {
        fs::path src = root / "src-demo";
        fs::create_directories(src / "sub");
        fs::create_directories(src / "data");
        fs::create_directories(src / ".git");
        std::ofstream(src / "index.html") << "<!doctype html><html><HEAD><title>demo</title></head><body><script src=\"app.js\"></script></body></html>\n";
        std::ofstream(src / "app.js") << "console.log(1)\n";
        std::ofstream(src / "style.css") << "body{}\n";
        std::ofstream(src / "img.png") << "\x89PNG\r\n";
        std::ofstream(src / "sub" / "page.txt") << "text\n";
        std::ofstream(src / "data" / "seed.json") << "{\"seed\":1}";
        std::ofstream(src / ".env") << "SECRET=1\n";
        std::ofstream(src / ".git" / "config") << "x\n";
        fs::create_symlink("/etc/hostname", src / "link.js");
        fs::path arts = root / "artifacts";
        auto skipped = server::add_artifact(arts, src, "demo");
        expect(skipped.size() == 1 && skipped[0].find("link.js") != std::string::npos && !fs::exists(arts / "demo" / "link.js"), "add skips a symlink and says so");
        expect(!fs::exists(arts / "demo" / ".env") && !fs::exists(arts / "demo" / ".git") && fs::exists(arts / "demo" / "sub" / "page.txt"), "and dotfiles, copying the rest");
        std::ofstream(src / "app.js") << "console.log(2)\n";
        std::ofstream(src / "data" / "seed.json") << "{\"seed\":2}";
        server::add_artifact(arts, src, "demo");
        expect(slurp(arts / "demo" / "app.js") == "console.log(2)\n" && slurp(arts / "demo" / "data" / "seed.json") == "{\"seed\":1}", "adding again updates the page and keeps its data");
        server::add_artifact(arts, src, "other");
        bool threw = false;
        try {
            server::add_artifact(arts, src, "../evil");
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw && !fs::exists(root / "evil"), "an id outside the safe charset is refused");
        auto list = server::list_artifacts(arts);
        expect(list.size() == 2 && list[0].id == "demo" && list[0].trust == "sandboxed" && list[0].data == std::vector<std::string>{"seed"}, "list shows each artifact, sandboxed, with its data");
        std::ofstream(arts / "other" / ".maid-artifact.json") << "{\"trust\": \"Trusted\"}";
        expect(server::artifact_trust(arts / "other") == "sandboxed", "an unknown trust value counts as sandboxed");
        std::ofstream(arts / "other" / ".maid-artifact.json") << "{\"trust\": \"trusted\"}";
        expect(server::artifact_trust(arts / "other") == "trusted", "the trusted flag is recorded (and loosens nothing)");
        expect(!server::artifact_allow_insecure(arts / "demo") && !list[0].allow_insecure, "ALLOW_INSECURE is off by default");
        server::set_artifact_allow_insecure(arts / "other", true);
        expect(server::artifact_allow_insecure(arts / "other") && server::artifact_trust(arts / "other") == "trusted", "turning it on is read back and keeps the trust record");
        server::set_artifact_allow_insecure(arts / "other", false);
        expect(!server::artifact_allow_insecure(arts / "other") && server::artifact_trust(arts / "other") == "trusted", "turning it off is read back");
        for (std::string junk : {"\"true\"", "1", "\"yes\"", "null", "[true]", "{}"}) {
            std::ofstream(arts / "other" / ".maid-artifact.json") << "{\"trust\": \"trusted\", \"ALLOW_INSECURE\": " << junk << "}";
            expect(!server::artifact_allow_insecure(arts / "other"), "a junk ALLOW_INSECURE value counts as off: " + junk);
        }
        std::ofstream(arts / "other" / ".maid-artifact.json") << "not json";
        expect(!server::artifact_allow_insecure(arts / "other"), "an unreadable record counts as off");
        std::ofstream(arts / "other" / ".maid-artifact.json") << "{\"trust\": \"trusted\"}";
        // Planted after the copy: what add never makes, the server must still refuse.
        std::ofstream(root / "outside" / "secret.txt") << "secret\n";
        std::ofstream(arts / "demo" / ".hidden") << "hidden\n";
        fs::create_symlink(root / "outside" / "secret.txt", arts / "demo" / "escape.js");
        fs::create_symlink("app.js", arts / "demo" / "inner.js");
        fs::create_symlink(".hidden", arts / "demo" / "dot.js");

        httplib::Client c(api.base);
        std::string origin = api.base;
        auto sandboxed = [](const httplib::Result& r) {
            return r && r->get_header_value("Content-Security-Policy").rfind("sandbox", 0) == 0 && r->get_header_value("X-Content-Type-Options") == "nosniff";
        };
        auto bearer = api.auth();
        // A person's request succeeds, which clears the rate limit's count between the refusals below.
        auto reopen = [&]() {
            auto r = c.Get("/a/demo/", bearer);
            std::string loc = r ? r->get_header_value("Location") : "";
            return loc.size() > 8 && loc.rfind("/a/demo~", 0) == 0 ? loc.substr(8, loc.size() - 9) : std::string();
        };

        auto r = c.Get("/a/demo/");
        expect(r && r->status == 401 && sandboxed(r), "no login: 401, and the refusal is sandboxed too");
        std::string cap = reopen();
        expect(cap.size() == 32, "a bearer token opens it: a redirect to a fresh capability");
        std::string page = "/a/demo~" + cap + "/";
        r = c.Get(page);
        std::string csp = r ? r->get_header_value("Content-Security-Policy") : "";
        expect(r && r->status == 200 && r->get_header_value("Content-Type") == "text/html; charset=utf-8" &&
                   r->body.find("<HEAD><meta name=\"maid-artifact-token\" content=\"" + cap + "\"><title>") != std::string::npos,
               "the page is served under the capability, carrying it in a meta tag first in its head");
        expect(csp.rfind("sandbox allow-scripts allow-forms allow-modals allow-downloads;", 0) == 0 && csp.find("allow-same-origin") == std::string::npos &&
                   csp.find("default-src 'none'") != std::string::npos && csp.find("script-src " + origin + page + " " + origin + "/a/_vendor/;") != std::string::npos &&
                   csp.find("connect-src " + origin + page + "data/;") != std::string::npos && csp.find("frame-ancestors 'none'") != std::string::npos &&
                   csp.find("'unsafe-eval'") == std::string::npos,
               "its policy: sandboxed with an opaque origin, scripts from its own files and /a/_vendor/, network to its data only: " + csp);
        expect(r && r->get_header_value("X-Frame-Options") == "DENY" && r->get_header_value("Referrer-Policy") == "no-referrer" &&
                   r->get_header_value("Access-Control-Allow-Origin") == "*",
               "no framing, no referrer, and CORS for the page's own opaque origin");
        {
            // ALLOW_INSECURE: only that artifact's script-src gains 'unsafe-eval'; the rest of its policy is the same, and loading its page is audited.
            auto script_src = [](const std::string& p) {
                size_t at = p.find("script-src ");
                return p.substr(at, p.find(';', at) - at);
            };
            auto without_eval = [](std::string p) {
                if (size_t at = p.find(" 'unsafe-eval'"); at != std::string::npos) p.erase(at, 14);
                return p;
            };
            auto open_cap = [&](const std::string& id) {
                auto o = c.Get("/a/" + id + "/", bearer);
                std::string loc = o ? o->get_header_value("Location") : "";
                return loc.size() > id.size() + 5 ? loc.substr(id.size() + 4, loc.size() - id.size() - 5) : std::string();
            };
            auto audit_text = [&]() { return slurp(state / "audit.log"); };
            expect(audit_text().find("ALLOW_INSECURE") == std::string::npos, "no ALLOW_INSECURE line in the audit log while it is off");
            server::set_artifact_allow_insecure(arts / "demo", true);
            r = c.Get(page);
            std::string on = r ? r->get_header_value("Content-Security-Policy") : "";
            expect(r && r->status == 200 && script_src(on) == script_src(csp) + " 'unsafe-eval'" && without_eval(on) == csp,
                   "ALLOW_INSECURE adds 'unsafe-eval' to script-src and changes nothing else: " + on);
            std::string other_cap = open_cap("other");
            auto ro = c.Get("/a/other~" + other_cap + "/");
            expect(ro && ro->status == 200 && ro->get_header_value("Content-Security-Policy").find("'unsafe-eval'") == std::string::npos,
                   "another artifact is not affected");
            auto rjs = c.Get(page + "app.js");
            expect(rjs && rjs->get_header_value("Content-Security-Policy").find("'unsafe-eval'") != std::string::npos, "nor are the artifact's other files left out");
            std::string log = audit_text();
            size_t first = log.find("ALLOW_INSECURE");
            expect(first != std::string::npos && log.find("ALLOW_INSECURE", first + 1) == std::string::npos && log.find("/a/demo/ index.html", first) != std::string::npos &&
                       log.find(cap) == std::string::npos,
                   "each load of its index.html writes one ALLOW_INSECURE audit line, without the capability");
            server::set_artifact_allow_insecure(arts / "demo", false);
            r = c.Get(page);
            expect(r && r->get_header_value("Content-Security-Policy") == csp, "turned off, the policy is the strict one again");
            std::ofstream(arts / "demo" / ".maid-artifact.json") << "{\"trust\": \"sandboxed\", \"ALLOW_INSECURE\": \"true\"}";
            r = c.Get(page);
            expect(r && r->get_header_value("Content-Security-Policy") == csp, "a junk ALLOW_INSECURE value served as off: no 'unsafe-eval'");
        }
        bool types = true;
        for (auto [f, t] : std::vector<std::pair<std::string, std::string>>{{"app.js", "text/javascript; charset=utf-8"}, {"style.css", "text/css; charset=utf-8"},
                                                                            {"img.png", "image/png"}, {"sub/page.txt", "text/plain; charset=utf-8"}, {"inner.js", "text/javascript; charset=utf-8"}}) {
            auto f_r = c.Get(page + f);
            types = types && f_r && f_r->status == 200 && f_r->get_header_value("Content-Type") == t && sandboxed(f_r);
        }
        expect(types, "every file has its content type and the sandbox, a symlink inside the folder included");
        bool contained = true;
        for (std::string bad : {"../other/index.html", "%2e%2e/%2e%2e/outside/secret.txt", "sub/../app.js", ".hidden", "escape.js", "dot.js", "sub/", "sub", "data/../app.js",
                                 ".maid-artifact.json", "a//b"}) {
            auto b = c.Get(page + bad);
            contained = contained && b && b->status == 404 && sandboxed(b) && b->body.find("secret\n") == std::string::npos;
        }
        expect(contained, "traversal, dotfiles, a symlink out of the folder or to a dotfile, and folders: 404, sandboxed");
        r = c.Get("/a/_vendor/vue/vue.global.prod.js");
        expect(r && r->status == 200 && r->get_header_value("Content-Type") == "text/javascript; charset=utf-8" && sandboxed(r) &&
                   r->get_header_value("Access-Control-Allow-Origin") == "*" && r->body.find("Vue") != std::string::npos,
               "the vendored Vue needs no login");
        r = c.Get("/a/_vendor/vue/%2e%2e/manifest.json");
        expect(r && r->status >= 400 && sandboxed(r), "and nothing beside it");
        r = c.Get("/a/demo");
        expect(r && r->status == 301 && r->get_header_value("Location") == "/a/demo/" && sandboxed(r), "/a/ID is sent on to /a/ID/, sandboxed");
        r = c.Get("/a/nope/", bearer);
        auto r2 = c.Get("/a/");
        expect(r && r->status == 404 && sandboxed(r) && r2 && r2->status == 404 && sandboxed(r2), "an unknown artifact, and /a/ itself, are sandboxed 404s");

        section("artifact capabilities");
        r = c.Get("/a/other~" + cap + "/");
        expect(r && r->status == 303 && r->get_header_value("Location") == "/a/other/", "demo's capability does not open other: its page is sent to log in");
        r = c.Get("/a/other~" + cap + "/app.js");
        r2 = c.Get("/a/other~" + cap + "/data/seed.json");
        expect(r && r->status == 401 && r2 && r2->status == 401 && sandboxed(r2) && !r2->has_header("Access-Control-Allow-Origin"), "nor its files or data");
        r = c.Get(page + "data/seed.json", {{"X-Maid-Artifact-Token", "not-" + cap.substr(4)}});
        expect(r && r->status == 401, "a header token that is not the path's capability is refused");
        reopen();
        r = c.Get("/api/status", {{"Authorization", "Bearer " + cap}});
        expect(r && r->status == 401, "a capability is no bearer token for the session API");
        r = c.Get("/a/demo/", {{"Authorization", "Bearer " + cap}});
        expect(r && r->status == 401, "nor for an artifact's login");
        reopen();
        r = c.Get("/api/status", {{"Authorization", "Bearer " + token}, {"X-Maid-Artifact-Token", cap}});
        expect(r && r->status == 403, "the session API refuses a request carrying an artifact token, even with a bearer token");
        r = c.Get("/api/status", {{"Authorization", "Bearer " + token}, {"Origin", "null"}});
        r2 = c.Get("/", {{"Origin", "null"}});
        expect(r && r->status == 403 && r2 && r2->status == 403, "and any request with Origin null, a sandboxed page's");

        section("artifact data");
        httplib::Headers h = {{"X-Maid-Artifact-Token", cap}};
        auto with = [&](httplib::Headers more) {
            more.insert(h.begin(), h.end());
            return more;
        };
        std::string answers = page + "data/answers.json";
        r = c.Options(answers, {{"Origin", "null"}, {"Access-Control-Request-Method", "PUT"}, {"Access-Control-Request-Headers", "x-maid-artifact-token,if-match"}});
        expect(r && r->status == 204 && r->get_header_value("Access-Control-Allow-Origin") == "*" && r->get_header_value("Access-Control-Allow-Methods") == "GET, PUT" &&
                   r->get_header_value("Access-Control-Allow-Headers").find("X-Maid-Artifact-Token") != std::string::npos,
               "the preflight is answered for the data route");
        // The sequence the comfymaid-review page runs (templates/comfymaid-review/local/app.js).
        r = c.Get(answers, h);
        expect(r && r->status == 404 && r->get_header_value("Access-Control-Allow-Origin") == "*" && sandboxed(r), "nothing stored yet: a 404 the page can read");
        r = c.Put(answers, with({{"If-None-Match", "*"}}), "{\"answers\":{\"a\":1}}", "application/json");
        std::string e1 = r ? r->get_header_value("ETag") : "";
        expect(r && r->status == 201 && e1 == "\"" + server::data_rev("{\"answers\":{\"a\":1}}") + "\"" && r->get_header_value("X-Rev") == e1.substr(1, 16) &&
                   r->get_header_value("Access-Control-Expose-Headers") == "ETag, X-Rev",
               "the first write creates it, with If-None-Match: *, and returns its ETag");
        r = c.Get(answers, h);
        expect(r && r->status == 200 && r->body == "{\"answers\":{\"a\":1}}" && r->get_header_value("ETag") == e1 && r->get_header_value("Content-Type") == "application/json",
               "a read returns it and the same ETag");
        r = c.Put(answers, with({{"If-Match", e1}}), "{\"answers\":{\"a\":2}}", "application/json");
        std::string e2 = r ? r->get_header_value("ETag") : "";
        expect(r && r->status == 200 && !e2.empty() && e2 != e1 && json::parse(r->body)["rev"] == e2.substr(1, 16), "a write with the ETag it read succeeds and returns the new one");
        r = c.Put(answers, with({{"If-Match", e1}}), "{\"answers\":{\"a\":3}}", "application/json");
        expect(r && r->status == 409 && r->get_header_value("ETag") == e2 && json::parse(r->body)["rev"] == e2.substr(1, 16) &&
                   r->get_header_value("Access-Control-Allow-Origin") == "*",
               "a stale write is a 409 naming the current revision");
        r = c.Put(answers, with({{"If-None-Match", "*"}}), "{}", "application/json");
        expect(r && r->status == 409, "creating what exists is a 409 too");
        expect(slurp(arts / "demo" / "data" / "answers.json") == "{\"answers\":{\"a\":2}}", "the file on disk is the last accepted write");
        std::ofstream(arts / "demo" / "data" / "answers.json") << "{\"answers\":{\"by\":\"an agent\"}}";
        r = c.Put(answers, with({{"If-Match", e2.substr(1, 16)}}), "{\"answers\":{\"a\":4}}", "application/json");
        expect(r && r->status == 409, "an agent's edit on disk changes the revision, so the page's next write is stale");
        r = c.Put(answers, h, "{}", "application/json");
        expect(r && r->status == 428, "a write without If-Match or If-None-Match is refused");
        r = c.Put(page + "data/fresh.json", with({{"If-None-Match", "*"}}), "{not json", "application/json");
        expect(r && r->status == 400 && !fs::exists(arts / "demo" / "data" / "fresh.json"), "a body that is not JSON is refused");
        r = c.Put(page + "data/big.json", with({{"If-None-Match", "*"}}), "\"" + std::string(server::kArtifactDataMax, 'x') + "\"", "application/json");
        expect(r && r->status == 413 && !fs::exists(arts / "demo" / "data" / "big.json"), "a document over 1 MiB is a 413");
        bool names = true;
        for (std::string bad : {"data/a.b.json", "data/.x.json", "data/x.txt", "data/_x.json"}) {
            auto b = c.Put(page + bad, with({{"If-None-Match", "*"}}), "{}", "application/json");
            names = names && b && b->status == 404;
        }
        expect(names, "data names outside the safe charset are refused");
        bool tidy = true;
        for (const auto& e : fs::directory_iterator(arts / "demo" / "data")) tidy = tidy && e.path().filename().string()[0] != '.';
        expect(tidy, "no temporary file is left beside the documents");
        r = c.Get("/a/demo/data/answers.json", bearer);
        expect(r && r->status == 200 && r->get_header_value("ETag") == "\"" + server::data_rev(r->body) + "\"", "a bearer token reads the data too (a device, or through the relay)");

        section("artifact logins");
        std::string code = server::new_artifact_login(state);
        r = c.Get("/a/_login?code=" + code + "&to=demo");
        std::string set = r ? r->get_header_value("Set-Cookie") : "";
        std::string login = set.substr(set.find('=') + 1, 32);
        expect(r && r->status == 303 && r->get_header_value("Location").rfind("/a/demo~", 0) == 0 && sandboxed(r), "the one-time link goes straight to a capability");
        expect(set.rfind("maid_artifacts=", 0) == 0 && set.find("; Path=/a/; HttpOnly; SameSite=Strict") != std::string::npos, "and sets the login cookie, HttpOnly, SameSite=Strict, for /a/ only");
        r = c.Get("/a/_login?code=" + code + "&to=demo");
        expect(r && r->status == 401 && sandboxed(r), "the link works once");
        httplib::Headers cookie = {{"Cookie", "theme=dark; maid_artifacts=" + login}};
        r = c.Get("/a/demo/", cookie);
        expect(r && r->status == 303 && r->get_header_value("Location").rfind("/a/demo~", 0) == 0, "the cookie opens an artifact later");
        r = c.Get("/a/demo/", {{"Cookie", "maid_artifacts=" + login}, {"Origin", "null"}});
        r2 = c.Get("/a/demo/", {{"Cookie", "maid_artifacts=" + login}, {"Sec-Fetch-Site", "cross-site"}});
        expect(r && r->status == 401 && r2 && r2->status == 401, "but not from a sandboxed page or another site");
        r = c.Get("/api/status", cookie);
        expect(r && r->status == 401, "and the session API never takes it");
        c.Get("/a/demo/", cookie);
        r = c.Get("/a/demo/", {{"Cookie", "maid_artifacts=" + cap}});
        r2 = c.Get("/a/demo~" + login + "/");
        expect(r && r->status == 401 && r2 && r2->status == 303 && r2->get_header_value("Location") == "/a/demo/", "a capability is no login, and a login no capability");
        c.Get("/a/demo/", cookie);
        r = c.Get("/a/_login?code=" + server::new_artifact_login(state, -1) + "&to=demo");
        expect(r && r->status == 401, "an expired link is refused");
        c.Get("/a/demo/", cookie);
        std::string audit = slurp(state / "audit.log");
        expect(audit.find("GET /a/demo~*/data/answers.json 200") != std::string::npos && audit.find(cap) == std::string::npos && audit.find(code) == std::string::npos,
               "the audit log names the route but never a capability or a login code");
    }

    section("no remote unlock");
    {
        int status = 0;
        api.post("/api/unlock", json::object(), &status);
        expect(status == 404, "there is no unlock route");
        api.post("/api/tripwire/reset", json::object(), &status);
        expect(status == 404, "nor any other reset route");
    }

    server.stop();
    serving.join();
    fs::remove_all(root);
    return finish();
}
