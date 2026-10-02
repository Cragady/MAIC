// protocol/ against itself, OpenAI's pinned description and the design (docs/design/engine-protocol.md section 15):
// the schemas use only keywords the validator checks, every method and event type is OpenAI's or MAIC's own, no
// MAIC schema takes an OpenAI name, ordering.json names real events, the engine's dispatcher and event list match
// the OpenRPC document and the union, and every JSON example in the design validates.
#include "check.hpp"

#include "maic/engine.hpp"
#include "maic/jsonschema.hpp"
#include "maic/protocol.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using namespace maic;
using nlohmann::json;

namespace {

json load(const std::string& path) {
    std::ifstream in(path);
    return json::parse(in);
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Every schema inside a MAIC file, for schema_unsupported: the $defs, the union, each method's params and result.
void schemas_in(const json& doc, const std::string& where, std::vector<std::pair<std::string, json>>& out) {
    if (doc.contains("$defs")) {
        for (const auto& [k, v] : doc["$defs"].items()) out.emplace_back(where + "#/$defs/" + k, v);
    }
    if (doc.contains("anyOf")) out.emplace_back(where + "#/anyOf", json{{"anyOf", doc["anyOf"]}});
    if (doc.contains("methods")) {
        for (const auto& m : doc["methods"]) {
            for (const auto& p : m["params"]) out.emplace_back(where + " " + m["name"].get<std::string>() + " " + p["name"].get<std::string>(), p["schema"]);
            if (m.contains("result")) out.emplace_back(where + " " + m["name"].get<std::string>() + " result", m["result"]["schema"]);
            if (m.contains("x-maic-params")) out.emplace_back(where + " " + m["name"].get<std::string>() + " x-maic-params", m["x-maic-params"]);
        }
    }
}

struct Example {
    size_t line;
    std::vector<std::pair<char, std::string>> messages;  // '>' sent to the engine, '<' from it, ' ' a bare value
};

// The ```json blocks of a markdown file, with their messages: a line starting with → or ← begins one, an indented
// line continues it. A block right after `<!-- example: skip ... -->` is left out.
std::vector<Example> examples(const std::string& markdown, size_t& skipped) {
    std::vector<Example> out;
    std::istringstream in(markdown);
    std::string line, prev;
    size_t n = 0;
    bool inside = false, skip = false;
    Example cur;
    while (std::getline(in, line)) {
        ++n;
        if (!inside && line == "```json") {
            inside = true;
            skip = prev.rfind("<!-- example: skip", 0) == 0;
            cur = Example{n, {}};
        } else if (inside && line == "```") {
            inside = false;
            if (skip) ++skipped;
            else out.push_back(cur);
        } else if (inside) {
            const std::string to = "→ ", from = "← ";
            if (line.rfind(to, 0) == 0) cur.messages.push_back({'>', line.substr(to.size())});
            else if (line.rfind(from, 0) == 0) cur.messages.push_back({'<', line.substr(from.size())});
            else if (!cur.messages.empty()) cur.messages.back().second += line;
            else cur.messages.push_back({' ', line});
        }
        prev = line;
    }
    return out;
}

}  // namespace

int main() {
    const std::string root = MAIC_PROTOCOL;
    const protocol::Schemas& schemas = protocol::Schemas::get();
    json spec = load(root + "/openai/openapi.json");
    const json& openai_schemas = spec["components"]["schemas"];

    section("the files and the keywords they use");
    {
        for (const char* f : {"maic.openrpc.json", "schemas/maic.schema.json", "schemas/event.schema.json", "ordering.json", "openai/subset.json"}) {
            expect(load(root + "/" + f) == load(root + "/" + f) && !schemas.file(f).is_null(), std::string(f) + " is built in");
        }
        std::vector<std::pair<std::string, json>> all;
        for (const char* f : {"maic.openrpc.json", "schemas/maic.schema.json", "schemas/event.schema.json"}) schemas_in(load(root + "/" + f), f, all);
        std::string bad;
        for (const auto& [where, s] : all) {
            std::string e = schema_unsupported(s);
            if (!e.empty() && bad.empty()) bad = where + " " + e;
        }
        expect(bad.empty() && all.size() > 60, "every MAIC schema (" + std::to_string(all.size()) + ") uses only keywords the validator checks" + (bad.empty() ? "" : ": " + bad));
        expect(load(root + "/maic.openrpc.json")["openrpc"] == "1.3.2", "the methods are an OpenRPC 1.3 document");
        expect(load(root + "/schemas/event.schema.json")["$schema"] == "https://json-schema.org/draft/2020-12/schema" &&
                   load(root + "/schemas/maic.schema.json")["$schema"] == "https://json-schema.org/draft/2020-12/schema",
               "the payloads are JSON Schema 2020-12");
    }

    section("names: OpenAI's exactly, MAIC's own under maic.");
    {
        std::set<std::string> operations;
        for (const auto& [path, ops] : spec["paths"].items()) {
            for (const auto& [verb, op] : ops.items()) {
                if (op.is_object() && op.contains("operationId")) operations.insert(op["operationId"].get<std::string>());
            }
        }
        std::set<std::string> client_events;
        for (const auto& [name, s] : openai_schemas.items()) {
            if (name.rfind("ResponsesClientEvent", 0) != 0) continue;
            std::string dump = s.dump();
            for (const char* t : {"response.create", "response.steer"}) {
                if (dump.find(std::string("\"") + t + "\"") != std::string::npos) client_events.insert(t);
            }
        }
        std::string bad;
        for (const auto& m : schemas.methods()) {
            if (m.rfind("maic.", 0) != 0 && !operations.count(m) && !client_events.count(m)) bad += " " + m;
        }
        expect(bad.empty() && operations.count("createConversation") && client_events.count("response.create"),
               "every method not under maic. is an operationId or a Responses WebSocket client event of the pinned spec" + (bad.empty() ? "" : ":" + bad));
        bad.clear();
        for (const auto& t : schemas.event_types()) {
            if (t.rfind("maic.", 0) == 0) continue;
            json probe = {{"type", t}};
            if (schemas.openai_event_error(probe).find("is not an event of OpenAI") != std::string::npos) bad += " " + t;
        }
        expect(bad.empty(), "every event type not under maic. is one of OpenAI's ResponseStreamEvent" + bad);
        bad.clear();
        for (const char* f : {"schemas/maic.schema.json", "schemas/event.schema.json"}) {
            json doc = load(root + "/" + f);
            for (const auto& [name, s] : doc["$defs"].items()) {
                if (openai_schemas.contains(name)) bad += " " + name;
            }
        }
        expect(bad.empty(), "no MAIC schema takes a name OpenAI's description defines" + bad);
        for (const char* n : {"maic.event", "maic.index", "maic.engine"}) {
            expect(schemas.method(n) && schemas.method(n)->notification, std::string(n) + " is a notification: a method with no result");
        }
    }

    section("the engine and the documents match");
    {
        std::vector<std::string> dispatched = Engine::methods(), described = schemas.methods();
        std::sort(dispatched.begin(), dispatched.end());
        std::sort(described.begin(), described.end());
        std::string missing, extra;
        for (const auto& m : dispatched) {
            if (!has(described, m)) missing += " " + m;
        }
        for (const auto& m : described) {
            if (!has(dispatched, m)) extra += " " + m;
        }
        expect(missing.empty() && extra.empty(), "every method in the dispatcher is in maic.openrpc.json and the reverse" + missing + extra);
        std::vector<std::string> emitted = Engine::event_types(), union_ = schemas.event_types();
        std::sort(emitted.begin(), emitted.end());
        std::sort(union_.begin(), union_.end());
        expect(emitted == union_, "every event type the engine emits is in event.schema.json's union and the reverse (" + std::to_string(union_.size()) + ")");
    }

    section("ordering.json names real events and reachable states");
    {
        const json& ordering = schemas.ordering();
        std::vector<std::string> types = schemas.event_types();
        std::set<std::string> ids;
        for (const auto& r : ordering["rules"]) ids.insert(r["id"].get<std::string>());
        expect(ids.size() == ordering["rules"].size() && ids.count("machine") && ids.count("seq.next"), "every rule has its own id");
        std::string bad;
        for (const auto& m : ordering["machines"]) {
            std::set<std::string> reached = {m["initial"].get<std::string>()};
            for (const auto& t : m["transitions"]) {
                for (const auto& on : t["on"]) {
                    // Not sent yet (response.queued, response.incomplete) is fine when it is OpenAI's.
                    bool openai = schemas.openai_event_error({{"type", on}}).find("is not an event of OpenAI") == std::string::npos;
                    if (!has(types, on) && !openai) bad += " " + m["name"].get<std::string>() + ":" + on.get<std::string>();
                }
                reached.insert(t["to"].get<std::string>());
            }
            for (const auto& t : m["transitions"]) {
                for (const auto& f : t["from"]) {
                    if (!reached.count(f)) bad += " " + m["name"].get<std::string>() + " from " + f.get<std::string>();
                }
            }
            for (const auto& term : m["terminal"]) {
                if (!reached.count(term)) bad += " " + m["name"].get<std::string>() + " terminal " + term.get<std::string>();
            }
        }
        expect(bad.empty(), "every event a machine moves on is in the union or one of OpenAI's, and every state is reachable" + bad);
        expect(!protocol::rule_text("response.items_done").empty(), "a rule's text is found by its id");
    }

    section("the schemas refuse what they should");
    {
        json notice = {{"type", "maic.notice"}, {"sequence_number", 3}, {"stream_id", "s1"}, {"text", "hi"}, {"level", "info"}};
        expect(schemas.event_error(notice).empty() && schemas.event_undeclared(notice).empty(), "a maic.notice fits");
        json no_stream = notice;
        no_stream.erase("stream_id");
        expect(schemas.event_error(no_stream) == "/stream_id: is required", "an event without stream_id is refused, naming it");
        json loud = notice;
        loud["level"] = "loud";
        expect(schemas.event_error(loud).rfind("/level: must be one of", 0) == 0, "a level outside the enum is refused");
        json delta = {{"type", "response.output_text.delta"}, {"sequence_number", 9}, {"stream_id", "s1"}, {"item_id", "~7"}, {"output_index", 0},
                      {"content_index", 0}, {"delta", "hi"}, {"logprobs", json::array()}};
        expect(schemas.event_error(delta).empty() && schemas.event_undeclared(delta).empty(), "OpenAI's text delta, with stream_id, fits");
        json beside = delta;
        beside["judged_by"] = "rules";
        expect(schemas.event_error(beside).empty() && schemas.event_undeclared(beside).rfind("/judged_by: no schema declares it", 0) == 0,
               "a MAIC field beside OpenAI's fits OpenAI's open object but is refused as undeclared");
        beside.erase("judged_by");
        beside["maic"] = {{"judged_by", "rules"}};
        expect(schemas.event_undeclared(beside).empty(), "inside the maic object it passes");
        expect(!schemas.event_error({{"type", "response.steer.accepted"}, {"sequence_number", 1}, {"stream_id", "s"}}).empty(),
               "an event the engine does not send yet is not in the union");
        expect(schemas.params_error("maic.approval.answer", {{"session", "s"}, {"approval", "a1"}, {"choice", "maybe"}}).rfind("/choice: must be one of", 0) == 0,
               "a request's params are checked against the OpenRPC method");
        expect(schemas.params_error("response.create", {{"conversation", "s"}, {"input", "hello"}}).empty() &&
                   !schemas.params_error("response.create", {{"conversation", "s"}, {"input", 7}}).empty(),
               "response.create takes OpenAI's InputParam");
        json view = *protocol::openai_view({{"type", "response.output_item.done"}, {"item", {{"type", "message"}, {"maic", {{"ok", true}}}}}, {"maic", 1}});
        expect(!view.contains("maic") && !view["item"].contains("maic") && !protocol::openai_view(notice), "the OpenAI-only view drops maic.* events and every maic object");
    }

    section("the stream checker on a hand-made stream");
    {
        auto ev = [](long n, json e) {
            e["sequence_number"] = n;
            e["stream_id"] = "s1";
            return e;
        };
        json resp = {{"id", "s1.r1"}, {"maic", {{"turn", 1}, {"origin", "local"}}}};
        json done = resp;
        done["maic"]["final"] = true;
        std::vector<json> good = {
            ev(0, {{"type", "maic.session.state"}, {"state", "live"}, {"activity", "idle"}, {"waiting", nullptr}}),
            ev(1, {{"type", "response.created"}, {"response", resp}}),
            ev(2, {{"type", "response.in_progress"}, {"response", resp}}),
            ev(3, {{"type", "response.output_item.added"}, {"output_index", 0}, {"item", {{"type", "message"}}}}),
            ev(4, {{"type", "response.content_part.added"}, {"output_index", 0}, {"content_index", 0}}),
            ev(5, {{"type", "response.output_text.delta"}, {"output_index", 0}, {"content_index", 0}}),
            ev(6, {{"type", "response.output_text.done"}, {"output_index", 0}, {"content_index", 0}}),
            ev(7, {{"type", "response.content_part.done"}, {"output_index", 0}, {"content_index", 0}}),
            ev(8, {{"type", "response.output_item.done"}, {"output_index", 0}, {"item", {{"type", "message"}}}}),
            ev(9, {{"type", "response.completed"}, {"response", done}}),
        };
        auto run = [&](const std::vector<json>& stream) -> std::string {
            protocol::StreamChecker c;
            for (const auto& e : stream) {
                if (auto v = c.check(e)) return v->rule;
            }
            return "";
        };
        expect(run(good).empty(), "a plain reply passes");
        auto without = good;
        without.erase(without.begin() + 8);
        expect(run(without) == "seq.next", "a dropped event is a gap");
        auto early = good;
        std::swap(early[7], early[8]);
        early[7]["sequence_number"] = 7;
        early[8]["sequence_number"] = 8;
        expect(run(early) == "machine", "an item closed before its part is an illegal transition");
        auto open = good;
        open.erase(open.begin() + 8);
        open.back()["sequence_number"] = 8;
        expect(run(open) == "response.items_done", "a response that ends with an item open is refused");
        auto twice = good;
        twice.back() = ev(9, {{"type", "response.created"}, {"response", {{"id", "s1.r2"}, {"maic", {{"turn", 2}, {"origin", "local"}}}}}});
        expect(run(twice) == "response.one_open", "a second response while one is open is refused");
        auto late = good;
        late.push_back(ev(10, {{"type", "response.output_text.delta"}, {"output_index", 0}, {"content_index", 0}}));
        expect(run(late) == "item.in_response", "a delta after the response ended is refused");
        protocol::StreamChecker joined;
        joined.join(4);
        bool ok = true;
        for (size_t i = 5; i < good.size(); ++i) ok = ok && !joined.check(good[i]);
        expect(ok, "a client that joined mid-stream checks what it saw open and close, and lets the rest pass");
    }

    section("every JSON example in the design validates");
    {
        size_t skipped = 0;
        auto blocks = examples(slurp(MAIC_DOCS "/design/engine-protocol.md"), skipped);
        size_t checked = 0;
        for (const auto& block : blocks) {
            std::map<std::string, std::string> requests;  // id -> method
            for (const auto& [dir, text] : block.messages) {
                std::string where = "engine-protocol.md:" + std::to_string(block.line);
                json msg = json::parse(text, nullptr, false);
                if (msg.is_discarded()) {
                    expect(false, where + " is JSON (write it out in full, or mark it `<!-- example: skip ... -->`): " + text.substr(0, 80));
                    continue;
                }
                std::string e;
                if (!msg.contains("jsonrpc")) {
                    e = schemas.def_error("Entry", msg);
                    where += " (an index entry)";
                } else if (msg.contains("method") && msg.contains("id")) {
                    requests[msg["id"].dump()] = msg["method"];
                    e = schemas.params_error(msg["method"], msg.value("params", json::object()));
                    where += " " + msg["method"].get<std::string>();
                } else if (msg.contains("method")) {
                    const json& p = msg["params"];
                    if (msg["method"] == "maic.event") {
                        e = schemas.event_error(p);
                        if (e.empty()) e = schemas.event_undeclared(p);
                        where += " " + p.value("type", "");
                    } else {
                        e = schemas.params_error(msg["method"], p);
                        where += " " + msg["method"].get<std::string>();
                    }
                } else if (msg.contains("error")) {
                    e = schemas.error_data_error(msg["error"].value("data", json::object()));
                    where += " error";
                } else {
                    auto it = requests.find(msg["id"].dump());
                    e = it == requests.end() ? "an answer to no request in its block" : schemas.result_error(it->second, msg["result"]);
                    where += " result" + (it == requests.end() ? "" : " of " + it->second);
                }
                expect(e.empty(), where + " fits" + (e.empty() ? "" : ": " + e));
                ++checked;
            }
        }
        expect(checked >= 12 && skipped == 1, std::to_string(checked) + " examples checked, " + std::to_string(skipped) + " block marked as an excerpt (maic.steer, step 7)");
    }

    return finish();
}
