// The engine in-process, driven through its dispatcher by test clients against a fake OpenAI-compatible server
// (docs/design/engine-protocol.md section 15). Every message each client sends and gets is recorded and checked as
// it happens: its schema, the order of ordering.json (protocol::Conformance), and the OpenAI-only view with every
// maic.* event and maic object removed. The recordings are written to build/protocol-streams/ for `maic protocol
// check`, then mutated: a mutation that breaks a rule must be caught with that rule's id, one that breaks none must pass.
#include "check.hpp"
#include "fake_server.hpp"

#include "maic/engine.hpp"
#include "maic/protocol.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <random>

using namespace maic;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Recording {
    std::string name;
    std::vector<json> records;
    protocol::Conformance live, openai{true};
    std::optional<protocol::Violation> first, first_openai;
    std::mutex mu;

    explicit Recording(std::string n) : name(std::move(n)) {}

    void add(const std::string& dir, const std::string& conn, const json& msg) {
        std::lock_guard lock(mu);
        json r = {{"dir", dir}, {"conn", conn}, {"msg", msg}};
        records.push_back(r);
        if (!first) first = live.feed(r);
        if (!first_openai) first_openai = openai.feed(r);
    }

    // Checked as it ran, in both views, and kept for `maic protocol check`.
    void finish() {
        if (!first) first = live.finish();
        if (!first_openai) first_openai = openai.finish();
        expect(!first, name + ": every message fits its schema and the order (" + std::to_string(live.events()) + " events)" +
                           (first ? ": " + protocol::describe(*first) : ""));
        expect(!first_openai, name + ": the OpenAI-only view still fits OpenAI's shapes and the response machines" +
                                  (first_openai ? ": " + protocol::describe(*first_openai) : ""));
        fs::create_directories(MAIC_PROTOCOL_STREAMS);
        std::ofstream out(fs::path(MAIC_PROTOCOL_STREAMS) / (name + ".jsonl"));
        for (const auto& r : records) out << r.dump() << "\n";
    }
};

struct TestClient {
    Engine& engine;
    Recording& rec;
    std::string id;
    int next = 1;
    std::vector<json> events;  // maic.event params, in order
    std::vector<json> other;   // maic.index and maic.engine

    TestClient(Engine& e, Recording& r, Origin origin, const std::string& name) : engine(e), rec(r), id(e.connect(origin, name, "in-process")) {}

    json call(const std::string& method, json params = json::object()) {
        json msg = {{"jsonrpc", "2.0"}, {"id", next++}, {"method", method}, {"params", std::move(params)}};
        rec.add("in", id, msg);
        json reply = engine.call(id, msg);
        rec.add("out", id, reply);
        return reply;
    }
    json ok(const std::string& method, json params = json::object()) {
        json reply = call(method, std::move(params));
        if (!reply.contains("result")) {
            expect(false, method + " answers: " + reply.dump().substr(0, 300));
            return json::object();
        }
        return reply["result"];
    }
    std::string error(const std::string& method, json params) {
        json reply = call(method, std::move(params));
        return reply.contains("error") ? reply["error"]["data"].value("code", json("")).is_string() ? reply["error"]["data"]["code"].get<std::string>() : "" : "(none)";
    }
    void hello() { ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "test"}, {"version", "0"}}}, {"capabilities", {"tool_output"}}}); }
    void pump(std::chrono::milliseconds wait) {
        for (auto& m : engine.take(id, wait)) {
            rec.add("out", id, m);
            if (m.value("method", "") == "maic.event") events.push_back(m["params"]);
            else other.push_back(m);
        }
    }
    // Pumps until an event from `from` on fits `pred`; its index, or -1 after 30 s.
    long until(const std::function<bool(const json&)>& pred, size_t from = 0) {
        auto deadline = std::chrono::steady_clock::now() + 30s;
        size_t seen = from;
        while (std::chrono::steady_clock::now() < deadline) {
            for (; seen < events.size(); ++seen) {
                if (pred(events[seen])) return static_cast<long>(seen);
            }
            pump(50ms);
        }
        return -1;
    }
    long until_type(const std::string& type, size_t from = 0) {
        return until([&](const json& e) { return e["type"] == type; }, from);
    }
    long until_idle(size_t from) {
        return until([](const json& e) { return e["type"] == "maic.session.state" && e["activity"] == "idle"; }, from);
    }
    std::string text(size_t from = 0) const {
        std::string out;
        for (size_t i = from; i < events.size(); ++i) {
            if (events[i]["type"] == "response.output_text.delta") out += events[i]["delta"].get<std::string>();
        }
        return out;
    }
    const json* find(const std::string& type, size_t from = 0) const {
        for (size_t i = from; i < events.size(); ++i) {
            if (events[i]["type"] == type) return &events[i];
        }
        return nullptr;
    }
    size_t count(const std::string& type) const {
        size_t n = 0;
        for (const auto& e : events) n += e["type"] == type;
        return n;
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The first violation of `records`, as `maic protocol check` would report it ("" when they conform).
std::string verdict(const std::vector<json>& records, bool openai_only = false) {
    protocol::Conformance c(openai_only);
    for (const auto& r : records) {
        if (auto v = c.feed(r)) return v->rule;
    }
    if (auto v = c.finish()) return v->rule;
    return "";
}

// Indexes of the records that are maic.event notifications of the given type.
std::vector<size_t> events_of(const std::vector<json>& records, const std::string& type) {
    std::vector<size_t> out;
    for (size_t i = 0; i < records.size(); ++i) {
        const json& m = records[i]["msg"];
        if (m.value("method", "") == "maic.event" && m["params"]["type"] == type) out.push_back(i);
    }
    return out;
}

// Inserts an event record before `at`, numbering it and every later event of its connection one up.
std::vector<json> insert_event(std::vector<json> records, size_t at, json event) {
    std::string conn = records[at]["conn"];
    event["sequence_number"] = records[at]["msg"]["params"]["sequence_number"];
    event["stream_id"] = records[at]["msg"]["params"]["stream_id"];
    for (size_t i = at; i < records.size(); ++i) {
        json& m = records[i]["msg"];
        if (records[i]["conn"] == conn && m.value("method", "") == "maic.event") m["params"]["sequence_number"] = m["params"]["sequence_number"].get<long>() + 1;
    }
    records.insert(records.begin() + static_cast<long>(at), json{{"dir", "out"}, {"conn", conn}, {"msg", {{"jsonrpc", "2.0"}, {"method", "maic.event"}, {"params", event}}}});
    return records;
}

}  // namespace

int main() {
    fs::path root = fs::temp_directory_path() / ("maic-protocol-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "ws");
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("MAIC_TRIPWIRE_FILE", (root / "tripwire").c_str(), 1);
    fs::remove_all(MAIC_PROTOCOL_STREAMS);
    fs::path ws = fs::weakly_canonical(root / "ws");

    FakeServer fake;
    fake.unique_call_ids = true;
    EngineOptions o;
    o.settings.model = "test";
    o.settings.mode = "manual";
    o.settings.harness = "dumb";  // the rules alone judge: no reviewer calls reach the fake
    o.settings.dumb_auto_ok = true;
    o.settings.steering.clients_remote = {"steer", "drop", "further", "interrupt", "keep"};  // a remote client may not halt here
    o.settings.providers = {fake.provider()};
    o.workspaces = {ws};
    o.index_file = root / "state" / "engine" / "index.json";
    o.protocol_log = root / "state" / "engine" / "protocol.log";
    auto engine = std::make_unique<Engine>(o);
    std::mutex calls_mu;
    std::vector<json> script;  // tool calls the fake answers with, in order; then echoes
    fake.tool_call_for = [&](const json& body) -> json {
        std::lock_guard lock(calls_mu);
        // A compaction's summary is asked with no tools: the script is for the agent's own calls.
        if (script.empty() || !body.contains("tools") || body["tools"].empty()) return nullptr;
        json c = script.front();
        script.erase(script.begin());
        return c;
    };
    auto plan = [&](std::vector<json> calls) {
        std::lock_guard lock(calls_mu);
        script = std::move(calls);
    };
    auto shell = [](const std::string& command) { return json{{"name", "run_shell"}, {"arguments", {{"command", command}}}}; };

    std::vector<Recording*> recordings;

    section("hello, versioning and the dispatcher");
    Recording basics("hello");
    std::string sid;
    {
        // Requests that fail their own schema on purpose are recorded apart; that recording is not kept.
        Recording bad("bad-requests");
        TestClient a(*engine, basics, Origin::Local, "tui");
        expect(a.error("createConversation", json::object()) == "maic_hello_required", "nothing but maic.hello before maic.hello");
        json h = a.ok("maic.hello", {{"protocol", 3}, {"client", {{"name", "tui"}}}});
        TestClient x(*engine, bad, Origin::Local, "old");
        expect(x.error("maic.hello", {{"protocol", 0}, {"client", {{"name", "old"}}}}) == "maic_unsupported_protocol", "a protocol below 1 is refused");
        expect(bad.first && bad.first->rule == "schema.message", "and the checker flags the request as outside its schema");
        expect(h["protocol"] == 1 && h["origin"] == "local" && h["client"] == a.id && h["tier"] == "guarded" && h["limits"]["always"] == true &&
                   h["path"]["via"] == "in-process" && h["engine"]["instance"].get<std::string>().size() == 6,
               "the hello answers the version it speaks, the client's id and origin, the tier and the limits");
        json r = a.call("deleteConversation", {{"conversation_id", "x"}});
        expect(r["error"]["code"] == -32601, "a method the engine does not offer is -32601");
        x.ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "old"}}}});
        r = x.call("maic.session.list", {{"limit", "many"}});
        expect(r.contains("error") && slurp(o.protocol_log).find("maic.session.list params /limit") != std::string::npos,
               "a request that fails its schema is logged to protocol.log (guarded)");
        a.pump(0ms);
        bool told = false;
        for (const auto& m : a.other) told = told || (m["method"] == "maic.engine" && m["params"].value("notice", "").find("schema.message") != std::string::npos);
        expect(told, "and a local client is told once");
        json conv = a.ok("createConversation", {{"metadata", {{"title", "fennec ears"}}}, {"maic", {{"workspace", ws.string()}}}});
        sid = conv["id"];
        expect(conv["object"] == "conversation" && conv["metadata"]["title"] == "fennec ears" && conv["maic"]["entry"]["state"] == "live" &&
                   conv["maic"]["entry"]["transcript"].is_string(),
               "createConversation answers OpenAI's conversation object with the index entry in maic");
        expect(a.error("createConversation", {{"maic", {{"workspace", (root / "nowhere").string()}}}}).empty(), "a workspace that is not a directory is invalid params");
        json list = a.ok("maic.session.list", json::object());
        bool listed = false;
        for (const auto& s : list["sessions"]) listed = listed || (s["id"] == sid && s["loaded"] == true);
        expect(listed, "maic.session.list lists the transcript, loaded");
        basics.finish();
    }

    section("a plain reply");
    Recording plain("plain-reply");
    {
        TestClient a(*engine, plain, Origin::Local, "tui");
        a.hello();
        json sub = a.ok("maic.session.subscribe", {{"session", sid}});
        expect(sub["replay_from"] == 0 && sub["activity"] == "idle", "subscribing replays the load from sequence_number 0");
        size_t mark = a.events.size();
        json r = a.ok("response.create", {{"conversation", sid}, {"input", "hello there"}});
        expect(r["object"] == "response" && r["status"] == "in_progress" && r["background"] == true && r["maic"]["turn"] == 1,
               "response.create answers the response, in progress, turn 1");
        long idle = a.until_idle(mark + 1);
        expect(idle > 0, "the turn ends with the session idle");
        const json* input = a.find("maic.input.added", mark);
        expect(input && (*input)["item"]["content"][0]["text"] == "hello there" && (*input)["by"]["client"] == a.id && (*input)["queued"] == false,
               "the input is announced first, naming its client");
        expect(a.text(mark) == "echo: hello there", "the reply streams as response.output_text.delta");
        const json* done = a.find("response.completed", mark);
        expect(done && (*done)["response"]["maic"]["final"] == true && (*done)["response"]["usage"]["total_tokens"].is_number() &&
                   (*done)["response"]["output"][0]["content"][0]["text"] == "echo: hello there",
               "response.completed carries the output, usage and maic.final");
        expect(a.find("maic.usage.updated", mark) != nullptr, "and maic.usage.updated the per-call figures");
        long n = -1;
        bool contiguous = true;
        for (const auto& e : a.events) {
            contiguous = contiguous && e["sequence_number"] == n + 1 && e["stream_id"] == sid;
            n = e["sequence_number"];
        }
        expect(contiguous && a.events[0]["type"] == "maic.session.state", "sequence_number counts from 0, one more per event, on the session's stream");
        plain.finish();
    }
    recordings.push_back(&plain);

    section("a tool call with streamed output and an approval");
    Recording tool("tool-call");
    {
        TestClient a(*engine, tool, Origin::Local, "tui");
        a.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        size_t mark = a.events.size();
        plan({shell("for i in 1 2 3; do echo tick$i; sleep 0.2; done")});
        a.ok("response.create", {{"conversation", sid}, {"input", "run the ticks"}});
        long at = a.until_type("maic.approval.requested", mark);
        expect(at > 0, "manual mode asks before the command");
        json approval = at > 0 ? a.events[at] : json::object();
        const json* call = a.find("response.output_item.done", mark);
        expect(call && (*call)["item"]["type"] == "shell_call" && (*call)["item"]["action"]["commands"][0].get<std::string>().find("tick") != std::string::npos &&
                   approval.value("call_id", json()) == (*call)["item"]["call_id"] && approval.value("item_id", json()) == (*call)["item"]["id"],
               "the shell_call item is done before the approval, which names its call");
        json snap;
        {
            Recording side("attach");
            TestClient b(*engine, side, Origin::Local, "nvim");
            b.hello();
            snap = b.ok("maic.session.attach", {{"session", sid}});
            engine->disconnect(b.id);
        }
        expect(snap["pending"].size() == 1 && snap["pending"][0]["id"] == approval["id"] && snap["entry"]["activity"] == "waiting" &&
                   snap["entry"]["waiting"]["id"] == approval["id"],
               "attach shows the pending approval and the session waiting for it");
        a.ok("maic.approval.answer", {{"session", sid}, {"approval", approval["id"]}, {"choice", "yes"}});
        expect(a.until_idle(at) > 0, "the turn finishes");
        std::string out;
        bool offsets = true;
        size_t next = 0;
        for (size_t i = at; i < a.events.size(); ++i) {
            const json& e = a.events[i];
            if (e["type"] != "response.shell_call_output_content.delta") continue;
            offsets = offsets && e["maic"]["offset"] == next;
            out += e["delta"]["stdout"].get<std::string>();
            next += e["delta"]["stdout"].get<std::string>().size();
        }
        expect(out == "tick1\ntick2\ntick3\n" && offsets, "the output streams as shell_call_output_content.delta while it runs, with byte offsets");
        const json* answered = a.find("maic.approval.answered", at);
        const json* result = nullptr;
        for (size_t i = at; i < a.events.size(); ++i) {
            if (a.events[i]["type"] == "response.output_item.done" && a.events[i]["item"]["type"] == "shell_call_output") result = &a.events[i];
        }
        expect(answered && (*answered)["choice"] == "yes" && (*answered)["by"]["client"] == a.id, "the answer is announced with who gave it");
        expect(result && (*result)["item"]["maic"]["ok"] == true && (*result)["item"]["output"][0]["outcome"]["exit_code"] == 0 &&
                   (*result)["item"]["output"][0]["stdout"].get<std::string>().rfind("exit code 0\ntick1", 0) == 0,
               "the output item closes with OpenAI's shell output and the exit code");
        tool.finish();
    }
    recordings.push_back(&tool);

    section("cancel mid-stream");
    Recording cancel("cancel");
    {
        TestClient a(*engine, cancel, Origin::Local, "tui");
        a.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        size_t mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "a long reply please"}});
        long at = a.until_type("response.output_text.delta", mark);
        std::string rid = a.find("response.created", mark) ? (*a.find("response.created", mark))["response"]["id"].get<std::string>() : "";
        json r = a.ok("cancelResponse", {{"response_id", rid}});
        expect(at > 0 && r["status"] == "cancelled" && r["id"] == rid, "cancelResponse answers the response, cancelled");
        expect(a.until_idle(at) > 0, "the turn stops");
        const json* end = a.find("maic.response.cancelled", mark);
        const json* item = nullptr;
        for (size_t i = mark; i < a.events.size(); ++i) {
            if (a.events[i]["type"] == "response.output_item.done") item = &a.events[i];
        }
        expect(end && (*end)["response"]["status"] == "cancelled" && (*end)["response"]["maic"]["final"] == true && (*end)["response"]["maic"]["ended_by"] == "cancel",
               "maic.response.cancelled ends the response and the turn");
        expect(item && (*item)["item"]["status"] == "incomplete" && !a.find("response.completed", mark), "the partial reply closes incomplete, kept");
        expect(a.error("cancelResponse", {{"response_id", rid}}) == "maic_not_found", "a response that is not running cannot be cancelled");
        cancel.finish();
    }
    recordings.push_back(&cancel);

    section("resume with starting_after after a disconnect");
    Recording resume("resume");
    {
        TestClient a(*engine, resume, Origin::Local, "phone-tab");
        a.hello();
        json sub = a.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        size_t mark = a.events.size();
        fake.reply = [](const json&) { return std::string(400, 'z'); };
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "say z a lot"}});
        a.until_type("response.output_text.delta", mark);
        long held = a.events.back()["sequence_number"];
        std::string before = a.text(mark);
        engine->disconnect(a.id);
        fake.hold_left = 0;
        TestClient b(*engine, resume, Origin::Local, "phone-tab");
        b.hello();
        expect(b.error("maic.session.subscribe", {{"session", sid}, {"epoch", "zzzz"}, {"starting_after", held}}) == "maic_resync",
               "another epoch answers maic_resync");
        json again = b.ok("maic.session.subscribe", {{"session", sid}, {"epoch", sub["epoch"]}, {"starting_after", held}});
        expect(again["replay_from"] == held + 1, "the same epoch resumes after the last number held");
        // The held reply idles until the agent hangs up; cancel it and keep what came.
        b.pump(200ms);
        std::string rid = a.find("response.created", mark) ? (*a.find("response.created", mark))["response"]["id"].get<std::string>() : "";
        b.ok("cancelResponse", {{"response_id", rid}});
        expect(b.until_idle(0) >= 0, "the second connection follows the turn to its end");
        expect(!b.events.empty() && b.events.front()["sequence_number"] == held + 1, "its first event is the one after what the first connection held");
        fake.reply = nullptr;
        resume.finish();
    }
    recordings.push_back(&resume);
    {
        Recording small("ring");
        EngineOptions so = o;
        so.ring_events = 8;
        so.index_file.clear();
        Engine e2(so);
        TestClient a(e2, small, Origin::Local, "tui");
        a.hello();
        std::string s2 = a.ok("createConversation", json::object())["id"];
        a.ok("maic.session.subscribe", {{"session", s2}});
        a.ok("response.create", {{"conversation", s2}, {"input", "fill the ring past eight events"}});
        a.until_idle(1);
        TestClient b(e2, small, Origin::Local, "late");
        b.hello();
        expect(b.error("maic.session.subscribe", {{"session", s2}, {"starting_after", 0}}) == "maic_resync", "a starting_after that left the ring answers maic_resync");
        json whole = b.ok("maic.session.subscribe", {{"session", s2}});
        b.pump(100ms);
        expect(whole["replay_from"].get<long>() > 0 && b.events.size() == 8, "without starting_after the replay is what the ring holds (8 events)");
        small.finish();
    }

    section("two clients on one session: the first approval answer wins");
    Recording two("two-clients");
    {
        TestClient a(*engine, two, Origin::Local, "tui");
        TestClient b(*engine, two, Origin::Remote, "phone");
        a.hello();
        json hb = b.ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "phone"}}}, {"capabilities", {"tool_output"}}});
        expect(hb["origin"] == "remote" && hb["limits"]["always"] == false, "a remote client is told it cannot answer always");
        a.ok("maic.session.subscribe", {{"session", sid}});
        b.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        b.pump(0ms);
        size_t mark = a.events.size(), bmark = b.events.size();
        plan({{{"name", "write_file"}, {"arguments", {{"path", "note.txt"}, {"content", "fluffy tail"}}}}});
        a.ok("response.create", {{"conversation", sid}, {"input", "make a note"}});
        long at = a.until_type("maic.approval.requested", mark);
        long bat = b.until_type("maic.approval.requested", bmark);
        std::string id = at > 0 ? a.events[at]["id"].get<std::string>() : "";
        expect(at > 0 && bat >= 0 && b.events[bat]["id"] == id, "both clients see the approval");
        expect(b.error("maic.approval.answer", {{"session", sid}, {"approval", id}, {"choice", "always"}}) == "maic_forbidden_remote",
               "a remote client cannot answer always");
        a.ok("maic.approval.answer", {{"session", sid}, {"approval", id}, {"choice", "yes"}});
        expect(b.error("maic.approval.answer", {{"session", sid}, {"approval", id}, {"choice", "no"}}) == "maic_already_answered",
               "the second answer gets maic_already_answered");
        a.until_idle(at);
        b.until_idle(bat);
        const json* answered = b.find("maic.approval.answered", bmark);
        expect(answered && (*answered)["choice"] == "yes" && (*answered)["by"]["client"] == a.id && (*answered)["by"]["origin"] == "local",
               "both see who answered, so the other prompt closes");
        expect(slurp(ws / "note.txt") == "fluffy tail", "the approved write happened");
        std::vector<long> sa, sb;
        for (const auto& e : a.events) sa.push_back(e["sequence_number"]);
        for (const auto& e : b.events) sb.push_back(e["sequence_number"]);
        expect(!sb.empty() && sa == sb, "every client sees the same events in the same order (" + std::to_string(sb.size()) + ")");
        expect(b.error("response.create", {{"conversation", sid}, {"input", "x"}, {"instructions", "be loud"}}) == "maic_forbidden_remote",
               "instructions are local only");
        two.finish();
    }
    recordings.push_back(&two);

    section("a remote message raises the turn's origin");
    Recording rise("origin-rises");
    {
        TestClient a(*engine, rise, Origin::Local, "tui");
        TestClient b(*engine, rise, Origin::Remote, "phone");
        a.hello();
        b.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        a.ok("maic.session.set", {{"session", sid}, {"mode", "auto"}});
        a.pump(0ms);
        size_t mark = a.events.size();
        plan({shell("sleep 1"), shell("echo after")});
        a.ok("response.create", {{"conversation", sid}, {"input", "check the build"}});
        long call = a.until([](const json& e) { return e["type"] == "response.output_item.done" && e["item"]["type"] == "shell_call"; }, mark);
        expect(call > 0 && !a.find("maic.approval.requested", mark), "auto mode runs the local turn's first command unasked");
        json first = a.find("response.created", mark) ? *a.find("response.created", mark) : json{{"response", {{"id", ""}, {"maic", {{"turn", 0}}}}}};
        std::string rid = first["response"]["id"];
        json q = b.ok("response.steer", {{"previous_response_id", rid}, {"input", "also run the tests"}});
        expect(q["steer"]["previous_response_id"] == rid, "response.steer on a busy session is accepted for the running response");
        long at = a.until_type("maic.approval.requested", call);
        expect(at > 0 && a.events[at]["origin"] == "remote" && a.events[at]["summary"].get<std::string>().find("echo after") != std::string::npos,
               "the command after the remote message is asked, as remote");
        a.ok("maic.approval.answer", {{"session", sid}, {"approval", a.events[at]["id"]}, {"choice", "yes"}});
        a.until_idle(at);
        const json* input = a.find("maic.input.added", call);
        const json* accepted = a.find("response.steer.accepted", call);
        const json* steered = a.find("response.incomplete", mark);
        const json* next = a.find("response.created", call);
        const json* done = a.find("response.completed", mark);
        expect(input && (*input)["queued"] == true && (*input)["by"]["origin"] == "remote", "the remote input is announced, queued, by the phone");
        expect(accepted && (*accepted)["steer"]["id"] == q["steer"]["id"], "response.steer.accepted says the engine owns it");
        expect(steered && (*steered)["response"]["id"] == rid && (*steered)["response"]["incomplete_details"]["reason"] == "steered" &&
                   (*steered)["response"]["maic"]["final"] == false && (*steered)["response"]["maic"]["origin"] == "local",
               "at the next boundary the response ends incomplete, steered, still local");
        expect(next && (*next)["response"]["previous_response_id"] == rid && (*next)["response"]["maic"]["turn"] == first["response"]["maic"]["turn"] &&
                   (*next)["response"]["maic"]["origin"] == "remote" && done && (*done)["response"]["id"] == (*next)["response"]["id"],
               "its successor carries the remote input, in the same turn, remote: the origin only rises");
        a.ok("maic.session.set", {{"session", sid}, {"mode", "manual"}});
        expect(b.error("maic.session.set", {{"session", sid}, {"mode", "auto"}}) == "maic_step_up_required", "a remote client loosens to auto only after a step-up");
        rise.finish();
    }
    recordings.push_back(&rise);

    section("steering: response.steer, the lane, and the six actions");
    Recording steering("steering");
    {
        TestClient a(*engine, steering, Origin::Local, "tui");
        TestClient b(*engine, steering, Origin::Remote, "phone");
        a.hello();
        b.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        auto running = [&](size_t from) {
            long at = a.until_type("response.output_text.delta", from);
            std::string rid;
            for (size_t i = from; i < a.events.size(); ++i) {
                if (a.events[i]["type"] == "response.created") rid = a.events[i]["response"]["id"];
            }
            return std::make_pair(at, rid);
        };
        auto last = [&](const std::string& type, size_t from) -> const json* {
            const json* found = nullptr;
            for (size_t i = from; i < a.events.size(); ++i) {
                if (a.events[i]["type"] == type) found = &a.events[i];
            }
            return found;
        };

        // The lane: response.create on a busy session is a turn of its own, after the running one.
        size_t mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "the first in line"}});
        auto [at, rid] = running(mark);
        json queued = b.ok("response.create", {{"conversation", sid}, {"input", "the second in line"}});
        expect(queued["status"] == "queued" && queued["maic"]["queued"] == true && queued["maic"]["turn"].get<int>() == a.find("response.created", mark)->at("response")["maic"]["turn"].get<int>() + 1,
               "response.create on a busy lane answers a queued response with the next turn's number");
        json dropped = b.ok("response.create", {{"conversation", sid}, {"input", "never mind this one"}});
        json gone = b.ok("cancelResponse", {{"response_id", dropped["id"]}});
        expect(gone["status"] == "cancelled", "cancelResponse takes a queued response off the lane");
        expect(b.error("response.steer", {{"previous_response_id", queued["id"]}, {"input", "x"}}) == "response_not_active", "a queued response takes no steer");
        a.ok("cancelResponse", {{"response_id", rid}});
        long queued_at = a.until([&](const json& e) { return e["type"] == "response.created" && e["response"]["id"] == queued["id"]; }, mark);
        expect(queued_at > 0 && a.events[queued_at]["response"]["previous_response_id"].is_null(), "after the cancel the queued turn runs, as a turn of its own");
        a.until([&](const json& e) { return e["type"] == "response.completed" && e["response"]["id"] == queued["id"]; }, mark);
        a.until_idle(static_cast<size_t>(queued_at));
        expect(a.text(static_cast<size_t>(queued_at)) == "echo: the second in line" && a.text(mark).find("never mind") == std::string::npos, "it answers its own input; the cancelled one never ran");
        expect(b.error("response.steer", {{"previous_response_id", rid}, {"input", "too late"}}) == "response_already_completed", "a steer for an ended response is response_already_completed");
        expect(b.error("response.steer", {{"previous_response_id", sid + ".r999"}, {"input", "x"}}) == "response_not_found", "one for no response is response_not_found");

        // drop, stopping now: the partial reply trimmed (all of it here), the note in a successor.
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "talk about the cluster"}});
        std::tie(at, rid) = running(mark);
        json st = a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "drop"}, {"note", "leave the cluster out"}, {"trim", "all"}});
        a.until_idle(static_cast<size_t>(at));
        const json* applied = a.find("maic.steer.applied", mark);
        const json* done_text = a.find("response.output_text.done", mark);
        const json* steered = a.find("response.incomplete", mark);
        expect(applied && (*applied)["steer"] == st["steer"]["id"] && (*applied)["action"] == "drop" && (*applied)["trigger"] == "client" && (*applied)["trimmed"]["from"] == 0 &&
                   a.find("response.steer.accepted", mark),
               "maic.steer drop is accepted and applied, saying what it trimmed");
        expect(done_text && (*done_text)["text"] == "" && (*done_text)["maic"]["trimmed"]["from"] == 0, "the reply's done event carries the trimmed text and the range");
        expect(steered && (*steered)["response"]["maic"]["ended_by"] == "drop" && (*steered)["response"]["incomplete_details"]["reason"] == "steered",
               "the response ends incomplete, steered by the drop");
        std::string said = a.text(static_cast<size_t>(steered ? steered - &a.events[0] : 0));
        expect(said.find("echo: The user dropped the topic you had started") == 0 && said.find("leave the cluster out") != std::string::npos,
               "the successor gets drop's text and the note");

        // interrupt pauses the turn; response.steer resumes it.
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "a slow one"}});
        std::tie(at, rid) = running(mark);
        a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "interrupt"}});
        long paused = a.until_type("maic.turn.paused", mark);
        const json* cancelled = a.find("maic.response.cancelled", mark);
        long waiting = a.until([](const json& e) { return e["type"] == "maic.session.state" && e["activity"] == "waiting" && e["waiting"]["kind"] == "steer"; }, mark);
        expect(paused > 0 && cancelled && (*cancelled)["response"]["maic"]["final"] == false && (*cancelled)["response"]["maic"]["ended_by"] == "interrupt" &&
                   a.events[paused]["response_id"] == rid && waiting > 0,
               "interrupt cancels the response without ending the turn, and the session waits, paused");
        expect(a.error("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "interrupt"}}) == "response_not_active", "a paused turn takes no second interrupt");
        b.ok("response.steer", {{"previous_response_id", rid}, {"input", "carry on, gently"}});
        long resumed = a.until([&](const json& e) { return e["type"] == "response.created" && e["response"]["previous_response_id"] == rid; }, mark);
        a.until_idle(static_cast<size_t>(resumed > 0 ? resumed : 0));
        expect(resumed > 0 && a.text(static_cast<size_t>(resumed)) == "echo: carry on, gently" && last("response.completed", mark) &&
                   (*last("response.completed", mark))["response"]["maic"]["final"] == true,
               "a message resumes it in a successor that ends the turn");

        // keep and halt.
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "keep what you have"}});
        std::tie(at, rid) = running(mark);
        a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "keep"}});
        a.until_idle(static_cast<size_t>(at));
        const json* kept = a.find("response.completed", mark);
        expect(kept && (*kept)["response"]["maic"]["ended_by"] == "keep" && (*kept)["response"]["maic"]["final"] == true &&
                   (*kept)["response"]["output"][0]["content"][0]["text"] == "echo",
               "keep ends the turn with the partial reply as the answer");
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "throw this away"}});
        std::tie(at, rid) = running(mark);
        json waits = a.ok("response.steer", {{"previous_response_id", rid}, {"input", "and then this"}});
        a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "halt"}});
        a.until_idle(static_cast<size_t>(at));
        const json* error = a.find("error", mark);
        const json* failed = a.find("response.steer.failed", mark);
        const json* halted = a.find("maic.response.cancelled", mark);
        const json* discarded = nullptr;
        for (size_t i = mark; i < a.events.size(); ++i) {
            if (a.events[i]["type"] == "response.output_item.done" && a.events[i]["item"]["type"] == "message") discarded = &a.events[i];
        }
        expect(error && (*error)["code"] == "maic_halted" && halted && (*halted)["response"]["maic"]["ended_by"] == "halt" && (*halted)["response"]["maic"]["final"] == true,
               "halt: OpenAI's error event with maic_halted, then the response cancelled, the turn over");
        expect(discarded && (*discarded)["item"]["status"] == "incomplete" && (*discarded)["item"]["maic"]["status"] == "discarded", "the partial reply is closed, discarded");
        expect(failed && (*failed)["steer"]["id"] == waits["steer"]["id"] && (*failed)["error"]["code"] == "response_not_active" && (*failed)["steer"]["input"] == "and then this",
               "a steer accepted before the halt comes back in response.steer.failed, with its input");
        bool told = false, leaked = false;
        mark = a.events.size();
        a.ok("response.create", {{"conversation", sid}, {"input", "after the halt"}});
        a.until_idle(mark);
        {
            std::lock_guard lock(fake.mu);
            for (const auto& m : fake.requests.back()["messages"]) {
                std::string t = FakeServer::text_of(m["content"]);
                told = told || t.find("The user halted this turn") != std::string::npos;
                leaked = leaked || (m["role"] == "user" && t.find("and then this") != std::string::npos);
            }
        }
        expect(told && !leaked, "the model is told it was halted, and the failed steer never reaches it");

        // A steer while an approval waits withdraws it; further waits for it.
        mark = a.events.size();
        plan({shell("echo withdrawn")});
        a.ok("response.create", {{"conversation", sid}, {"input", "run something"}});
        long ask = a.until_type("maic.approval.requested", mark);
        rid = a.find("response.created", mark)->at("response")["id"];
        json further = b.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "further"}, {"note", "and why"}});
        const json* waits_for = nullptr;
        a.until_type("maic.steer.applied", mark);
        waits_for = a.find("maic.steer.applied", mark);
        expect(waits_for && (*waits_for)["waits_for"] == a.events[ask]["id"] && !a.find("maic.approval.answered", mark), "further leaves the approval alone and waits for it");
        a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "steer"}, {"note", "do not run it"}});
        a.until_idle(static_cast<size_t>(ask));
        const json* answered = a.find("maic.approval.answered", mark);
        const json* result = nullptr;
        for (size_t i = mark; i < a.events.size(); ++i) {
            if (a.events[i]["type"] == "response.output_item.done" && a.events[i]["item"]["type"] == "shell_call_output") result = &a.events[i];
        }
        expect(answered && (*answered)["choice"] == "withdrawn" && result && (*result)["item"]["output"][0]["stdout"] == "not run: the user redirected",
               "steer withdraws the waiting approval: the call says it was not run");
        std::string next_text = a.text(mark);
        expect(next_text.find("The user asks you to go deeper") != std::string::npos || next_text.find("The user redirected you: do not run it.") != std::string::npos,
               "and the successor carries the notes");

        // Who may send what: steering.clients and steering.actions.
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", sid}, {"input", "one more"}});
        std::tie(at, rid) = running(mark);
        expect(b.error("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "halt"}}) == "maic_steer_disabled", "an action steering.clients keeps from remote clients is maic_steer_disabled");
        a.ok("cancelResponse", {{"response_id", rid}});
        a.until_idle(static_cast<size_t>(at));
        b.pump(100ms);
        steering.finish();
    }
    recordings.push_back(&steering);

    section("a slow consumer gets skips, never a gap");
    Recording slow("slow-consumer");
    {
        TestClient a(*engine, slow, Origin::Local, "tui");
        a.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        a.ok("maic.session.set", {{"session", sid}, {"mode", "auto"}});
        TestClient watcher(*engine, slow, Origin::Local, "watcher");
        watcher.hello();
        watcher.ok("maic.session.subscribe", {{"session", sid}});
        watcher.pump(0ms);
        a.pump(0ms);
        size_t mark = watcher.events.size(), amark = a.events.size();
        plan({shell("head -c 6000000 /dev/zero | tr '\\0' x")});
        a.ok("response.create", {{"conversation", sid}, {"input", "a lot of output"}});
        // `a` reads nothing until the turn is over; the watcher answers if the harness asks.
        for (size_t from = mark;;) {
            long at = watcher.until([](const json& e) { return e["type"] == "maic.approval.requested" || (e["type"] == "maic.session.state" && e["activity"] == "idle"); }, from);
            if (at < 0 || watcher.events[at]["type"] != "maic.approval.requested") break;
            watcher.ok("maic.approval.answer", {{"session", sid}, {"approval", watcher.events[at]["id"]}, {"choice", "yes"}});
            from = static_cast<size_t>(at) + 1;
        }
        a.pump(100ms);
        size_t skipped = 0, sent = 0, offset = 0;
        bool ordered = true;
        for (size_t i = amark; i < a.events.size(); ++i) {
            const json& e = a.events[i];
            if (e["type"] != "response.shell_call_output_content.delta") continue;
            ordered = ordered && e["maic"]["offset"].get<size_t>() >= offset;
            offset = e["maic"]["offset"];
            if (e["maic"].contains("skipped")) skipped += e["maic"]["skipped"].get<size_t>();
            else sent += e["delta"]["stdout"].get<std::string>().size();
        }
        expect(skipped > 0 && sent >= (3u << 19) && sent <= (2u << 20) + (16u << 10) && ordered && engine->closed(a.id).empty(),
               "past 2 MiB queued the output arrives as skip counts (" + std::to_string(sent) + " sent, " + std::to_string(skipped) + " skipped), the connection stays open");
        a.ok("maic.session.set", {{"session", sid}, {"mode", "manual"}});
        slow.finish();
    }
    recordings.push_back(&slow);

    section("a filtered connection: what it excludes is accounted for by maic.filtered_from");
    Recording filtered("filtered");
    {
        TestClient a(*engine, filtered, Origin::Local, "tui");
        a.hello();
        TestClient f(*engine, filtered, Origin::Local, "nvim");
        expect(f.error("maic.hello", {{"protocol", 1}, {"client", {{"name", "nvim"}}}, {"exclude", {"response.completed"}}}) == "maic_not_filterable",
               "a lifecycle event cannot be excluded");
        json h = f.ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "nvim"}}}, {"exclude", {"response.output_text.delta", "response.someday.delta"}}});
        std::set<std::string> want = {"response.output_text.delta", "response.shell_call_output_content.delta", "maic.tool.output.delta"};
        expect(h["exclude"].get<std::set<std::string>>() == want,
               "the hello answers what the connection is spared: the deltas it excluded and the tool output it has no capability for, not a type the engine does not know");
        a.ok("maic.session.subscribe", {{"session", sid}});
        f.ok("maic.session.subscribe", {{"session", sid}});
        a.pump(0ms);
        f.pump(0ms);
        size_t mark = a.events.size(), fmark = f.events.size();
        a.ok("maic.session.set", {{"session", sid}, {"mode", "auto"}});
        plan({shell("echo paw; echo tail")});
        a.ok("response.create", {{"conversation", sid}, {"input", "wag"}});
        expect(a.until_idle(mark) > 0 && f.until_idle(fmark) > 0, "both connections see the turn end");
        a.ok("maic.session.set", {{"session", sid}, {"mode", "manual"}});
        a.pump(100ms);
        f.pump(100ms);
        std::map<long, std::string> all;
        for (size_t i = mark; i < a.events.size(); ++i) all[a.events[i]["sequence_number"]] = a.events[i]["type"];
        bool spared = true, accounted = true;
        size_t marked = 0;
        long last = f.events[fmark - 1]["sequence_number"];
        for (size_t i = fmark; i < f.events.size(); ++i) {
            const json& e = f.events[i];
            long n = e["sequence_number"];
            spared = spared && !want.count(e["type"].get<std::string>());
            long from = e.contains("/maic/filtered_from"_json_pointer) ? e["maic"]["filtered_from"].get<long>() : n;
            marked += from != n;
            accounted = accounted && from == last + 1;
            for (long k = from; k < n; ++k) accounted = accounted && all.count(k) && want.count(all[k]);
            last = n;
        }
        expect(spared && accounted && marked >= 2,
               "the filtered connection gets none of those types, and every run of them is named by the next event's maic.filtered_from (" + std::to_string(marked) + " runs)");
        expect(f.text(fmark).empty() && a.text(mark) == "echo: wag", "the full connection still streams the text");
        // Handing off: a connection with no filter resumes from a number the filtered one holds.
        long held = f.events[fmark + (f.events.size() - fmark) / 2]["sequence_number"];
        TestClient g(*engine, filtered, Origin::Local, "phone");
        g.hello();
        json sub = g.ok("maic.session.subscribe", {{"session", sid}, {"starting_after", held}});
        g.pump(100ms);
        bool resumed = !g.events.empty() && g.events.front()["sequence_number"] == held + 1;
        for (size_t i = 0; i < g.events.size(); ++i) resumed = resumed && g.events[i] == a.events[mark + (static_cast<size_t>(held + 1) - a.events[mark]["sequence_number"].get<size_t>()) + i];
        expect(sub["replay_from"] == held + 1 && resumed, "the numbers are the session's: another connection resumes after one the filtered connection holds and gets the rest unfiltered");
        filtered.finish();
    }
    recordings.push_back(&filtered);

    section("an in-process host's session: open_local, the `:` commands, `!cmd`, titles, delivering now");
    Recording host("local-host");
    {
        EngineOptions lo = o;  // an engine of its own: the index scenarios below count this one's sessions
        lo.index_file.clear();
        Engine local(lo);
        TestClient a(local, host, Origin::Local, "tui");
        a.hello();
        Settings st = o.settings;
        st.small_model = "test";  // the fake titles it: the echo of the first message, one line
        st.dumb_auto_ok = false;
        LocalSession ls;
        ls.workspace = ws;
        ls.settings = st;
        ls.log = std::make_unique<SessionLog>("tui", root / "state" / "local");
        bool set_up = false;
        ls.setup = [&](Agent& agent, SessionLog& log) {
            configure_agent(agent, st);
            agent.mode = Mode::Manual;
            agent.set_log(&log);
            set_up = true;
        };
        std::string lid = local.open_local(a.id, std::move(ls));
        json snap = a.ok("maic.session.attach", {{"session", lid}});
        expect(set_up && snap["entry"]["id"] == lid && snap["entry"]["harness"] == "dumb" && snap["entry"]["think"] == false && snap["sequence_number"] == 0,
               "open_local sets the session up before anyone sees it; attach shows it from its first event");
        auto run = [&](const std::string& line) {
            json r = a.ok("maic.session.command", {{"session", lid}, {"line", line}});
            a.pump(0ms);
            std::string text;
            for (const auto& l : r.value("lines", json::array())) text += l["text"].get<std::string>() + "\n";
            return std::make_pair(r, text);
        };

        size_t mark = a.events.size();
        a.ok("response.create", {{"conversation", lid}, {"input", "name the ears"}});
        long idle = a.until_idle(mark);
        long done = a.until_type("response.completed", mark);
        long titled = a.until_type("maic.session.title", mark);
        expect(idle > 0 && titled > done && titled < idle && a.events[titled]["text"] == "echo: name the ears" && a.events[titled]["source"] == "auto",
               "after the first turn small_model titles the session, between the response's end and idle");
        expect(slurp(fs::path(snap["entry"]["transcript"].get<std::string>())).find("\"type\":\"title\"") != std::string::npos, "and the transcript keeps the title");

        mark = a.events.size();
        auto [mode, mode_text] = run("mode plan");
        const json* changed = a.find("maic.session.settings", mark);
        expect(mode["ok"] == true && mode["lines"].empty() && changed && (*changed)["mode"] == "plan" && (*changed)["by"]["client"] == a.id,
               ":mode changes the mode for everyone, announced in maic.session.settings");
        auto [status, status_text] = run("status");
        expect(status_text.find("mode: plan  (idle)") != std::string::npos && status_text.find("session: ") != std::string::npos, ":status answers in lines");
        auto [ban, ban_text] = run("ban add fennec");
        expect(ban_text == "banned \"fennec\" (from the next model call)\n", ":ban add answers as the TUI always has");
        auto [unknown, unknown_text] = run("frobnicate");
        expect(unknown["ok"] == false && unknown["lines"][0]["level"] == "error", "an unknown command is an error line");
        mark = a.events.size();
        run("rename the tail");
        const json* renamed = a.find("maic.session.title", mark);
        expect(renamed && (*renamed)["text"] == "the tail" && (*renamed)["source"] == "rename" && (*renamed)["by"]["client"] == a.id, ":rename is a maic.session.title by its client");
        json conv = a.ok("updateConversation", {{"conversation_id", lid}, {"metadata", {{"title", "fluffy tail"}}}});
        expect(conv["metadata"]["title"] == "fluffy tail" && conv["maic"]["entry"]["title"] == "fluffy tail", "updateConversation renames it too");

        // Auto under the dumb harness asks first: a command asks its question in the result, maic.session.set refuses.
        expect(a.error("maic.session.set", {{"session", lid}, {"mode", "auto"}}) == "maic_confirm_required", "maic.session.set auto under a dumb harness needs confirm");
        auto [asked, asked_text] = run("mode auto");
        expect(asked.contains("ask") && asked["ask"]["keys"] == "yn" && asked["ask"]["title"] == " dumb harness + auto mode ", ":mode auto asks, with the keys it takes");
        json no = a.ok("maic.session.command", {{"session", lid}, {"ask", asked["ask"]["id"]}, {"key", "n"}});
        expect(no["lines"][0]["text"] == "staying in plan", "answered n, the mode stays");
        expect(a.ok("maic.session.command", {{"session", lid}, {"ask", asked["ask"]["id"]}, {"key", "y"}})["ok"] == false, "a question is answered once");
        auto [again, again_text] = run("mode auto");
        mark = a.events.size();
        json yes = a.ok("maic.session.command", {{"session", lid}, {"ask", again["ask"]["id"]}, {"key", "y"}});
        a.pump(0ms);
        expect(yes["ok"] == true && a.find("maic.session.settings", mark) && (*a.find("maic.session.settings", mark))["mode"] == "auto",
               "answered y, auto is on and announced");
        run("mode manual");

        // `!cmd`: output as maic.tool.output.delta with no output_index, then context for the model.
        mark = a.events.size();
        json sh = a.ok("maic.session.shell", {{"session", lid}, {"command", "printf 'paw\\n'; printf 'tail\\n'; exit 3"}});
        a.pump(0ms);
        std::string printed;
        bool bare = true;
        for (size_t i = mark; i < a.events.size(); ++i) {
            if (a.events[i]["type"] != "maic.tool.output.delta") continue;
            printed += a.events[i]["data"].get<std::string>();
            bare = bare && !a.events[i].contains("output_index") && a.events[i]["item_id"] == sh["item_id"];
        }
        expect(sh["exit_code"] == 3 && printed == "paw\ntail\n" && bare, "!cmd streams its output with no output_index and answers its exit code");
        mark = a.events.size();
        a.ok("response.create", {{"conversation", lid}, {"input", "what did I run"}});
        a.until_idle(mark);
        bool told = false;
        {
            std::lock_guard lock(fake.mu);
            for (const auto& m : fake.requests.back()["messages"]) told = told || FakeServer::text_of(m["content"]).find("[The user ran this in their shell: `printf") != std::string::npos;
        }
        expect(told, "what it printed reaches the model with the next message");

        // A write's content for a diff beside its approval.
        mark = a.events.size();
        plan({{{"name", "write_file"}, {"arguments", {{"path", "whiskers.txt"}, {"content", "long and white"}}}}});
        a.ok("response.create", {{"conversation", lid}, {"input", "write the whiskers"}});
        long ask = a.until_type("maic.approval.requested", mark);
        json proposed = ask > 0 ? a.ok("maic.approval.proposed", {{"session", lid}, {"approval", a.events[ask]["id"]}}) : json::object();
        expect(proposed.value("text", json()) == "long and white" && a.events[ask]["proposed_size"] == 14, "maic.approval.proposed answers what the write would leave");
        if (ask > 0) a.ok("maic.approval.answer", {{"session", lid}, {"approval", a.events[ask]["id"]}, {"choice", "no"}});
        a.until_idle(ask);

        // maic.now: a message mid-turn that does not wait for the model call to end.
        mark = a.events.size();
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", lid}, {"input", "hold on"}});
        a.until_type("response.output_text.delta", mark);
        json now = a.ok("response.create", {{"conversation", lid}, {"input", "and the paws"}, {"maic", {{"now", true}}}});
        long end = a.until_idle(mark);
        expect(now["maic"]["queued"] == true && end > 0 && a.text(mark).find("echo: and the paws") != std::string::npos,
               "maic.now delivers into the running response at once: the held call is dropped and the next one has it");

        // A ban entry's steer: the filter cuts before the match, then the engine applies the action as a person's. One
        // that keeps firing past `retries` halts the turn.
        for (bool escalate : {false, true}) {
            Settings bst = st;
            bst.small_model.clear();
            bst.bans = Bans::from_json({{"patterns", json::array({{{"1", "forbidden"}, {"steer", "drop"}, {"note", escalate ? "Still forbidden." : "Say it another way."}}})}, {"retries", 1}});
            LocalSession bs;
            bs.workspace = ws;
            bs.settings = bst;
            bs.log = std::make_unique<SessionLog>("tui", root / "state" / "local");
            bs.setup = [&](Agent& agent, SessionLog& log) {
                configure_agent(agent, bst);
                agent.mode = Mode::Manual;
                agent.set_log(&log);
            };
            std::string bid = local.open_local(a.id, std::move(bs));
            a.ok("maic.session.attach", {{"session", bid}});
            a.pump(50ms);  // the session it left goes to the background: that state, idle, is not this turn's end
            mark = a.events.size();
            a.ok("response.create", {{"conversation", bid}, {"input", "say forbidden words"}});
            a.until_idle(mark);
            const json* applied = a.find("maic.steer.applied", mark);
            expect(applied && (*applied)["trigger"] == "ban" && (*applied)["ban"]["list"] == "patterns" && (*applied)["ban"]["index"] == 0 &&
                       (*applied)["by"]["client"] == "engine" && (*applied)["by"]["name"] == "bans" && (*applied)["action"] == "drop",
                   "a ban's steer is applied with trigger ban, naming its entry and never the matched text");
            expect(a.text(mark).find("forbidden") == std::string::npos, "the match never reaches a screen");
            if (!escalate) {
                const json* done = a.find("response.completed", mark);
                expect(a.find("response.incomplete", mark) && done && (*done)["response"]["maic"]["final"] == true &&
                           a.text(mark).find("echo: The user dropped the topic you had started") != std::string::npos && a.text(mark).find("Say it another way.") != std::string::npos,
                       "drop: the response ends steered and a successor gets drop's text and the entry's note");
            } else {
                const json* error = a.find("error", mark);
                expect(error && (*error)["code"] == "maic_halted" && a.count("maic.steer.applied") >= 2, "a ban that fires again past its retries halts the turn");
            }
        }

        // The remote allow-list of section 7.
        TestClient b(local, host, Origin::Remote, "phone");
        b.hello();
        expect(b.error("maic.session.command", {{"session", lid}, {"line", "allow rm *"}}) == "maic_forbidden_remote", "a remote client cannot :allow");
        expect(b.error("maic.session.command", {{"session", lid}, {"line", "mode auto"}}) == "maic_forbidden_remote", "nor loosen to auto");
        expect(b.error("maic.session.command", {{"session", lid}, {"line", "forbid remove 1"}}) == "maic_forbidden_remote", "nor remove a forbidden term");
        expect(b.ok("maic.session.command", {{"session", lid}, {"line", "forbid fennec-free"}})["ok"] == true, "but may add one");
        expect(b.error("maic.session.command", {{"session", lid}, {"ask", "k1"}, {"key", "t"}}) == "maic_forbidden_remote", "and never answers a command's question");
        expect(b.error("maic.session.shell", {{"session", lid}, {"command", "id"}}) == "maic_forbidden_remote", "!cmd is local only");
        local.disconnect(b.id);
        host.finish();
    }
    recordings.push_back(&host);

    section("the session index");
    {
        Recording idx("index");
        TestClient a(*engine, idx, Origin::Local, "tui");
        a.hello();
        json entries = a.ok("maic.index.get")["entries"];
        expect(entries.size() == 1 && entries[0]["id"] == sid && entries[0]["state"] == "live", "maic.index.get lists the loaded session");
        a.ok("maic.index.subscribe");
        a.ok("maic.session.set", {{"session", sid}, {"mode", "plan"}});
        a.pump(100ms);
        bool told = false;
        for (const auto& m : a.other) told = told || (m["method"] == "maic.index" && m["params"]["entry"]["mode"] == "plan");
        expect(told, "a subscribed client is told when an entry changes");
        struct stat st {};
        expect(stat(o.index_file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && slurp(o.index_file).find(sid) != std::string::npos,
               "the index is written to its file, 0600");
        TestClient remote(*engine, idx, Origin::Remote, "phone");
        remote.hello();
        expect(!remote.ok("maic.index.get")["entries"][0].contains("transcript"), "a remote client's entries omit the transcript path");
        idx.finish();
    }

    section("driven: a fixed-seed run of messages, approvals, answers from two clients, cancels and steers");
    Recording driven("driven");
    {
        std::mt19937 rng(20261002);
        TestClient a(*engine, driven, Origin::Local, "tui");
        TestClient b(*engine, driven, Origin::Remote, "phone");
        a.hello();
        b.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        b.ok("maic.session.subscribe", {{"session", sid}});
        a.ok("maic.session.set", {{"session", sid}, {"mode", "manual"}});  // every command asks: the approvals are part of the run
        int turns = 0;
        for (int step = 0; step < 14; ++step) {
            a.pump(0ms);
            size_t mark = a.events.size();
            int kind = static_cast<int>(rng() % 6);
            TestClient& who = rng() % 2 ? a : b;
            if (kind == 1 || kind == 3) plan({shell("echo driven " + std::to_string(step))});
            if (kind == 2 || kind == 4) fake.hold_left = 1;
            if (kind == 5) plan({shell("echo steered " + std::to_string(step))});
            who.ok("response.create", {{"conversation", sid}, {"input", "step " + std::to_string(step)}});
            if (kind == 1) {
                long at = a.until_type("maic.approval.requested", mark);
                TestClient& answerer = rng() % 2 ? a : b;
                if (at > 0) answerer.ok("maic.approval.answer", {{"session", sid}, {"approval", a.events[at]["id"]}, {"choice", rng() % 2 ? "yes" : "no"}});
            } else if (kind == 2) {
                a.until_type("response.output_text.delta", mark);
                const json* created = a.find("response.created", mark);
                if (created) (rng() % 2 ? a : b).ok("cancelResponse", {{"response_id", (*created)["response"]["id"]}});
            } else if (kind == 3) {
                // A message while the turn waits on an approval reaches the model at the next step.
                long at = a.until_type("maic.approval.requested", mark);
                const json* created = a.find("response.created", mark);
                if (created) (rng() % 2 ? a : b).ok("response.steer", {{"previous_response_id", (*created)["response"]["id"]}, {"input", "and one more thing"}});
                if (at > 0) a.ok("maic.approval.answer", {{"session", sid}, {"approval", a.events[at]["id"]}, {"choice", "yes"}});
            } else if (kind == 4 || kind == 5) {
                // A steer of a random action mid-reply (4) or at an approval (5); a paused turn is resumed or ended.
                // Mid-reply the fake holds until it is hung up on: a further (which waits for the end of the call) or a
                // refused steer would hold it ten seconds, so those go to the approvals.
                static const char* actions[] = {"steer", "drop", "interrupt", "keep", "halt", "further"};
                std::string action = actions[rng() % (kind == 4 ? 5 : 6)];
                long at = kind == 4 ? a.until_type("response.output_text.delta", mark) : a.until_type("maic.approval.requested", mark);
                const json* created = a.find("response.created", mark);
                if (at > 0 && created) {
                    std::string rid = (*created)["response"]["id"];
                    TestClient& who = kind == 4 || rng() % 2 ? a : b;
                    json r = who.call("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", action}, {"note", "steer " + std::to_string(step)}});
                    if (!r.contains("error") && action == "interrupt" && a.until_type("maic.turn.paused", mark) > 0) {
                        int how = static_cast<int>(rng() % 3);
                        if (how == 0) a.ok("response.steer", {{"previous_response_id", rid}, {"input", "go on"}});
                        else if (how == 1) a.ok("maic.steer", {{"session", sid}, {"response_id", rid}, {"action", "keep"}});
                        else a.ok("cancelResponse", {{"response_id", rid}});
                    }
                }
                // What is still waiting (a further's approval, an approval after a steer's successor) is answered.
                for (size_t from = mark;;) {
                    long ask = a.until([](const json& e) { return e["type"] == "maic.approval.requested" || (e["type"] == "maic.session.state" && e["activity"] == "idle"); }, from);
                    if (ask < 0 || a.events[ask]["type"] != "maic.approval.requested") break;
                    a.call("maic.approval.answer", {{"session", sid}, {"approval", a.events[ask]["id"]}, {"choice", "yes"}});
                    from = static_cast<size_t>(ask) + 1;
                }
            }
            turns += a.until_idle(mark) > 0;
        }
        b.pump(200ms);
        expect(turns == 14, "fourteen driven turns, steered at random, ran to their end");
        driven.finish();
    }
    recordings.push_back(&driven);

    section("the recorded streams, mutated");
    {
        const auto& base = plain.records;
        auto deltas = events_of(base, "response.output_text.delta");
        auto mutate = [&](const std::string& what, const std::function<void(std::vector<json>&)>& fn, const std::string& rule) {
            std::vector<json> r = base;
            fn(r);
            std::string got = verdict(r);
            expect(got == rule, what + (rule.empty() ? " breaks no rule and passes" : " is caught as " + rule) + (got == rule ? "" : " (got " + (got.empty() ? "nothing" : got) + ")"));
        };
        mutate("an event dropped", [&](std::vector<json>& r) { r.erase(r.begin() + static_cast<long>(deltas[1])); }, "seq.next");
        mutate("an event duplicated", [&](std::vector<json>& r) { r.insert(r.begin() + static_cast<long>(deltas[1]), r[deltas[1]]); }, "seq.repeat");
        mutate("an event renumbered", [&](std::vector<json>& r) { r[deltas[1]]["msg"]["params"]["sequence_number"] = r[deltas[1]]["msg"]["params"]["sequence_number"].get<long>() + 5; },
               "seq.next");
        mutate("two events swapped", [&](std::vector<json>& r) {
            size_t done = events_of(r, "response.output_text.done")[0];
            size_t part = events_of(r, "response.content_part.done")[0];
            std::swap(r[done]["msg"]["params"], r[part]["msg"]["params"]);
            std::swap(r[done]["msg"]["params"]["sequence_number"], r[part]["msg"]["params"]["sequence_number"]);
        }, "machine");
        mutate("a type changed", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["type"] = "response.output_text.done"; }, "schema.event");
        mutate("a required field removed", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"].erase("logprobs"); }, "schema.event");
        mutate("a field added beside OpenAI's, outside maic", [&](std::vector<json>& r) {
            size_t d = events_of(r, "response.output_item.done")[0];
            r[d]["msg"]["params"]["item"]["judged_by"] = "rules";
        }, "schema.extra");
        mutate("an event put on another stream", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["stream_id"] = "elsewhere"; }, "stream.id");
        mutate("the stream truncated before an answer", [&](std::vector<json>& r) {
            for (size_t i = 0; i < r.size(); ++i) {
                if (r[i]["dir"] == "in" && r[i]["msg"]["method"] == "response.create") {
                    r.resize(i + 1);
                    break;
                }
            }
        }, "request.answered");
        mutate("a second response opened inside the first", [&](std::vector<json>& r) {
            size_t created = events_of(r, "response.created")[0];
            json second = r[created]["msg"]["params"];
            second["response"]["id"] = second["response"]["id"].get<std::string>() + "x";
            r = insert_event(r, deltas[0], second);
        }, "response.one_open");
        mutate("a steer accepted after the turn ended", [&](std::vector<json>& r) {
            size_t done = events_of(r, "response.completed")[0];
            std::string rid = r[done]["msg"]["params"]["response"]["id"];
            r = insert_event(r, done + 1, {{"type", "response.steer.accepted"}, {"steer", {{"id", "st99"}, {"previous_response_id", rid}}}});
        }, "steer.in_turn");
        mutate("a pause while the response is open", [&](std::vector<json>& r) {
            std::string rid = r[events_of(r, "response.created")[0]]["msg"]["params"]["response"]["id"];
            r = insert_event(r, deltas[1], {{"type", "maic.turn.paused"}, {"turn", 1}, {"steer", "st99"}, {"response_id", rid}});
        }, "turn.paused");
        mutate("a delta's text changed", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["delta"] = "fennec"; }, "");
        mutate("a field added inside maic", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["maic"] = {{"note", "fine"}}; }, "");
        {
            // The filtered recording: a marker removed leaves a gap; an excluded type sent, or a marker on a connection that filters nothing, breaks seq.filtered.
            auto marked = [](const std::vector<json>& r, const std::string& conn) {
                for (size_t i = 0; i < r.size(); ++i) {
                    if (r[i]["conn"] == conn && r[i]["msg"].contains("/params/maic/filtered_from"_json_pointer)) return i;
                }
                return r.size();
            };
            std::string nvim, tui;
            for (const auto& rec : filtered.records) {
                if (rec["dir"] != "in" || rec["msg"]["method"] != "maic.hello") continue;
                if (rec["msg"]["params"]["client"]["name"] == "nvim") nvim = rec["conn"];
                else if (tui.empty()) tui = rec["conn"];  // the full connection said hello first
            }
            std::vector<json> r = filtered.records;
            size_t at = marked(r, nvim);
            r[at]["msg"]["params"]["maic"].erase("filtered_from");
            std::string got = verdict(r);
            expect(got == "seq.next", "a filtered_from removed is a gap (" + got + ")");
            r = filtered.records;
            json sneak = r[at]["msg"];
            sneak["params"] = {{"type", "response.output_text.delta"}, {"sequence_number", r[at]["msg"]["params"]["sequence_number"]}, {"stream_id", sid},
                               {"item_id", "x"}, {"output_index", 0}, {"content_index", 0}, {"delta", "fennec"}, {"logprobs", json::array()}};
            r[at]["msg"] = sneak;
            got = verdict(r);
            expect(got == "seq.filtered", "an excluded type sent to the connection is caught (" + got + ")");
            r = filtered.records;
            for (size_t i = 0; i < r.size(); ++i) {
                if (r[i]["conn"] == tui && r[i]["dir"] == "out" && r[i]["msg"].value("method", "") == "maic.event" && r[i]["msg"]["params"]["sequence_number"] == r[at]["msg"]["params"]["sequence_number"]) {
                    r[i]["msg"]["params"]["maic"]["filtered_from"] = r[at]["msg"]["params"]["maic"]["filtered_from"];
                }
            }
            got = verdict(r);
            expect(got == "seq.filtered", "a filtered_from on a connection that excludes nothing is caught (" + got + ")");
        }
        std::vector<json> r = tool.records;
        size_t req = events_of(r, "maic.approval.requested")[0];
        r.erase(r.begin() + static_cast<long>(req));
        std::string got = verdict(r);
        expect(got == "seq.next", "the approval dropped from the tool call stream is caught (" + got + ")");
        size_t ans = events_of(tool.records, "maic.approval.answered")[0];
        size_t first_output = events_of(tool.records, "response.shell_call_output_content.delta")[0];
        r = tool.records;
        std::swap(r[ans]["msg"]["params"], r[first_output]["msg"]["params"]);
        std::swap(r[ans]["msg"]["params"]["sequence_number"], r[first_output]["msg"]["params"]["sequence_number"]);
        got = verdict(r);
        expect(got == "machine", "output before its approval was answered is caught (" + got + ")");
        r = steering.records;
        size_t applied = events_of(r, "maic.steer.applied")[0];
        r.insert(r.begin() + static_cast<long>(applied) + 1, r[applied]);
        r[applied + 1]["msg"]["params"]["sequence_number"] = r[applied]["msg"]["params"]["sequence_number"].get<long>() + 1;
        for (size_t i = applied + 2; i < r.size(); ++i) {
            json& m = r[i]["msg"];
            if (r[i]["conn"] == r[applied]["conn"] && m.value("method", "") == "maic.event") m["params"]["sequence_number"] = m["params"]["sequence_number"].get<long>() + 1;
        }
        got = verdict(r);
        expect(got == "machine", "a steer applied twice is caught (" + got + ")");
    }

    section("the OpenAI-only view still follows a session");
    {
        std::vector<std::string> types;
        for (const auto& rec : tool.records) {
            const json& m = rec["msg"];
            if (m.value("method", "") != "maic.event" || rec["conn"] != tool.records.front()["conn"]) continue;
            if (auto v = protocol::openai_view(m["params"])) types.push_back((*v)["type"]);
        }
        auto has = [&](const std::string& t) { return std::find(types.begin(), types.end(), t) != types.end(); };
        expect(has("response.created") && has("response.output_item.added") && has("response.shell_call_output_content.delta") && has("response.completed") &&
                   !has("maic.approval.requested"),
               "with maic.* removed, it still sees the response created, its items, the command's output and the response completed");
    }

    section("auto at start: held at manual where the workspace is not trusted, unless asked for");
    {
        EngineOptions oa = o;
        oa.settings.mode = "auto";
        oa.index_file = root / "state" / "engine-auto" / "index.json";
        oa.protocol_log = root / "state" / "engine-auto" / "protocol.log";
        Engine e(oa);
        Recording autostart("auto-start");
        TestClient a(e, autostart, Origin::Local, "tui");
        a.hello();
        json held = a.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}});
        a.ok("maic.session.subscribe", {{"session", held["id"]}});
        long notice = a.until([](const json& ev) { return ev["type"] == "maic.notice" && ev.value("text", "").find("auto mode waits") != std::string::npos; });
        expect(held["maic"]["entry"]["mode"] == "manual" && notice >= 0, "auto from the settings starts in manual in an untrusted workspace, and says why");
        json asked = a.ok("createConversation", {{"maic", {{"workspace", ws.string()}, {"mode", "auto"}}}});
        expect(asked["maic"]["entry"]["mode"] == "auto", "a local client asking for auto in the call starts in auto");
        TestClient r(e, autostart, Origin::Remote, "phone");
        r.hello();
        json remote = r.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}});
        expect(remote["maic"]["entry"]["mode"] == "manual", "a remote client's session starts in manual when it names no mode");
        expect(r.error("createConversation", {{"maic", {{"workspace", ws.string()}, {"mode", "auto"}}}}) == "maic_step_up_required",
               "a remote client creating a session in auto needs the same step-up as switching one to auto");
        expect(r.ok("createConversation", {{"maic", {{"workspace", ws.string()}, {"mode", "edit"}}}})["maic"]["entry"]["mode"] == "edit",
               "a remote client may create a session in edit");
        EngineOptions ob = oa;
        ob.mode_asked = true;
        ob.index_file = root / "state" / "engine-asked" / "index.json";
        ob.protocol_log = root / "state" / "engine-asked" / "protocol.log";
        autostart.finish();
        Engine eb(ob);
        Recording asked_rec("auto-start-asked");
        TestClient b(eb, asked_rec, Origin::Local, "rpc");
        b.hello();
        expect(b.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}})["maic"]["entry"]["mode"] == "auto", "the host's --mode auto starts in auto anywhere");
        asked_rec.finish();
    }

    section("protocol tiers per session: the default, a directory's, :tier, a resumed session's own; and leaving a session");
    {
        fs::path open_dir = root / "ws-open", air_dir = root / "ws-air";
        fs::create_directories(open_dir);
        fs::create_directories(air_dir);
        open_dir = fs::weakly_canonical(open_dir);
        air_dir = fs::weakly_canonical(air_dir);
        EngineOptions ot = o;
        ot.settings.protocol_tiers = {{open_dir.string(), "open"}, {air_dir.string(), "airtight"}};
        ot.workspaces = {ws, open_dir, air_dir};
        ot.index_file = root / "state" / "engine-tier" / "index.json";
        ot.protocol_log = root / "state" / "engine-tier" / "protocol.log";
        std::string oid;
        {
            Engine e(ot);
            Recording tiers("tiers");
            TestClient a(e, tiers, Origin::Local, "tui");
            a.hello();
            json plain = a.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}});
            expect(plain["maic"]["entry"]["tier"] == "guarded", "a session where nothing is enrolled works at the default tier");
            json open = a.ok("createConversation", {{"maic", {{"workspace", open_dir.string()}}}});
            oid = open.value("id", "");
            expect(open["maic"]["entry"]["tier"] == "open", "a session in a directory protocol_tiers enrolls works at that tier");
            expect(a.error("createConversation", {{"maic", {{"workspace", air_dir.string()}}}}) == "maic_tier_unavailable",
                   "airtight is refused by a build without the conformance stamp");
            a.ok("maic.session.subscribe", {{"session", oid}});
            json up = a.ok("maic.session.command", {{"session", oid}, {"line", "tier guarded"}});
            long changed = a.until([](const json& ev) { return ev["type"] == "maic.session.settings" && ev.value("tier", "") == "guarded"; });
            expect(up.value("ok", false) && changed >= 0, ":tier guarded tightens it, announced in maic.session.settings");
            TestClient r(e, tiers, Origin::Remote, "phone");
            r.hello();
            expect(!r.ok("maic.session.command", {{"session", oid}, {"line", "tier open"}}).value("ok", true), "a remote client cannot loosen it");
            expect(a.ok("maic.session.command", {{"session", oid}, {"line", "tier open"}}).value("ok", false), "a local client loosens it back to the tier it opened at");
            expect(!a.ok("maic.session.command", {{"session", plain["id"]}, {"line", "tier open"}}).value("ok", true), "but never below the tier a session opened at");
            json status = a.ok("maic.session.command", {{"session", oid}, {"line", "status"}});
            expect(status.dump().find("protocol tier: open (directory " + open_dir.string() + " (protocol_tiers))") != std::string::npos, ":status says the tier and where it came from");

            // A client that goes (Engine::leave, as the daemon's connections end): the idle session in its focus is parked.
            TestClient b(e, tiers, Origin::Local, "socket");
            b.hello();
            std::string idle = b.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}}).value("id", "");
            e.leave(b.id);
            e.disconnect(b.id);
            json listed = a.ok("maic.index.get");
            bool parked = false;
            for (const auto& en : listed["entries"]) parked |= en["id"] == idle && en["state"] == "parked";
            expect(parked, "leaving parks the idle session a client had in focus");
            a.ok("maic.session.park", {{"session", oid}});
            tiers.finish();
        }
        // Resumed, a session keeps the tier its start record names, though the directory is no longer enrolled.
        EngineOptions oc = ot;
        oc.settings.protocol_tiers.clear();
        Engine e(oc);
        Recording again("tiers-resumed");
        TestClient a(e, again, Origin::Local, "tui");
        a.hello();
        json resumed = a.ok("maic.session.resume", {{"session", oid}});
        expect(resumed.value("tier", "") == "open", "a resumed session keeps the tier it started at");
        again.finish();
    }

    section("history: attach's exchanges, listConversationItems paging back lazily, collapsing, maic.item.expand, a fork");
    Recording hist("history");
    {
        // A transcript written here: 60 exchanges of a question, a command and an answer, an attached file and a
        // title before them, a compaction every 20 exchanges, and skeleton and bookkeeping lines between.
        fs::path dir = root / "hist";
        fs::create_directories(dir);
        fs::path parent = dir / "20260101-090000-tui-4242.jsonl", child = dir / "20260102-090000-tui-4343.jsonl";
        std::vector<std::string> expected;   // every displayable record's id, in order
        std::vector<size_t> starts;          // where each exchange starts, into `expected`
        std::map<std::string, std::string> results;
        std::string attached;
        for (int i = 0; attached.size() < 100 * 1024; ++i) attached += "line " + std::to_string(i) + ": フェネックの耳と大きな尻尾\n";
        size_t inherited = 0, inherited_lines = 0;
        {
            std::ofstream f(parent);
            size_t line = 0;
            auto put = [&](json r, bool shown) {
                r["time"] = "2026-01-01T09:00:00+00:00";
                f << r.dump() << "\n";
                std::string id = parent.stem().string() + "#" + std::to_string(++line);
                if (r["type"] == "user") starts.push_back(expected.size());
                if (shown) expected.push_back(id);
                return id;
            };
            f << json{{"type", "skeleton"}, {"of", "start"}, {"hash", "sha256:0"}, {"canonical", "RFC 8785"}, {"skeleton", json::object()}}.dump() << "\n";
            put({{"type", "start"}, {"workspace", ws.string()}, {"model", "test"}, {"mode", "manual"}}, false);
            put({{"type", "context"}, {"text", attached}}, true);
            put({{"type", "title"}, {"text", "fennec history"}}, true);
            for (int k = 0; k < 60; ++k) {
                put({{"type", "user"}, {"text", "question " + std::to_string(k)}}, true);
                std::string out = "exit code 0\n";
                while (out.size() < 5000) out += "output of " + std::to_string(k) + "\n";
                results[put({{"type", "tool"}, {"tool", "run_shell"}, {"arguments", {{"command", "echo " + std::to_string(k)}}}, {"ok", true}, {"result", out}}, true)] = out;
                put({{"type", "usage"}, {"input_tokens", 10}, {"output_tokens", 2}}, false);
                if (k == 30) f << json{{"type", "skeleton"}, {"of", "usage"}, {"hash", "sha256:1"}, {"canonical", "RFC 8785"}, {"skeleton", json::object()}}.dump() << "\n";
                put({{"type", "assistant"}, {"text", "answer " + std::to_string(k)}}, true);
                if (k % 20 == 19) put({{"type", "compact"}, {"stage", "head"}, {"summary", "the first " + std::to_string(k + 1) + " exchanges"}}, true);
                if (k == 9) inherited = expected.size(), inherited_lines = line;
            }
        }
        {
            std::ofstream f(child);
            f << json{{"type", "resumed_from"}, {"path", parent.string()}, {"id", parent.stem().string()}, {"records", inherited_lines}}.dump() << "\n";
            f << json{{"type", "start"}, {"workspace", ws.string()}, {"model", "test"}, {"mode", "manual"}}.dump() << "\n";
            f << json{{"type", "user"}, {"text", "forked question"}}.dump() << "\n";
            f << json{{"type", "assistant"}, {"text", "forked answer"}}.dump() << "\n";
        }
        auto ids = [](const json& items) {
            std::vector<std::string> out;
            for (const auto& i : items) out.push_back(i["id"]);
            return out;
        };

        TestClient a(*engine, hist, Origin::Local, "tui");
        a.hello();
        std::string hid = a.ok("maic.session.resume", {{"session", parent.string()}})["id"];
        json snap = a.ok("maic.session.attach", {{"session", hid}});
        std::vector<std::string> last3(expected.begin() + static_cast<long>(starts[starts.size() - 3]), expected.end());
        expect(snap["more_before"] == true && ids(snap["items"]) == last3,
               "attach answers the last three exchanges from the transcript, each item `<file id>#<line>` (skeleton lines are not lines)");
        const json& first = snap["items"][0];
        expect(first["type"] == "message" && first["role"] == "user" && first["content"][0]["text"] == "question 57", "a user record is OpenAI's user message");
        const json& out57 = snap["items"][1];
        expect(out57["type"] == "shell_call_output" && out57["maic"]["summary"] == "$ echo 57" && out57["maic"]["ok"] == true &&
                   out57["output"][0]["stdout"] == results[out57["id"]] && !out57["maic"].contains("collapsed"),
               "a run_shell record is a shell_call_output with the harness's summary, whole under a local client's 64 KiB");
        expect(snap["items"].back()["type"] == "maic.notice" && snap["items"].back()["kind"] == "compact", "a compaction shows as a maic.notice");

        std::vector<std::string> older;
        std::string after = snap["items"][0]["id"];
        int pages = 0;
        for (bool more = true; more && pages < 20; ++pages) {
            json page = a.ok("listConversationItems", {{"conversation_id", hid}, {"after", after}, {"maic", {{"exchanges", 25}}}});
            std::vector<std::string> got = ids(page["data"]);
            older.insert(older.begin(), got.rbegin(), got.rend());
            more = page["has_more"];
            if (!got.empty()) after = page["last_id"];
            expect(page["object"] == "list" && (got.empty() || (page["first_id"] == got.front() && page["last_id"] == got.back())), "a page is OpenAI's item list");
        }
        older.insert(older.end(), last3.begin(), last3.end());
        expect(older == expected && pages == 3, "listConversationItems pages back newest first, 25 exchanges a page, to the first record and no further");
        json page5 = a.ok("listConversationItems", {{"conversation_id", hid}, {"after", snap["items"][0]["id"]}, {"maic", {{"exchanges", 5}}}});
        std::vector<std::string> want5(expected.begin() + static_cast<long>(starts[starts.size() - 8]), expected.begin() + static_cast<long>(starts[starts.size() - 3]));
        std::vector<std::string> got5 = ids(page5["data"]);
        std::reverse(got5.begin(), got5.end());
        expect(got5 == want5 && page5["has_more"] == true, "maic.exchanges takes whole exchanges");
        json asc = a.ok("listConversationItems", {{"conversation_id", hid}, {"order", "asc"}, {"limit", 7}});
        json asc2 = a.ok("listConversationItems", {{"conversation_id", hid}, {"order", "asc"}, {"limit", 7}, {"after", asc["last_id"]}});
        expect(ids(asc["data"]) == std::vector<std::string>(expected.begin(), expected.begin() + 7) &&
                   ids(asc2["data"]) == std::vector<std::string>(expected.begin() + 7, expected.begin() + 14) && asc2["has_more"] == true,
               "order asc pages forward from the start by limit");
        json latest = a.ok("listConversationItems", {{"conversation_id", hid}, {"limit", 2}});
        expect(ids(latest["data"]) == std::vector<std::string>{expected.back(), expected[expected.size() - 2]}, "with no `after`, desc starts at the newest");

        json ctx = a.ok("getConversationItem", {{"conversation_id", hid}, {"item_id", expected[0]}});
        expect(ctx["type"] == "maic.notice" && ctx["kind"] == "context" && ctx["text"] == "" && ctx["maic"]["collapsed"] == true &&
                   ctx["maic"]["size"] == attached.size() && ctx["maic"]["head"] == attached.substr(0, ctx["maic"]["head"].get<std::string>().size()) &&
                   ctx["maic"]["head"].get<std::string>().size() <= 200,
               "an attached file over collapse_over comes collapsed, with its size and a head of at most 200 bytes");
        std::string whole;
        size_t offset = 0;
        int parts = 0;
        bool contiguous = true, done = false;
        while (!done && parts < 10) {
            json part = a.ok("maic.item.expand", {{"session", hid}, {"item_id", expected[0]}, {"offset", offset}, {"length", 40000}});
            contiguous = contiguous && part["offset"] == offset && part["size"] == attached.size();
            whole += part["text"].get<std::string>();
            offset += part["text"].get<std::string>().size();
            done = part["done"];
            ++parts;
        }
        expect(whole == attached && parts == 3 && contiguous, "maic.item.expand serves it in parts cut on characters, end to end");
        expect(a.error("getConversationItem", {{"conversation_id", hid}, {"item_id", "nope#1"}}) == "maic_not_found" &&
                   a.error("maic.item.expand", {{"session", hid}, {"item_id", "nope#1"}}) == "maic_not_found" &&
                   a.error("listConversationItems", {{"conversation_id", hid}, {"after", "nope#1"}}) == "maic_not_found",
               "an item that is not there is maic_not_found");

        TestClient r(*engine, hist, Origin::Remote, "phone");
        r.hello();
        json rsnap = r.ok("maic.session.attach", {{"session", hid}, {"exchanges", 1}});
        const json& rout = rsnap["items"][1];
        expect(rout["type"] == "shell_call_output" && rout["output"].empty() && rout["maic"]["collapsed"] == true && rout["maic"]["size"] == results[rout["id"]].size() &&
                   rout["maic"]["head"] == "exit code 0\noutput of 59\noutput of 59",
               "a remote client's 2 KiB collapses a command's output to its first three lines");
        TestClient n(*engine, hist, Origin::Local, "maic.nvim");
        n.ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "maic.nvim"}}}, {"capabilities", {"tool_output"}}, {"view", {{"collapse_over", 0}}}});
        expect(n.ok("getConversationItem", {{"conversation_id", hid}, {"item_id", expected[0]}})["text"] == attached, "collapse_over 0 never collapses");

        std::string cid = a.ok("maic.session.resume", {{"session", child.string()}})["id"];
        json csnap = a.ok("maic.session.attach", {{"session", cid}, {"exchanges", 100}});
        std::vector<std::string> want(expected.begin(), expected.begin() + static_cast<long>(inherited));
        want.push_back(child.stem().string() + "#3");
        want.push_back(child.stem().string() + "#4");
        expect(ids(csnap["items"]) == want && csnap["more_before"] == false, "a fork's history starts with its parent's records under the parent's ids");

        // A live turn on the resumed session: its records join the history as they are written.
        a.pump(0ms);  // attach subscribed it
        size_t mark = a.events.size();
        a.ok("response.create", {{"conversation", hid}, {"input", "one more"}});
        expect(a.until_idle(mark) > 0, "a turn on the resumed session finishes");
        json after_turn = a.ok("maic.session.attach", {{"session", hid}, {"exchanges", 1}});
        const json& items = after_turn["items"];
        auto line_of = [](const std::string& id) { return std::stol(id.substr(id.find('#') + 1)); };
        expect(items.size() == 2 && items[0]["content"][0]["text"] == "one more" && items[1]["content"][0]["text"] == "echo: one more" &&
                   line_of(items[0]["id"]) > line_of(expected.back()),
               "what was appended is read on the next request, after the records before it");

        // Attach while a command runs: inflight carries its output so far and the call.
        plan({shell("echo first; sleep 1; echo second")});
        mark = a.events.size();
        a.ok("response.create", {{"conversation", hid}, {"input", "run it"}});
        long ask = a.until_type("maic.approval.requested", mark);
        a.ok("maic.approval.answer", {{"session", hid}, {"approval", ask > 0 ? a.events[ask]["id"] : json("")}, {"choice", "yes"}});
        a.until([](const json& e) { return e["type"] == "response.shell_call_output_content.delta" && e["delta"]["stdout"].get<std::string>().find("first") != std::string::npos; }, mark);
        json live = n.ok("maic.session.attach", {{"session", hid}, {"exchanges", 1}});
        bool running = false;
        for (const auto& i : live["inflight"].value("items", json::array())) {
            running = running || (i["item"]["type"] == "shell_call_output" && i["text"].get<std::string>().find("first") != std::string::npos &&
                                  i["call"]["type"] == "shell_call");
        }
        expect(running, "attach mid-command answers inflight: the output item with what it printed so far, and its call");
        expect(a.until_idle(mark) > 0, "and the turn finishes");
        engine->disconnect(r.id);
        engine->disconnect(n.id);
        hist.finish();
    }

    section("several sessions in one engine: parallel turns, focus, fork, park, resume and stop");
    Recording multi("sessions");
    {
        TestClient a(*engine, multi, Origin::Local, "tui");
        a.hello();
        a.ok("maic.index.subscribe");
        auto state_of = [&](const std::string& id) {
            json entries = a.ok("maic.index.get")["entries"];
            for (const auto& e : entries) {
                if (e["id"] == id) return e;
            }
            return json();
        };
        auto on = [](const std::string& id, const std::string& type) { return [id, type](const json& e) { return e["stream_id"] == id && e["type"] == type; }; };
        json one = a.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}});
        std::string s1 = one["id"];
        expect(one["maic"]["entry"]["state"] == "live", "a session a client creates opens live, in its focus");
        json two = a.ok("createConversation", {{"maic", {{"workspace", ws.string()}}}});
        std::string s2 = two["id"];
        expect(two["maic"]["entry"]["state"] == "live" && state_of(s1)["state"] == "background",
               "creating another moves the client's focus, and without `leave` the first only goes to the background");
        a.ok("maic.session.subscribe", {{"session", s1}});
        a.ok("maic.session.subscribe", {{"session", s2}});
        a.pump(50ms);

        // The first session's reply is held mid-stream; the second's runs to its end meanwhile.
        int base;
        {
            std::lock_guard lock(fake.mu);
            base = fake.streaming;
        }
        size_t mark = a.events.size();
        fake.hold_left = 1;
        json r1 = a.ok("response.create", {{"conversation", s1}, {"input", "the first one waits"}});
        fake.wait_streaming(base + 1);
        a.ok("response.create", {{"conversation", s2}, {"input", "the second one runs"}});
        long done2 = a.until(on(s2, "response.completed"), mark);
        bool first_open = true;
        for (size_t i = mark; i < a.events.size(); ++i) first_open = first_open && !on(s1, "response.completed")(a.events[i]);
        expect(done2 >= 0 && first_open, "two sessions' turns run in parallel: the second completes while the first is still streaming");
        a.ok("cancelResponse", {{"response_id", r1["id"]}});
        a.until([&](const json& e) { return e["stream_id"] == s1 && e["type"] == "maic.session.state" && e["activity"] == "idle"; }, mark);
        expect(state_of(s1)["unseen"] == true && state_of(s2)["unseen"] == false, "a turn that ends while no client has the session in focus marks it unseen");

        json f = a.ok("maic.session.focus", {{"session", s1}, {"leave", {{"as", "bg"}}}});
        expect(f["state"] == "live" && f["unseen"] == false && state_of(s2)["state"] == "background",
               "maic.session.focus brings a session into focus, clears unseen, and the one left goes to the background");

        mark = a.events.size();
        json fk = a.ok("maic.session.fork", {{"session", s1}, {"leave", {{"as", "bg"}}}});
        std::string s3 = fk["id"];
        a.ok("maic.session.subscribe", {{"session", s3}});
        long first = a.until(on(s3, "maic.session.state"), mark);
        expect(fk["state"] == "live" && fk["turns"].get<int>() == 1 && state_of(s1)["state"] == "background", "a fork opens in focus with its parent's turns");
        expect(first >= 0 && a.events[first].contains("forked_from") && a.events[first]["forked_from"]["session"] == s1,
               "its first event's epoch names the session it forked from");

        a.ok("maic.session.focus", {{"session", s1}, {"leave", {{"as", "default"}}}});
        expect(state_of(s3)["state"] == "parked" && a.error("getConversation", {{"conversation_id", s3}}) == "maic_not_found",
               "the default leave parks an idle session: it leaves memory and stays in the index");

        {
            std::lock_guard lock(fake.mu);
            base = fake.streaming;
        }
        fake.hold_left = 1;
        a.ok("response.create", {{"conversation", s2}, {"input", "busy for a while"}});
        fake.wait_streaming(base + 1);
        expect(a.error("maic.session.park", {{"session", s2}}) == "maic_busy", "parking a session mid-turn is maic_busy without interrupt");
        a.ok("response.create", {{"conversation", s2}, {"input", "queued behind"}});
        json pk = a.ok("maic.session.park", {{"session", s2}, {"interrupt", true}});
        expect(pk["state"] == "parked" && pk["queued"] == 1 && !pk.contains("queued_inputs"),
               "with interrupt it parks, keeping the message that waited on the lane (its text stays in the engine)");
        mark = a.events.size();
        json rs = a.ok("maic.session.resume", {{"session", s2}, {"leave", {{"as", "bg"}}}});
        a.ok("maic.session.subscribe", {{"session", s2}});
        long ran = a.until([&](const json& e) {
            return e["stream_id"] == s2 && e["type"] == "response.output_text.done" && e.value("text", "").find("echo: queued behind") != std::string::npos;
        }, mark);
        expect(rs["state"] == "live" && ran >= 0, "resuming the parked session runs the message that waited");
        a.until([&](const json& e) { return e["stream_id"] == s2 && e["type"] == "maic.session.state" && e["activity"] == "idle"; }, static_cast<size_t>(std::max(ran, 0L)));

        a.pump(50ms);
        json stop = a.ok("maic.session.stop", {{"session", s2}});
        a.pump(50ms);
        bool removed = false;
        for (const auto& m : a.other) removed = removed || (m["method"] == "maic.index" && m["params"].value("removed", "") == s2);
        expect(stop["state"] == "stopped" && state_of(s2).is_null() && removed, "maic.session.stop ends it, takes it off the index and says so");
        json stopped_parked = a.ok("maic.session.stop", {{"session", s3}});
        expect(stopped_parked["state"] == "stopped" && state_of(s3).is_null(), "a parked session can be stopped too");

        TestClient b(*engine, multi, Origin::Local, "nvim");
        b.hello();
        b.ok("maic.session.focus", {{"session", s1}});
        engine->disconnect(a.id);
        TestClient c(*engine, multi, Origin::Local, "look");
        c.hello();
        auto entry_of = [&](const std::string& id) { return c.ok("getConversation", {{"conversation_id", id}})["maic"]["entry"]; };
        expect(entry_of(s1)["state"] == "live", "a session stays live while another client has it in focus");
        engine->disconnect(b.id);
        expect(entry_of(s1)["state"] == "background", "a client that goes lets go of its focus: the session stays loaded, in the background");
        engine->disconnect(c.id);
        multi.finish();

        // A session loads again only with a load's header: a state after `parked` without one is caught.
        std::vector<json> mutated = multi.records;
        bool parked = false, cut = false;
        for (auto& r : mutated) {
            json& ev = r["msg"]["params"];
            if (r["msg"].value("method", "") != "maic.event" || ev["stream_id"] != s2 || ev["type"] != "maic.session.state") continue;
            if (ev["state"] == "parked") parked = true;
            else if (parked && ev.contains("epoch")) {
                ev.erase("epoch");
                ev.erase("protocol");
                cut = true;
                break;
            }
        }
        expect(cut && verdict(mutated) == "machine", "mutated: a session's state after parked without a new load's epoch is caught");
    }
    recordings.push_back(&multi);

    // The epoch a load ran under, and the number its stream reached, before the restart.
    std::string epoch_before;
    long numbers_before = -1;
    {
        Recording live("before-restart");
        TestClient a(*engine, live, Origin::Local, "tui-last");
        a.hello();
        json snap = a.ok("maic.session.attach", {{"session", sid}});
        epoch_before = snap["epoch"];
        numbers_before = snap["sequence_number"];
        live.finish();
    }

    section("shutdown parks the sessions in the index, and the epoch survives the restart");
    {
        engine->shutdown();
        engine.reset();
        Engine again(o);
        Recording after("restart");
        TestClient a(again, after, Origin::Local, "tui");
        a.hello();
        json entries = a.ok("maic.index.get")["entries"];
        bool parked = !entries.empty(), listed = false;
        for (const auto& e : entries) {
            parked = parked && e["state"] == "parked";
            listed = listed || e["id"] == sid;
        }
        expect(listed && parked, "after a restart the session is listed parked, with the others that were loaded");
        json entry = a.ok("maic.session.resume", {{"session", sid}});
        expect(entry["state"] == "live" && entry["turns"].get<int>() >= 5, "maic.session.resume loads it again, its turns counted from the transcript");
        // A client that held everything up to the last load's close (shutdown's `parked`, the number after the attach) resumes across the restart.
        json sub = a.ok("maic.session.subscribe", {{"session", sid}, {"epoch", epoch_before}, {"starting_after", numbers_before + 1}});
        a.until_type("maic.session.state");
        const json* found = a.find("maic.session.state");
        json state = found ? *found : json::object();
        expect(sub["epoch"] == epoch_before, "the reloaded session keeps its epoch across the restart");
        expect(found && state["sequence_number"].get<long>() == numbers_before + 2, "its numbers go on from where the last load closed, not back to 0");
        expect(found && state["epoch"] == epoch_before && state["protocol"]["hash"] == maic::protocol::protocol_hash() && state["protocol"]["canonical"] == "RFC 8785",
               "the load's first event carries the epoch and the build's protocol hash");
        expect(!state["maic"].contains("previous_epoch") && !state.contains("previous_epoch"), "no new epoch: nothing was reset");
        after.finish();
    }

    section("the engine's own checks found nothing");
    {
        std::string log = slurp(o.protocol_log);
        expect(log.find("\"kind\":\"event\"") == std::string::npos && log.find("\"kind\":\"result\"") == std::string::npos,
               "no event or result the engine sent failed its schema or the order at run time (protocol.log)" +
                   (log.find("\"kind\":\"event\"") == std::string::npos ? "" : ": " + log.substr(log.find("\"kind\":\"event\""), 300)));
    }

    fs::remove_all(root);
    return finish();
}
