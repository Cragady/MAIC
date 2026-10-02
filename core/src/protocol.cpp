// The engine protocol's schemas and ordering machines, from the protocol/ files built into the binary.
#include "maic/protocol.hpp"

#include "maic/jsonschema.hpp"
#include "protocol_files.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace maic::protocol {

using nlohmann::json;

namespace {

std::string escape_token(const std::string& s) {
    std::string out;
    for (char c : s) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return out;
}

// "schemas/../openai/subset.json" -> "openai/subset.json"
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::string dirname(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

// Every $ref in `node`, written relative to the file at `path`, becomes a pointer into the bundle.
void rewrite_refs(json& node, const std::string& path) {
    if (node.is_object()) {
        for (auto& [k, v] : node.items()) {
            if (k == "$ref" && v.is_string()) {
                std::string ref = v;
                size_t hash = ref.find('#');
                std::string target = hash == std::string::npos ? ref : ref.substr(0, hash);
                std::string fragment = hash == std::string::npos ? "" : ref.substr(hash + 1);
                std::string file = target.empty() ? path : normalize((dirname(path).empty() ? "" : dirname(path) + "/") + target);
                v = "#" + bundle_pointer(file) + fragment;
            } else {
                rewrite_refs(v, path);
            }
        }
    } else if (node.is_array()) {
        for (auto& v : node) rewrite_refs(v, path);
    }
}

json ref_to(const std::string& pointer) {
    return {{"$ref", "#" + pointer}};
}

}  // namespace

std::string bundle_pointer(const std::string& path) {
    return "/files/" + escape_token(path);
}

const Schemas& Schemas::get() {
    static const Schemas schemas;
    return schemas;
}

Schemas::Schemas() {
    bundle_ = {{"files", json::object()}};
    for (const auto& f : embedded_files()) {
        std::string text;
        for (const char* c : f.chunks) text += c;
        json doc = json::parse(text);
        rewrite_refs(doc, f.path);
        bundle_["files"][f.path] = std::move(doc);
    }
    const json& rpc = file("maic.openrpc.json");
    for (const auto& m : rpc.at("methods")) {
        Method method;
        method.name = m.at("name");
        method.notification = m.value("x-maic-notification", false);
        method.remote = m.value("x-maic-remote", true);
        for (const auto& p : m.value("x-maic-local-only-params", json::array())) method.local_only_params.insert(p.get<std::string>());
        if (m.contains("x-maic-params")) {
            method.params = m["x-maic-params"];
        } else {
            json props = json::object(), required = json::array();
            for (const auto& p : m.at("params")) {
                props[p.at("name").get<std::string>()] = p.at("schema");
                if (p.value("required", false)) required.push_back(p.at("name"));
            }
            method.params = {{"type", "object"}, {"properties", props}};
            if (!required.empty()) method.params["required"] = required;
        }
        if (m.contains("result")) method.result = m["result"].at("schema");
        if (!method.notification) method_order_.push_back(method.name);
        methods_[method.name] = std::move(method);
    }
    const std::string defs = "/$defs/";
    for (const auto& branch : file("schemas/event.schema.json").at("anyOf")) {
        std::string ref = branch.at("$ref");
        event_types_.push_back(ref.substr(ref.rfind(defs) + defs.size()));
    }
    const json& subset = file("openai/subset.json");
    for (const auto& branch : subset.at("components").at("schemas").at("ResponseStreamEvent").at("anyOf")) {
        std::string ref = branch.at("$ref");
        const json& schema = bundle_.at(json::json_pointer(ref.substr(1)));
        const json& type = schema.at("properties").at("type");
        if (type.contains("enum")) openai_events_[type["enum"][0].get<std::string>()] = ref.substr(1);
    }
}

const json& Schemas::file(const std::string& path) const {
    return bundle_.at("files").at(path);
}

const Schemas::Method* Schemas::method(const std::string& name) const {
    auto it = methods_.find(name);
    return it == methods_.end() ? nullptr : &it->second;
}

std::vector<std::string> Schemas::methods() const {
    return method_order_;
}

std::vector<std::string> Schemas::notifications() const {
    std::vector<std::string> out;
    for (const auto& [name, m] : methods_) {
        if (m.notification) out.push_back(name);
    }
    return out;
}

std::vector<std::string> Schemas::event_types() const {
    return event_types_;
}

std::string Schemas::against(const std::string& pointer, const json& value) const {
    return schema_error(bundle_, ref_to(pointer), value);
}

std::string Schemas::event_error(const json& event) const {
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) return "/type: is required";
    std::string type = event["type"];
    if (std::find(event_types_.begin(), event_types_.end(), type) == event_types_.end()) return "/type: no event of type " + json(type).dump() + " in event.schema.json";
    return against(bundle_pointer("schemas/event.schema.json") + "/$defs/" + type, event);
}

std::string Schemas::event_undeclared(const json& event) const {
    std::string type = event.value("type", "");
    if (std::find(event_types_.begin(), event_types_.end(), type) == event_types_.end()) return "";
    std::string where = schema_undeclared(bundle_, ref_to(bundle_pointer("schemas/event.schema.json") + "/$defs/" + type), event, {"maic"});
    return where.empty() ? "" : where + ": no schema declares it; MAIC's fields on an OpenAI-shaped object go in its maic object";
}

std::string Schemas::openai_event_error(const json& event) const {
    std::string type = event.value("type", "");
    auto it = openai_events_.find(type);
    if (it == openai_events_.end()) return "/type: " + json(type).dump() + " is not an event of OpenAI's ResponseStreamEvent";
    return against(it->second, event);
}

std::string Schemas::params_error(const std::string& name, const json& params) const {
    const Method* m = method(name);
    if (!m) return "(root): no method " + name + " in maic.openrpc.json";
    return schema_error(bundle_, m->params, params);
}

std::string Schemas::result_error(const std::string& name, const json& result) const {
    const Method* m = method(name);
    if (!m) return "(root): no method " + name + " in maic.openrpc.json";
    if (m->result.is_null()) return "(root): " + name + " is a notification and has no result";
    return schema_error(bundle_, m->result, result);
}

std::string Schemas::error_data_error(const json& data) const {
    return def_error("ErrorData", data);
}

std::string Schemas::def_error(const std::string& def, const json& value) const {
    return against(bundle_pointer("schemas/maic.schema.json") + "/$defs/" + def, value);
}

// ---------- the ordering machines ----------

namespace {

struct Transition {
    std::set<std::string> from, on;
    std::vector<std::pair<json::json_pointer, json>> when;  // pointer, allowed values
    std::vector<json::json_pointer> key;                   // empty: the machine's
    std::string to;
};

struct Machine {
    std::string name;
    bool stream = false;  // one instance per stream
    bool openai = false;
    bool one_open = false;
    std::string scope;
    std::vector<json::json_pointer> key;
    std::string initial;
    std::set<std::string> terminal;
    std::vector<std::string> closes;
    std::vector<Transition> transitions;
};

std::vector<json::json_pointer> pointers(const json& list) {
    std::vector<json::json_pointer> out;
    for (const auto& p : list) out.emplace_back(p.get<std::string>());
    return out;
}

const std::vector<Machine>& machines() {
    static const std::vector<Machine> all = [] {
        std::vector<Machine> out;
        for (const auto& m : Schemas::get().ordering().at("machines")) {
            Machine machine;
            machine.name = m.at("name");
            machine.stream = m.value("key", json()) == "stream";
            if (!machine.stream && m.contains("key")) machine.key = pointers(m["key"]);
            machine.openai = m.value("openai", false);
            machine.one_open = m.value("one_open", false);
            machine.scope = m.value("scope", "");
            machine.initial = m.at("initial");
            for (const auto& t : m.at("terminal")) machine.terminal.insert(t.get<std::string>());
            for (const auto& c : m.value("closes", json::array())) machine.closes.push_back(c);
            for (const auto& t : m.at("transitions")) {
                Transition tr;
                for (const auto& f : t.at("from")) tr.from.insert(f.get<std::string>());
                for (const auto& o : t.at("on")) tr.on.insert(o.get<std::string>());
                json when = t.value("when", json::object());
                for (const auto& [ptr, allowed] : when.items()) tr.when.emplace_back(json::json_pointer(ptr), allowed);
                if (t.contains("key")) tr.key = pointers(t["key"]);
                tr.to = t.at("to");
                machine.transitions.push_back(std::move(tr));
            }
            out.push_back(std::move(machine));
        }
        return out;
    }();
    return all;
}

bool holds(const Transition& t, const json& event) {
    for (const auto& [ptr, allowed] : t.when) {
        if (!event.contains(ptr)) return false;
        const json& v = event.at(ptr);
        if (std::find(allowed.begin(), allowed.end(), v) == allowed.end()) return false;
    }
    return true;
}

// The instance an event names: its key values joined; false when one is missing.
bool instance_key(const std::vector<json::json_pointer>& key, const json& event, std::string& out) {
    out.clear();
    for (const auto& p : key) {
        if (!event.contains(p)) return false;
        out += event.at(p).dump() + "\x1f";
    }
    return true;
}

bool is_terminal_response(const std::string& type) {
    return type == "response.completed" || type == "response.incomplete" || type == "response.failed" || type == "maic.response.cancelled";
}

const json& nothing() {
    static const json null;
    return null;
}

const json& at(const json& v, const char* ptr) {
    json::json_pointer p(ptr);
    return v.contains(p) ? v.at(p) : nothing();
}

}  // namespace

std::string rule_text(const std::string& id) {
    for (const auto& r : Schemas::get().ordering().at("rules")) {
        if (r.at("id") == id) return r.at("text");
    }
    return "";
}

std::string describe(const Violation& v) {
    std::string out = v.sequence_number >= 0 ? "#" + std::to_string(v.sequence_number) + " " : "";
    out += v.rule + ": " + v.detail;
    std::string text = rule_text(v.rule);
    if (!text.empty()) out += " (" + text + ")";
    return out;
}

StreamChecker::StreamChecker(bool openai_only) : openai_only_(openai_only) {}

void StreamChecker::join(long after) {
    joined_ = true;
    started_ = true;
    last_ = after;
}

void StreamChecker::replay_from(long starting_after) {
    if (!started_) {
        if (starting_after >= 0) join(starting_after);
        return;
    }
    replay_until_ = last_;
    if (starting_after > last_) last_ = starting_after;  // the client skipped some: the next event shows the gap
}

std::optional<Violation> StreamChecker::check(const json& event) {
    std::string type = event.value("type", "");
    long n = event.contains("sequence_number") && event["sequence_number"].is_number_integer() ? event["sequence_number"].get<long>() : -1;
    auto fail = [&](const std::string& rule, const std::string& detail) { return std::optional<Violation>(Violation{n, rule, type + ": " + detail}); };
    if (n < 0) return fail("seq.next", "no sequence_number");
    if (replay_until_ >= 0) {
        if (n <= replay_until_) return std::nullopt;
        replay_until_ = -1;
    }

    // Sequence numbers.
    long first = n;
    const json& merged = at(event, "/maic/merged_from");
    if (merged.is_number_integer()) first = merged.get<long>();
    if (!started_) {
        if (!openai_only_ && (n != 0 || type != "maic.session.state")) return fail("seq.start", "the load's first event is #" + std::to_string(n));
    } else if (n <= last_) {
        return fail("seq.repeat", "after #" + std::to_string(last_));
    } else if (!openai_only_ && first != last_ + 1) {
        return fail("seq.next", "#" + std::to_string(first) + " after #" + std::to_string(last_));
    }
    if (!event.contains("stream_id") || !event["stream_id"].is_string()) return fail("stream.id", "no stream_id");
    if (!stream_.empty() && event["stream_id"] != stream_) return fail("stream.id", "stream_id " + event["stream_id"].dump() + " on the stream of " + stream_);

    // The response, its items and the turn: the rules that are not one machine.
    std::string open = open_response_;
    std::string closing;  // a response this event ends
    bool turn_open = turn_open_;
    long turn = last_turn_;
    std::string last_response = last_response_;
    std::map<std::string, long> next_index;
    if (type == "response.created") {
        std::string rid = at(event, "/response/id").is_string() ? at(event, "/response/id").get<std::string>() : "";
        if (!open.empty()) {
            if (!openai_only_) return fail("response.one_open", "response " + rid + " while " + open + " is open");
            closing = open;  // the OpenAI-only view never sees a cancelled response end
        }
        if (!openai_only_) {
            const json& t = at(event, "/response/maic/turn");
            const json& prev = at(event, "/response/previous_response_id");
            if (t.is_number_integer()) {
                long k = t.get<long>();
                if (!turn_open && last_turn_ > 0 && k != last_turn_ + 1) {
                    return fail("turn.number", "turn " + std::to_string(k) + " opens after turn " + std::to_string(last_turn_));
                }
                if (turn_open && (k != last_turn_ || !prev.is_string() || prev != last_response_)) {
                    return fail("turn.number", "response " + rid + " in turn " + std::to_string(k) + " does not continue " + last_response_ + " of turn " + std::to_string(last_turn_));
                }
                turn = k;
            }
            turn_open = true;
        }
        open = rid;
        next_index[rid] = 0;
    } else if (is_terminal_response(type)) {
        std::string rid = at(event, "/response/id").is_string() ? at(event, "/response/id").get<std::string>() : "";
        closing = rid;
        if (rid == open) open.clear();
        last_response = rid;
        if (!openai_only_) turn_open = !(at(event, "/response/maic/final") == true);
    } else if (type == "response.output_item.added" && !open.empty()) {
        long want = next_output_index_.count(open) ? next_output_index_[open] : 0;
        const json& idx = at(event, "/output_index");
        if (!(joined_ && !seen_responses_.count(open)) && (!idx.is_number_integer() || idx.get<long>() != want)) {
            return fail("item.index", "output_index " + idx.dump() + " where " + std::to_string(want) + " comes next");
        }
        next_index[open] = want + 1;
    }

    // The machines.
    std::vector<std::tuple<std::string, std::string, std::string>> sets;
    for (const Machine& m : machines()) {
        if (openai_only_ && !m.openai) continue;
        std::vector<const Transition*> cands;
        for (const auto& t : m.transitions) {
            if (t.on.count(type) && holds(t, event)) cands.push_back(&t);
        }
        if (cands.empty()) continue;
        std::string key;
        if (!m.stream && !instance_key(cands[0]->key.empty() ? m.key : cands[0]->key, event, key)) continue;
        if (m.scope == "response") {
            std::string resp = type == "response.created" ? open : open_response_;
            if (resp.empty()) {
                if (joined_ && seen_responses_.empty()) continue;  // inside a response opened before the client joined
                return fail("item.in_response", "no response is open");
            }
            if (joined_ && !seen_responses_.count(resp) && resp != open) continue;
            key = resp + "\x1f" + key;
        }
        auto& instances = states_[m.name];
        auto it = instances.find(key);
        std::string state = it == instances.end() ? m.initial : it->second;
        const json& rid = at(event, "/response/id");
        bool unknown = joined_ && it == instances.end() && m.scope.empty() && (m.name != "response" || !rid.is_string() || !seen_responses_.count(rid.get<std::string>()));
        const Transition* chosen = nullptr;
        for (const Transition* t : cands) {
            if (t->from.count(state)) {
                chosen = t;
                break;
            }
        }
        if (!chosen) {
            if (unknown) continue;  // opened before the client joined
            std::string where = m.terminal.count(state) ? " after it reached " + state : " in state " + state;
            return fail("machine", "not a legal transition of the " + m.name + " machine" + where);
        }
        sets.emplace_back(m.name, key, chosen->to);
    }

    // A response ends after its items.
    if (!closing.empty() && !(openai_only_ && type == "response.created")) {
        for (const Machine& m : machines()) {
            if (m.name != "response") continue;
            for (const auto& child : m.closes) {
                std::string prefix = closing + "\x1f";
                auto& instances = states_[child];
                for (auto it = instances.lower_bound(prefix); it != instances.end() && it->first.rfind(prefix, 0) == 0; ++it) {
                    bool done = false;
                    for (const Machine& c : machines()) {
                        if (c.name == child) done = c.terminal.count(it->second) > 0;
                    }
                    for (const auto& [mn, k, to] : sets) {
                        if (mn == child && k == it->first) done = true;
                    }
                    if (!done) return fail("response.items_done", "response " + closing + " ends with a " + child + " still " + it->second);
                }
            }
        }
    }

    // Nothing broke: the stream moves past the event.
    started_ = true;
    last_ = n;
    if (stream_.empty()) stream_ = event["stream_id"];
    for (const auto& [mn, k, to] : sets) states_[mn][k] = to;
    if (type == "response.created") seen_responses_.insert(open);
    for (const auto& [r, i] : next_index) next_output_index_[r] = i;
    if (!closing.empty()) {
        if (openai_only_ && type == "response.created") states_["response"][json(closing).dump() + "\x1f"] = "cancelled";
        std::string prefix = closing + "\x1f";
        for (auto& [mn, instances] : states_) {
            if (mn == "response") continue;
            for (auto it = instances.lower_bound(prefix); it != instances.end() && it->first.rfind(prefix, 0) == 0;) it = instances.erase(it);
        }
        next_output_index_.erase(closing);
    }
    open_response_ = open;
    turn_open_ = turn_open;
    last_turn_ = turn;
    last_response_ = last_response;
    return std::nullopt;
}

std::optional<json> openai_view(const json& event) {
    if (event.value("type", "").rfind("maic.", 0) == 0) return std::nullopt;
    std::function<void(json&)> strip = [&](json& v) {
        if (v.is_object()) {
            v.erase("maic");
            for (auto& [k, sub] : v.items()) strip(sub);
        } else if (v.is_array()) {
            for (auto& sub : v) strip(sub);
        }
    };
    json out = event;
    strip(out);
    return out;
}

// ---------- recorded streams ----------

std::optional<Violation> Conformance::fail(Violation v) {
    if (!first_) first_ = std::move(v);
    return first_;
}

std::optional<Violation> Conformance::feed(const json& record) {
    if (first_) return first_;
    const Schemas& schemas = Schemas::get();
    if (!record.is_object() || !record.contains("msg") || !record["msg"].is_object()) return fail({-1, "schema.message", "a record without msg"});
    std::string dir = record.value("dir", ""), conn = record.value("conn", "");
    const json& msg = record["msg"];
    if (msg.value("jsonrpc", "") != "2.0") return fail({-1, "schema.message", "not a JSON-RPC 2.0 message: " + msg.dump().substr(0, 120)});
    if (dir == "in") {
        if (!msg.contains("method") || !msg.contains("id")) return std::nullopt;
        std::string method = msg["method"].is_string() ? msg["method"].get<std::string>() : "";
        json params = msg.value("params", json::object());
        std::string key = msg["id"].dump();
        if (pending_[conn].count(key)) return fail({-1, "request.answered", conn + " reused request id " + key + " while it was pending"});
        pending_[conn][key] = Request{method, params};
        // An unknown method is answered -32601; that is the engine's answer, not a malformed request.
        if (schemas.method(method)) {
            if (std::string e = schemas.params_error(method, params); !e.empty()) return fail({-1, "schema.message", method + " params " + e});
        }
        return std::nullopt;
    }
    if (dir == "out") return out(conn, msg);
    return fail({-1, "schema.message", "a record whose dir is neither in nor out"});
}

std::optional<Violation> Conformance::out(const std::string& conn, const json& msg) {
    const Schemas& schemas = Schemas::get();
    if (msg.contains("id")) {
        std::string key = msg["id"].dump();
        auto it = pending_[conn].find(key);
        if (it == pending_[conn].end()) return fail({-1, "request.answered", "an answer on " + conn + " to no pending request " + key});
        Request req = it->second;
        pending_[conn].erase(it);
        if (msg.contains("error")) {
            const json& err = msg["error"];
            if (err.contains("data")) {
                if (std::string e = schemas.error_data_error(err["data"]); !e.empty()) return fail({-1, "schema.message", req.method + " error data " + e});
            }
            if (req.method == "maic.session.subscribe" && err.contains("data") && err["data"].value("code", json()) == "maic_resync") {
                streams_[conn].erase(req.params.value("session", ""));
            }
            return std::nullopt;
        }
        if (!msg.contains("result")) return fail({-1, "schema.message", "an answer with neither result nor error"});
        const json& result = msg["result"];
        if (!schemas.method(req.method)) return std::nullopt;
        if (std::string e = schemas.result_error(req.method, result); !e.empty()) return fail({-1, "schema.message", req.method + " result " + e});
        if (req.method == "maic.session.attach") {
            StreamChecker c(openai_only_);
            c.join(result.value("sequence_number", -1L));
            streams_[conn].insert_or_assign(result["entry"]["id"].get<std::string>(), c);
        } else if (req.method == "maic.session.subscribe") {
            std::string session = req.params.value("session", "");
            long after = req.params.value("starting_after", -1L);
            long from = result.value("replay_from", after + 1);
            auto s = streams_[conn].find(session);
            if (s != streams_[conn].end()) {
                s->second.replay_from(after);
            } else {
                StreamChecker c(openai_only_);
                if (from > 0) c.join(from - 1);
                streams_[conn].emplace(session, c);
            }
        } else if (req.method == "maic.session.unsubscribe") {
            streams_[conn].erase(req.params.value("session", ""));
        }
        return std::nullopt;
    }
    std::string method = msg.value("method", "");
    json params = msg.value("params", json::object());
    if (method != "maic.event") {
        if (!schemas.method(method)) return fail({-1, "schema.message", "a notification of no method: " + method});
        if (std::string e = schemas.params_error(method, params); !e.empty()) return fail({-1, "schema.message", method + " params " + e});
        return std::nullopt;
    }
    ++events_;
    long n = params.value("sequence_number", -1L);
    json event = params;
    if (openai_only_) {
        auto view = openai_view(params);
        if (!view) return std::nullopt;
        event = *view;
        if (std::string e = schemas.openai_event_error(event); !e.empty()) return fail({n, "openai.view", event.value("type", "") + " " + e});
    } else {
        if (std::string e = schemas.event_error(event); !e.empty()) return fail({n, "schema.event", event.value("type", "") + " " + e});
        if (std::string e = schemas.event_undeclared(event); !e.empty()) return fail({n, "schema.extra", event.value("type", "") + " " + e});
    }
    std::string session = params.value("stream_id", "");
    auto s = streams_[conn].find(session);
    if (s == streams_[conn].end()) return fail({n, "stream.id", "an event of " + session + " on " + conn + ", which did not attach or subscribe to it"});
    if (auto v = s->second.check(event)) return fail(*v);
    return std::nullopt;
}

std::optional<Violation> Conformance::finish() {
    if (first_) return first_;
    for (const auto& [conn, reqs] : pending_) {
        for (const auto& [id, req] : reqs) return fail({-1, "request.answered", req.method + " " + id + " on " + conn + " was never answered"});
    }
    return std::nullopt;
}

Recorder::Recorder(const std::filesystem::path& file) : fd_(open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)) {}

Recorder::~Recorder() {
    if (fd_ >= 0) close(fd_);
}

std::optional<Violation> Recorder::add(const std::string& dir, const std::string& conn, const json& msg) {
    if (fd_ < 0) return std::nullopt;
    json r = {{"dir", dir}, {"conn", conn}, {"msg", msg}};
    std::string line = r.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    ssize_t n = write(fd_, line.data(), line.size());  // one write per record: a killed process leaves whole lines
    (void)n;
    if (violated_) return std::nullopt;
    auto v = check_.feed(r);
    violated_ = v.has_value();
    return v;
}

std::optional<Violation> check_file(const std::string& path, bool openai_only, size_t* events) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("can't read " + path);
    Conformance c(openai_only);
    std::string line;
    size_t n = 0;
    std::optional<Violation> v;
    while (!v && std::getline(in, line)) {
        ++n;
        if (line.empty()) continue;
        json record = json::parse(line, nullptr, false);
        if (record.is_discarded()) {
            v = Violation{-1, "schema.message", "line " + std::to_string(n) + " is not JSON"};
            break;
        }
        v = c.feed(record);
    }
    if (!v) v = c.finish();
    if (events) *events = c.events();
    return v;
}

}  // namespace maic::protocol
