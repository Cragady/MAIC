#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The engine protocol's schemas and ordering machines (protocol/, docs/design/engine-protocol.md section 15): one
// source for the engine's runtime checks, the tests and `maic protocol check`.
namespace maic::protocol {

// protocol/ as built into the binary: maic.openrpc.json, schemas/*.json, ordering.json and openai/subset.json,
// bundled into one document so every $ref resolves in it (a reference to another file becomes a pointer into that
// file's place in the bundle). Validation is maic/jsonschema.hpp's.
class Schemas {
public:
    static const Schemas& get();

    const nlohmann::json& bundle() const { return bundle_; }
    const nlohmann::json& file(const std::string& path) const;  // as written, "schemas/event.schema.json"
    const nlohmann::json& ordering() const { return file("ordering.json"); }

    struct Method {
        std::string name;
        bool notification = false;
        bool remote = true;
        std::set<std::string> local_only_params;
        nlohmann::json params;  // the schema of the whole params object
        nlohmann::json result;  // null for a notification
    };
    const Method* method(const std::string& name) const;
    std::vector<std::string> methods() const;        // requests, in the document's order
    std::vector<std::string> notifications() const;  // maic.event, maic.index, maic.engine
    std::vector<std::string> event_types() const;    // the union of event.schema.json, in its order

    // "" when the value fits, else "<pointer>: <problem>" (schema_error's form).
    std::string event_error(const nlohmann::json& event) const;         // event.schema.json, the branch for its type
    std::string event_undeclared(const nlohmann::json& event) const;    // a field no schema declares, outside `maic`
    std::string openai_event_error(const nlohmann::json& event) const;  // OpenAI's own schema for the type
    std::string params_error(const std::string& method, const nlohmann::json& params) const;
    std::string result_error(const std::string& method, const nlohmann::json& result) const;
    std::string error_data_error(const nlohmann::json& data) const;  // a JSON-RPC error's data: OpenAI's Error
    // A schema of maic.schema.json's $defs, by name ("Entry").
    std::string def_error(const std::string& def, const nlohmann::json& value) const;

private:
    Schemas();
    std::string against(const std::string& pointer, const nlohmann::json& value) const;

    nlohmann::json bundle_;
    std::map<std::string, Method> methods_;
    std::vector<std::string> method_order_;
    std::vector<std::string> event_types_;
    std::map<std::string, std::string> openai_events_;  // type -> pointer to OpenAI's schema for it
};

// The pointer to `path` inside the bundle: "schemas/event.schema.json" -> "/files/schemas~1event.schema.json".
std::string bundle_pointer(const std::string& path);

struct Violation {
    long sequence_number = -1;  // of the event, -1 when the fault is not an event's
    std::string rule;           // an id from ordering.json's rules
    std::string detail;         // what happened
};
std::string rule_text(const std::string& id);  // the rule's text from ordering.json
std::string describe(const Violation& v);       // "#N rule: detail (text)"

// One session stream's order: sequence numbers, the machines of ordering.json, turns. Schemas are checked apart.
// A violation leaves the state as it was, so an engine that withholds the event can go on.
class StreamChecker {
public:
    // openai_only: the view an OpenAI-only client has (every maic.* event and maic object removed): sequence numbers
    // only rise, and only the machines marked `openai` run; a response the view never sees end is closed by the next
    // one's response.created (open question 25: a cancelled response shows to such a client as cancelled on getResponse).
    explicit StreamChecker(bool openai_only = false);

    std::optional<Violation> check(const nlohmann::json& event);

    // The client holds everything up to `after` already (it attached or subscribed mid-stream): objects opened
    // before that are not checked, everything opened after is.
    void join(long after);
    // A resubscribe with starting_after: events up to what was already checked are a replay and pass unchecked.
    void replay_from(long starting_after);
    long last() const { return last_; }

private:
    struct Pending;
    bool openai_only_;
    bool started_ = false;
    bool joined_ = false;
    long last_ = -1;
    long replay_until_ = -1;  // events numbered up to this are a replay
    std::string stream_;
    std::map<std::string, std::map<std::string, std::string>> states_;  // machine -> instance -> state
    std::string open_response_;
    std::set<std::string> seen_responses_;
    std::map<std::string, long> next_output_index_;  // response -> the next output_index
    long last_turn_ = 0;
    bool turn_open_ = false;
    std::string last_response_;
};

// The OpenAI-only view of an event: nullopt for a maic.* event, else the event with every `maic` object removed.
std::optional<nlohmann::json> openai_view(const nlohmann::json& event);

// A recorded exchange, one record per message: {"dir": "in" (client to engine) | "out", "conn": "c1", "msg": {...}}.
// Checks each message against its schema, every request answered once on its connection, and each connection's
// view of each session's events against the order (a StreamChecker per connection and session, joined where the
// connection attached or subscribed). The first violation is reported; later records are not checked.
class Conformance {
public:
    explicit Conformance(bool openai_only = false) : openai_only_(openai_only) {}
    std::optional<Violation> feed(const nlohmann::json& record);
    std::optional<Violation> finish();  // requests never answered
    size_t events() const { return events_; }

private:
    struct Request {
        std::string method;
        nlohmann::json params;
    };
    std::optional<Violation> fail(Violation v);
    std::optional<Violation> out(const std::string& conn, const nlohmann::json& msg);
    bool openai_only_;
    std::optional<Violation> first_;
    std::map<std::string, std::map<std::string, Request>> pending_;      // conn -> request id -> request
    std::map<std::string, std::map<std::string, StreamChecker>> streams_;  // conn -> session -> its view
    size_t events_ = 0;
};

// Checks a recorded stream file (JSON lines as above). The first violation, or nullopt when it conforms;
// `events` is set to how many events it held.
std::optional<Violation> check_file(const std::string& path, bool openai_only, size_t* events = nullptr);

}  // namespace maic::protocol
