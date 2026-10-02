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
    o.settings.providers = {fake.provider()};
    o.workspaces = {ws};
    o.index_file = root / "state" / "engine" / "index.json";
    o.protocol_log = root / "state" / "engine" / "protocol.log";
    auto engine = std::make_unique<Engine>(o);
    std::mutex calls_mu;
    std::vector<json> script;  // tool calls the fake answers with, in order; then echoes
    fake.tool_call_for = [&](const json&) -> json {
        std::lock_guard lock(calls_mu);
        if (script.empty()) return nullptr;
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
                   h["path"]["via"] == "in-process" && h["engine"]["epoch"].get<std::string>().size() == 6,
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
        expect(b.error("maic.session.subscribe", {{"session", sid}, {"load", "zzzz"}, {"starting_after", held}}) == "maic_resync",
               "another load answers maic_resync");
        json again = b.ok("maic.session.subscribe", {{"session", sid}, {"load", sub["load"]}, {"starting_after", held}});
        expect(again["replay_from"] == held + 1, "the same load resumes after the last number held");
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
        json hb = b.ok("maic.hello", {{"protocol", 1}, {"client", {{"name", "phone"}}}});
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
        json q = b.ok("response.create", {{"conversation", sid}, {"input", "also run the tests"}});
        expect(q["maic"]["queued"] == true, "a message on a busy session is delivered into the running response");
        long at = a.until_type("maic.approval.requested", call);
        expect(at > 0 && a.events[at]["origin"] == "remote" && a.events[at]["summary"].get<std::string>().find("echo after") != std::string::npos,
               "the command after the remote message is asked, as remote");
        a.ok("maic.approval.answer", {{"session", sid}, {"approval", a.events[at]["id"]}, {"choice", "yes"}});
        a.until_idle(at);
        const json* input = a.find("maic.input.added", call);
        const json* created = a.find("response.created", mark);
        const json* done = a.find("response.completed", mark);
        expect(input && (*input)["queued"] == true && (*input)["by"]["origin"] == "remote", "the remote input is announced, queued, by the phone");
        expect(created && done && (*created)["response"]["maic"]["origin"] == "local" && (*done)["response"]["maic"]["origin"] == "remote",
               "the response opened local and closed remote: the origin only rises");
        a.ok("maic.session.set", {{"session", sid}, {"mode", "manual"}});
        expect(b.error("maic.session.set", {{"session", sid}, {"mode", "auto"}}) == "maic_step_up_required", "a remote client loosens to auto only after a step-up");
        rise.finish();
    }
    recordings.push_back(&rise);

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

    section("driven: a fixed-seed run of messages, approvals, answers from two clients and cancels");
    Recording driven("driven");
    {
        std::mt19937 rng(20261002);
        TestClient a(*engine, driven, Origin::Local, "tui");
        TestClient b(*engine, driven, Origin::Remote, "phone");
        a.hello();
        b.hello();
        a.ok("maic.session.subscribe", {{"session", sid}});
        b.ok("maic.session.subscribe", {{"session", sid}});
        int turns = 0;
        for (int step = 0; step < 8; ++step) {
            a.pump(0ms);
            size_t mark = a.events.size();
            int kind = static_cast<int>(rng() % 4);
            TestClient& who = rng() % 2 ? a : b;
            if (kind == 1 || kind == 3) plan({shell("echo driven " + std::to_string(step))});
            if (kind == 2) fake.hold_left = 1;
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
                (rng() % 2 ? a : b).ok("response.create", {{"conversation", sid}, {"input", "and one more thing"}});
                if (at > 0) a.ok("maic.approval.answer", {{"session", sid}, {"approval", a.events[at]["id"]}, {"choice", "yes"}});
            }
            turns += a.until_idle(mark) > 0;
        }
        b.pump(200ms);
        expect(turns == 8, "eight driven turns ran to their end");
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
        mutate("a delta's text changed", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["delta"] = "fennec"; }, "");
        mutate("a field added inside maic", [&](std::vector<json>& r) { r[deltas[0]]["msg"]["params"]["maic"] = {{"note", "fine"}}; }, "");
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

    section("shutdown parks the sessions in the index");
    {
        engine->shutdown();
        engine.reset();
        Engine again(o);
        Recording after("restart");
        TestClient a(again, after, Origin::Local, "tui");
        a.hello();
        json entries = a.ok("maic.index.get")["entries"];
        expect(entries.size() == 1 && entries[0]["id"] == sid && entries[0]["state"] == "parked", "after a restart the session is listed parked");
        json entry = a.ok("maic.session.resume", {{"session", sid}});
        expect(entry["state"] == "live" && entry["turns"].get<int>() >= 5, "maic.session.resume loads it again, its turns counted from the transcript");
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
