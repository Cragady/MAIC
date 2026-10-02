#pragma once

#include "maic/agent.hpp"
#include "maic/settings.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace maic {

// What an engine starts with. Its transport builds one: maic-server now (every client remote); the TUI in-process
// (step 6), `maic --rpc` on stdio (step 8) and the daemon on its socket (step 13) later.
struct EngineOptions {
    Settings settings;                              // what each session's Agent is set up from: providers, model, mode, ...
    std::vector<std::filesystem::path> workspaces;  // where a remote client may open or resume a session (server.workspaces)
    std::string kind = "engine";                    // the transcripts' kind ("server" for maic-server's sessions)
    std::filesystem::path index_file;               // the session index, <state>/engine/index.json for the daemon; "" keeps none
    std::filesystem::path protocol_log;             // where the guarded tier writes what it finds; "" is <state>/engine/protocol.log
    std::string tier = "guarded";                   // open or guarded; airtight needs the conformance stamp (step 18)
    size_t ring_events = 10000;                     // each session's event ring, in memory only (section 5)
    size_t ring_bytes = 8 << 20;
};

// The engine of docs/design/engine-protocol.md: the session table, each session's Agent, SessionLog and ordered event
// stream (a ring for `starting_after` resume), approvals and questions, the session index, and the dispatcher from
// JSON-RPC method to handler. It turns AgentEvents into OpenAI's Responses events (MAIC's own under `maic.`), and
// checks every message against protocol/ at the session's tier: `open` checks nothing, `guarded` logs what fails
// to protocol.log and tells local clients once per kind.
//
// Transports hold connections. A connection is stamped by its transport with an origin and a name; a client never
// says where it is. Everything a client sends goes through call(); what the engine sends unasked (maic.event,
// maic.index, maic.engine) waits in the connection's queue for take(), so the engine never waits on a client: a
// connection that falls 2 MiB behind gets tool output as skip counts and text deltas merged, and one 8 MiB behind is
// closed with maic_too_slow (section 4).
class Engine {
public:
    explicit Engine(EngineOptions options);
    ~Engine();  // shutdown()

    // A connection. `via` names the transport for the hello's path: "in-process", "stdio", "http", ...
    // `wake`, when set, is called (from whichever thread sent) each time the connection's queue gains a message;
    // it must not call into the engine.
    std::string connect(Origin origin, std::string name, std::string via, std::function<void()> wake = {});
    void disconnect(const std::string& client);

    // One JSON-RPC 2.0 message from the client: the answer, or null for a notification.
    nlohmann::json call(const std::string& client, const nlohmann::json& message);

    // The messages waiting for the client, in order; waits up to `wait` for the first one.
    std::vector<nlohmann::json> take(const std::string& client, std::chrono::milliseconds wait);
    // Why the engine closed the connection ("maic_too_slow"), or "" while it is open.
    std::string closed(const std::string& client) const;

    // Interrupts every running turn, answers what waits for a person with no, waits for the turns to end, and
    // leaves the sessions parked in the index. Idempotent.
    void shutdown();

    static std::vector<std::string> methods();      // what the dispatcher answers
    static std::vector<std::string> event_types();  // every event type the engine can send

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace maic
