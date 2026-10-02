#pragma once

#include "maic/agent.hpp"
#include "maic/settings.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace maic {

// What an engine starts with. Its transport builds one: maic-server (every client remote), the TUI and `maic -p`
// in-process, `maic --rpc` on stdio, the daemon (`maic daemon run`) on its socket.
struct EngineOptions {
    Settings settings;                              // what each session's Agent is set up from: providers, model, mode, ...
    bool mode_asked = false;                        // settings.mode is the host's --mode: auto from it starts in any workspace
    std::vector<std::filesystem::path> workspaces;  // where a remote client may open or resume a session (server.workspaces)
    std::string kind = "engine";                    // the transcripts' kind ("server" for maic-server's sessions)
    bool titles = false;                            // small_model titles each session it opens after its first turn
    std::filesystem::path index_file;               // the session index, <state>/engine/index.json for the daemon; "" keeps none
    bool keeps_sessions = false;                    // sessions outlive their clients (the daemon, maic-server): a quit can leave one working
    std::filesystem::path protocol_log;             // where the guarded tier writes what it finds; "" is <state>/engine/protocol.log
    std::string tier = "guarded";                   // for what names no session (protocol_tier); each session resolves its own
    size_t ring_events = 10000;                     // each session's event ring, in memory only (section 5)
    size_t ring_bytes = 8 << 20;
    // What `:cd` reads in the directory it moves to; unset, the settings files there. The TUI adds its flags.
    std::function<Settings(const std::filesystem::path&)> settings_at;
    // The settings a session createConversation or maic.session.resume opens in a workspace starts from; unset,
    // `settings`. The daemon serves every directory, so it reads each one's settings files.
    std::function<Settings(const std::filesystem::path&)> settings_for;
    // What a local host adds to each session createConversation or maic.session.resume opens, after the engine's
    // own setup and before its history is restored: `maic --rpc` sets it up as the TUI sets up its own.
    std::function<void(Agent&, const Settings&)> setup;
};

// A session an in-process host set up itself (Engine::open_local): what `maic`, `maic -r`, `--fork-at` and the
// command line's flags make of it, which no protocol method can say before step 12's create, fork and resume.
struct LocalSession {
    std::filesystem::path workspace;
    Settings settings;                                // the session's own: its `:` commands read and change them
    std::unique_ptr<SessionLog> log;                  // new, a fork, or reopened to append
    std::function<void(Agent&, SessionLog&)> setup;   // configures the agent, gives it the log, restores a history
    bool titles = true;                               // small_model titles it after its first turn
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
    // What a client that goes away does to the session in its focus (`:q`, maic.session.leave): `as` (bg, park or
    // stop), else the session's leave.quit for an idle or a working one, unless another client has it in focus.
    // Without `keeps_sessions`, what would stay running does what leave.no_daemon says, as does every session in
    // the background. The daemon's connections and `maic --rpc` end this way.
    void leave(const std::string& client, const std::string& as = "");

    // In-process only: opens a session its host set up, `setup` running on the new session's Agent before any
    // client can reach it. The session opens with maic.session.state by `client` (a local connection); the host
    // then attaches as any client does. Returns the session id; throws what `setup` throws.
    std::string open_local(const std::string& client, LocalSession session);

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

// An agent set up from settings as every front end sets up its session's: providers and context windows, the
// harness and reviewer, models and presets, compaction, the budget, kept outputs, instruction files, the operator
// text and prefill, rules, permission, agents, forbidden terms, bans and sampling. The mode, the transcript and
// anything that is one front end's (an nvim host, a restored history) are the caller's. Throws when a system
// prompt or prefill file cannot be read.
void configure_agent(Agent& agent, const Settings& settings);
// The sampling the agent's current provider gets: settings.sampling, the provider's own, then `live` over them
// (:sampling and the command line; a null value unsets a key). Also whether the operator note rides in each turn.
void apply_sampling(Agent& agent, const Settings& settings, const nlohmann::json& live = nlohmann::json::object());

// The error a failed turn shows. A transport failure to a local provider adds what to do about the service
// behind it (start it, link a model), from the service state on that port.
std::string failure_text(const Agent& agent, const std::exception& e);

}  // namespace maic
