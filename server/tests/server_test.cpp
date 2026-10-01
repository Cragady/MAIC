// The server against a fake OpenAI-compatible server: tokens, streaming, the remote-origin approval round trip, interrupt, audit.
#include "check.hpp"

#include "auth.hpp"
#include "maic/paths.hpp"
#include "server.hpp"
#include "tls.hpp"

#include "maic/http.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace maic;
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
        if (e["type"] == "text") out += e["text"].get<std::string>();
    }
    return out;
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
    fs::path root = fs::temp_directory_path() / "maic-server-test";
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
        expect(!store.verify(token.substr(0, 31) + "x") && !store.verify(""), "a wrong or empty token does not");
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
        expect(id.find("-server-") != std::string::npos && fs::exists(s["transcript"].get<std::string>()), "it is recorded as kind server: " + id);
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
        expect(!events.empty() && events.front()["type"] == "user" && events.front()["text"] == "hello there", "the stream starts with the user message");
        expect(text_of(events) == "echo: hello there", "text deltas stream in order");
        const json* done = find_event(events, "done");
        expect(done && (*done)["usage"]["calls"] == 1 && (*done)["interrupted"] == false, "the stream ends with done and usage");
        json s2 = api.get("/api/sessions/" + id);
        expect(s2["running"] == false && s2["turns"] == 1 && s2["entries"].size() == 2 && s2["entries"][1]["text"] == "echo: hello there",
               "the transcript folds the deltas into one entry");
        auto replay = api.stream("GET", "/api/sessions/" + id + "/events?after=0");
        expect(replay.size() == events.size() && replay.back()["type"] == "done", "the event log replays from any point");
        auto tail = api.stream("GET", "/api/sessions/" + id + "/events?after=" + std::to_string(events.size() - 1));
        expect(tail.size() == 1 && tail[0]["type"] == "done", "after=N skips what was seen");
        api.post("/api/sessions/" + id + "/messages", {{"text", ""}}, &status);
        expect(status == 400, "an empty message is a 400");
        json origin_check = fake.last_request();
        expect(origin_check["messages"][0]["content"].get<std::string>().find("inside MAIC") != std::string::npos, "the model gets the normal briefing");
    }

    section("approval round trip: a remote request is asked even in auto mode");
    {
        {
            std::lock_guard lock(fake.mu);
            fake.tool_call = json{{"name", "write_file"}, {"arguments", {{"path", "note.txt"}, {"content", "hello"}}}};
            fake.calls_left = 1;
        }
        json s = api.post("/api/sessions", {{"mode", "auto"}});
        std::string sid = s["id"];
        std::vector<json> events;
        std::thread streaming([&] { events = api.stream("POST", "/api/sessions/" + sid + "/messages", {{"text", "make a note"}}); });
        json approval = wait_for_approval(api, sid);
        expect(approval.is_object() && approval["tool"] == "write_file" && approval["origin"] == "remote", "auto mode still asks, because the origin is remote");
        expect(approval["preview"].get<std::string>().find("new file") != std::string::npos, "the request carries the write preview");
        int status = 0;
        api.post("/api/sessions/" + sid + "/approvals/wrong", {{"choice", "yes"}}, &status);
        expect(status == 404, "an unknown approval id is a 404");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "maybe"}}, &status);
        expect(status == 400, "a choice must be yes, no, always or trip");
        api.post("/api/sessions/" + sid + "/approvals/" + approval["id"].get<std::string>(), {{"choice", "yes"}}, &status);
        expect(status == 200, "yes is accepted");
        streaming.join();
        const json* answered = find_event(events, "approval_answered");
        const json* result = find_event(events, "tool_result");
        expect(find_event(events, "approval") && answered && (*answered)["choice"] == "yes", "the stream shows the request and the answer");
        expect(result && (*result)["ok"] == true && slurp(root / "ws" / "note.txt") == "hello", "the approved write happened");
        expect(api.get("/api/sessions/" + sid)["pending_approval"].is_null(), "nothing is pending afterwards");
        std::string transcript = slurp(s["transcript"].get<std::string>());
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
        const json* result = find_event(events, "tool_result");
        expect(result && (*result)["ok"] == false && (*result)["text"].get<std::string>().find("use the docs folder") != std::string::npos,
               "the reason reaches the model as the tool result");
        bool fed_back = false;
        json last = fake.last_request();
        for (const auto& m : last["messages"]) {
            if (m["role"] == "tool" && m["content"].get<std::string>().find("use the docs folder") != std::string::npos) fed_back = true;
        }
        expect(fed_back, "the next model call carries it");
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
        const json* done = find_event(events, "done");
        expect(done && (*done)["interrupted"] == true && std::chrono::steady_clock::now() - t0 < 3s, "the turn stops quickly and says so");
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
        const json* answered = find_event(events2, "approval_answered");
        expect(answered && (*answered)["choice"] == "no" && find_event(events2, "done"), "interrupt denies the pending approval and ends the turn");
    }

    section("mode and status");
    {
        int status = 0;
        json s = api.post("/api/sessions/" + id + "/mode", {{"mode", "plan"}}, &status);
        expect(status == 200 && s["mode"] == "plan", "the mode changes per session");
        api.post("/api/sessions/" + id + "/mode", {{"mode", "root"}}, &status);
        expect(status == 400, "an unknown mode is refused");
        json st = api.get("/api/status");
        expect(st["sessions"] == 4 && st["workspaces"][0] == fs::weakly_canonical(root / "ws").string() && st["listen"] == "127.0.0.1:" + std::to_string(port),
               "status counts sessions and shows the roots");
        expect(st["remote_model"] == false && st["harness"].contains("tripped"), "status says whether the model is remote and the harness state");
    }

    section("tls");
    {
        server::TlsPair pair = server::ensure_self_signed(root / "tls" / "cert.pem", root / "tls" / "key.pem", {"127.0.0.1", "maic-test"});
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
