#include "server.hpp"

#include "auth.hpp"
#include "home_link.hpp"
#include "tls.hpp"
#include "tunnel.hpp"

#include "maic/agent.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/session.hpp"
#include "maic/status.hpp"
#include "maic/tripwire.hpp"
#include "maic/trust.hpp"

#include "maic/http.hpp"
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace maic::server {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

const char* choice_name(Approval a) {
    switch (a) {
        case Approval::Yes: return "yes";
        case Approval::No: return "no";
        case Approval::Always: return "always";
        case Approval::Trip: return "trip";
    }
    return "no";
}

std::optional<Approval> parse_choice(const std::string& s) {
    if (s == "yes") return Approval::Yes;
    if (s == "no") return Approval::No;
    if (s == "always") return Approval::Always;
    if (s == "trip") return Approval::Trip;
    return std::nullopt;
}

// A handler throws one of these for a client error; anything else is a 500.
struct HttpError {
    int status;
    std::string message;
};

// One agent, its transcript, and the event log clients stream from. Events carry a `seq` so a client that lost
// its connection can pick up where it was (GET .../events?after=N).
struct Session {
    std::string id;
    fs::path workspace;
    std::unique_ptr<SessionLog> log;
    Agent agent;
    std::string created = utc_now();

    std::mutex mu;
    std::condition_variable cv;
    std::vector<json> events;
    bool running = false;
    int turns = 0;
    std::atomic<bool> cancel{false};
    std::thread worker;

    struct Pending {
        std::string id;
        ApprovalRequest request;
        std::optional<ApprovalAnswer> answer;
    };
    std::optional<Pending> pending;
    int approvals = 0;

    Session(fs::path ws, std::string model) : workspace(std::move(ws)), agent(workspace, std::move(model)) {}

    void push_locked(json e) {
        e["seq"] = events.size();
        events.push_back(std::move(e));
        cv.notify_all();
    }
    void push(json e) {
        std::lock_guard lock(mu);
        push_locked(std::move(e));
    }

    static json approval_json(const Pending& p) {
        const auto& r = p.request;
        return {{"type", "approval"}, {"id", p.id}, {"tool", r.tool}, {"summary", r.summary}, {"reason", r.reason},
                {"origin", r.origin == Origin::Remote ? "remote" : "local"}, {"always_covers", r.always_covers}, {"preview", r.preview}};
    }
};

// The agent's callbacks become events; ask() parks the worker thread until a client answers or the turn is interrupted.
class Events : public AgentEvents {
public:
    explicit Events(Session& s) : s_(s) {}
    void on_text(std::string_view delta, bool thinking) override { s_.push({{"type", "text"}, {"text", std::string(delta)}, {"thinking", thinking}}); }
    void on_tool_call(const std::string& summary) override { s_.push({{"type", "tool_call"}, {"summary", summary}}); }
    void on_tool_result(const std::string& text, bool ok) override { s_.push({{"type", "tool_result"}, {"text", text}, {"ok", ok}}); }
    void on_notice(const std::string& text) override { s_.push({{"type", "notice"}, {"text", text}}); }
    ApprovalAnswer ask(const ApprovalRequest& request) override {
        std::unique_lock lock(s_.mu);
        std::string id = "a" + std::to_string(++s_.approvals);
        s_.pending = Session::Pending{id, request, std::nullopt};
        s_.push_locked(Session::approval_json(*s_.pending));
        s_.cv.wait(lock, [&] { return s_.pending->answer.has_value() || s_.cancel.load(); });
        ApprovalAnswer answer = s_.pending->answer.value_or(ApprovalAnswer{Approval::No, "interrupted by the user"});
        s_.pending.reset();
        s_.push_locked({{"type", "approval_answered"}, {"id", id}, {"choice", choice_name(answer.choice)}});
        return answer;
    }

private:
    Session& s_;
};

}  // namespace

struct Server::Impl {
    explicit Impl(ServerOptions o) : options(std::move(o)), tokens(options.state / "tokens.json"), pairs(options.state / "pairs.json") {}

    ServerOptions options;
    std::unique_ptr<httplib::Server> srv;
    std::string host;
    int port = 0;
    bool tls = false;
    std::string fingerprint;
    std::vector<fs::path> roots;
    std::atomic<bool> stopping{false};

    std::mutex tokens_mu;
    TokenStore tokens;
    RateLimit rate;
    std::mutex audit_mu;
    std::mutex pairs_mu;
    PairStore pairs;
    std::unique_ptr<HomeLink> home;  // the outbound connection to server.relay, when one is set

    std::mutex sessions_mu;
    std::map<std::string, std::shared_ptr<Session>> sessions;

    std::optional<std::string> token_name(const httplib::Request& req) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.rfind("Bearer ", 0) != 0) return std::nullopt;
        std::lock_guard lock(tokens_mu);
        tokens.refresh();
        return tokens.verify(auth.substr(7));
    }

    void audit(const httplib::Request& req, const httplib::Response& res) {
        // A request out of the relay tunnel arrives from our own HomeLink on loopback; it names the phone.
        std::string source = req.remote_addr;
        if (req.has_header("X-Maic-Via") && (source == "127.0.0.1" || source == "::1")) source = req.get_header_value("X-Maic-Via");
        std::string line = utc_now() + " " + source + " " + token_name(req).value_or("-") + " " + req.method + " " + req.path + " " +
                           std::to_string(res.status) + "\n";
        std::lock_guard lock(audit_mu);
        int fd = open((options.state / "audit.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return;
        ssize_t n = write(fd, line.data(), line.size());
        (void)n;
        close(fd);
    }

    static void fail(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(dump({{"error", message}}), "application/json");
    }

    static void reply(httplib::Response& res, const json& body, int status = 200) {
        res.status = status;
        res.set_content(dump(body), "application/json");
    }

    static json body_of(const httplib::Request& req) {
        if (req.body.empty()) return json::object();
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object()) throw HttpError{400, "the request body must be a JSON object"};
        return j;
    }

    std::shared_ptr<Session> find(const std::string& id) {
        std::lock_guard lock(sessions_mu);
        auto it = sessions.find(id);
        if (it == sessions.end()) throw HttpError{404, "no session " + id};
        return it->second;
    }

    // Resolved with symlinks followed, then checked against every allowed root.
    fs::path allowed_workspace(const std::string& given) {
        std::error_code ec;
        fs::path ws = fs::weakly_canonical(given, ec);
        if (ec || !fs::is_directory(ws)) throw HttpError{400, "not a directory: " + given};
        for (const auto& root : roots) {
            auto rel = ws.lexically_relative(root);
            if (!rel.empty() && *rel.begin() != "..") return ws;
        }
        throw HttpError{403, "workspace outside the allowed roots (server.workspaces in settings): " + given};
    }

    std::shared_ptr<Session> make_session(const fs::path& ws, const std::string& model, Mode mode) {
        const Settings& st = options.settings;
        if (st.tripwire == "isolated") throw HttpError{403, "this MAIC runs isolated sessions (tripwire = isolated): it takes no remote work"};
        auto s = std::make_shared<Session>(ws, model);
        s->agent.providers = st.providers;
        s->agent.set_forbid(st.forbid);
        s->agent.set_permission(st.permission);
        s->agent.agents = st.agents;
        s->agent.presets = st.presets;
        s->agent.small_model = st.small_model;
        s->agent.reviewer_budget_tokens = st.reviewer_budget_tokens;
        s->agent.mode = mode;
        s->agent.think = st.think;
        s->agent.compaction.at = st.compact_at;
        s->agent.compaction.keep_results = st.compact_keep_results;
        s->agent.compaction.model = st.compact_model;
        s->agent.budget_tokens = st.budget_tokens;
        s->agent.set_instruction_names(st.instruction_files);
        return s;
    }

    json session_json(Session& s) {
        std::lock_guard lock(s.mu);
        json j = {{"id", s.id},      {"workspace", s.workspace.string()}, {"model", s.agent.model}, {"mode", std::string(mode_name(s.agent.mode))},
                  {"remote_model", s.agent.remote()}, {"running", s.running}, {"turns", s.turns}, {"created", s.created}, {"seq", s.events.size()},
                  {"transcript", s.log->path().string()}};
        j["pending_approval"] = s.pending && !s.pending->answer ? Session::approval_json(*s.pending) : json();
        return j;
    }

    // The event log folded for display: text deltas of one reply become one entry.
    static json transcript_json(const Session& s) {
        json out = json::array();
        for (const auto& e : s.events) {
            std::string type = e.value("type", "");
            if (type == "text" && !out.empty() && out.back()["type"] == "text" && out.back()["thinking"] == e["thinking"]) {
                out.back()["text"] = out.back()["text"].get<std::string>() + e["text"].get<std::string>();
                continue;
            }
            if (type == "approval" || type == "approval_answered" || type == "done") continue;
            out.push_back(e);
        }
        return out;
    }

    json status_json() {
        auto services = load_services(root_dir() / "services");
        StatusReport r = status_report(services);
        json svc = json::array();
        for (const auto& s : r.services) svc.push_back({{"name", s.name}, {"state", s.state}, {"runtime", s.runtime}, {"where", s.where}, {"detail", s.detail}});
        const Settings& st = options.settings;
        auto [provider, model_name] = resolve_model(st.providers, st.model);
        json roots_json = json::array();
        for (const auto& p : roots) roots_json.push_back(p.string());
        size_t count;
        {
            std::lock_guard lock(sessions_mu);
            count = sessions.size();
        }
        json relay;
        if (home) {
            HomeLink::State hs = home->state();
            relay = {{"url", st.server.relay}, {"connected", hs.connected}, {"since", hs.since}, {"last_connected", hs.last_connected}, {"error", hs.error}};
        }
        return {{"harness", {{"tripped", r.tripped}, {"reason", r.tripwire}}},
                {"relay", relay},
                {"services", svc},
                {"model", st.model},
                {"provider", provider.name},
                {"remote_model", provider.remote()},
                {"mode", st.mode},
                {"sessions", count},
                {"listen", host + ":" + std::to_string(port)},
                {"tls", tls},
                {"fingerprint", fingerprint},
                {"workspaces", roots_json},
                {"version", MAIC_VERSION}};
    }

    void start_turn(const std::shared_ptr<Session>& s, const std::string& text) {
        if (s->worker.joinable()) s->worker.join();
        s->worker = std::thread([s, text] {
            Events events(*s);
            try {
                s->agent.submit(text, Origin::Remote, events, s->cancel);
            } catch (const std::exception& e) {
                s->push({{"type", "error"}, {"text", e.what()}});
            }
            Agent::UsageReport u = s->agent.usage();
            std::lock_guard lock(s->mu);
            s->running = false;
            ++s->turns;
            s->push_locked({{"type", "done"},
                            {"interrupted", s->cancel.exchange(false)},
                            {"usage", {{"input", u.total_input}, {"output", u.total_output}, {"calls", u.calls}, {"context", u.last.context}, {"last_input", u.last.input}}}});
        });
    }

    // Server-sent events from `from` on: everything already logged, then whatever the running turn adds, until
    // the session is idle again. A comment line every 15 s keeps the connection open through silence.
    void stream(std::shared_ptr<Session> s, size_t from, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider("text/event-stream", [this, s, next = from](size_t, httplib::DataSink& sink) mutable {
            std::string out;
            bool finished = false;
            {
                std::unique_lock lock(s->mu);
                s->cv.wait_for(lock, 15s, [&] { return next < s->events.size() || stopping.load(); });
                for (; next < s->events.size(); ++next) out += "data: " + dump(s->events[next]) + "\n\n";
                finished = !s->running;
            }
            if (stopping.load()) return false;
            if (out.empty()) out = ": keepalive\n\n";
            if (!sink.write(out.data(), out.size())) return false;
            if (finished) sink.done();
            return true;
        });
    }

    void routes() {
        srv->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/") return httplib::Server::HandlerResponse::Unhandled;
            if (rate.blocked(req.remote_addr)) {
                fail(res, 429, "too many failed attempts from this address; try again in a minute");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!token_name(req)) {
                rate.failed(req.remote_addr);
                res.set_header("WWW-Authenticate", "Bearer");
                fail(res, 401, "a bearer token from `maic server token new` is required");
                return httplib::Server::HandlerResponse::Handled;
            }
            rate.succeeded(req.remote_addr);
            return httplib::Server::HandlerResponse::Unhandled;
        });
        srv->set_logger([this](const httplib::Request& req, const httplib::Response& res) { audit(req, res); });
        srv->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const HttpError& e) {
                fail(res, e.status, e.message);
            } catch (const std::exception& e) {
                fail(res, 500, e.what());
            }
        });

        srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream in(options.web);
            if (!in) {
                res.status = 404;
                res.set_content("no web client at " + options.web.string() + "\n", "text/plain");
                return;
            }
            res.set_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "text/html; charset=utf-8");
        });

        srv->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) { reply(res, status_json()); });

        srv->Get("/api/sessions", [this](const httplib::Request&, httplib::Response& res) {
            std::vector<std::shared_ptr<Session>> all;
            {
                std::lock_guard lock(sessions_mu);
                for (const auto& [id, s] : sessions) all.push_back(s);
            }
            json out = json::array();
            for (const auto& s : all) out.push_back(session_json(*s));
            reply(res, {{"sessions", out}});
        });

        srv->Post("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            std::string mode_str = body.value("mode", options.settings.mode);
            auto mode = parse_mode(mode_str);
            if (!mode) throw HttpError{400, "unknown mode '" + mode_str + "' (manual, auto-read, edit, auto, plan)"};
            std::shared_ptr<Session> s;
            if (body.contains("resume")) {
                auto info = find_session(body["resume"].get<std::string>());
                if (!info) throw HttpError{404, "no session matching " + body["resume"].get<std::string>()};
                fs::path ws = allowed_workspace(info->workspace);
                LoadedSession old = load_session(info->path);
                s = make_session(ws, body.value("model", old.model.empty() ? options.settings.model : old.model), *mode);
                s->log = std::make_unique<SessionLog>(SessionLog::Reopen{}, info->path);
                s->id = info->id;
                s->agent.set_log(s->log.get());
                s->agent.restore(old.messages);
                for (const auto& t : old.transcript) {
                    if (t.type == "assistant") s->push({{"type", "text"}, {"text", t.text}, {"thinking", false}});
                    else if (t.type == "tool_call") s->push({{"type", "tool_call"}, {"summary", t.text}});
                    else if (t.type == "tool_result") s->push({{"type", "tool_result"}, {"text", t.text}, {"ok", t.ok}});
                    else s->push({{"type", t.type}, {"text", t.text}});
                }
            } else {
                fs::path ws = allowed_workspace(body.value("workspace", roots.front().string()));
                s = make_session(ws, body.value("model", options.settings.model), *mode);
                s->log = std::make_unique<SessionLog>("server", resolve_sessions_home(options.settings, ws));
                s->id = s->log->path().stem().string();
                s->agent.set_log(s->log.get());
            }
            {
                std::lock_guard lock(sessions_mu);
                sessions[s->id] = s;
            }
            reply(res, session_json(*s), 201);
        });

        srv->Get(R"(/api/sessions/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            json j = session_json(*s);
            Agent::UsageReport u = s->agent.usage();
            j["usage"] = {{"input", u.total_input}, {"output", u.total_output}, {"calls", u.calls}, {"context", u.last.context}, {"last_input", u.last.input}};
            std::lock_guard lock(s->mu);
            j["entries"] = transcript_json(*s);
            reply(res, j);
        });

        srv->Get(R"(/api/sessions/([^/]+)/events)", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            size_t after = 0;
            if (req.has_param("after")) after = std::stoul(req.get_param_value("after"));
            stream(s, after, res);
        });

        srv->Post(R"(/api/sessions/([^/]+)/messages)", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            std::string text = body_of(req).value("text", "");
            if (text.empty()) throw HttpError{400, "text is empty"};
            size_t from;
            bool start = false;
            {
                std::lock_guard lock(s->mu);
                from = s->events.size();
                if (s->running) {
                    // Mid-turn, like typing while the agent works: reaches the model at its next call.
                    s->agent.post_message(text);
                    s->agent.deliver_now();
                    s->push_locked({{"type", "user"}, {"text", text}, {"queued", true}});
                } else {
                    s->running = true;
                    s->cancel = false;
                    s->push_locked({{"type", "user"}, {"text", text}});
                    start = true;
                }
            }
            if (start) start_turn(s, text);
            stream(s, from, res);
        });

        srv->Post(R"(/api/sessions/([^/]+)/approvals/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            json body = body_of(req);
            auto choice = parse_choice(body.value("choice", ""));
            if (!choice) throw HttpError{400, "choice must be yes, no, always or trip"};
            std::lock_guard lock(s->mu);
            if (!s->pending || s->pending->id != req.matches[2].str()) throw HttpError{404, "no approval " + req.matches[2].str() + " is waiting"};
            if (s->pending->answer) throw HttpError{409, "already answered"};
            s->pending->answer = ApprovalAnswer{*choice, body.value("feedback", "")};
            s->cv.notify_all();
            reply(res, {{"id", s->pending->id}, {"choice", choice_name(*choice)}});
        });

        srv->Post(R"(/api/sessions/([^/]+)/interrupt)", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            std::lock_guard lock(s->mu);
            if (!s->running) {
                reply(res, {{"running", false}});
                return;
            }
            s->cancel = true;
            if (s->pending && !s->pending->answer) s->pending->answer = ApprovalAnswer{Approval::No, "interrupted by the user"};
            s->cv.notify_all();
            reply(res, {{"running", true}, {"interrupting", true}});
        });

        srv->Post(R"(/api/sessions/([^/]+)/mode)", [this](const httplib::Request& req, httplib::Response& res) {
            auto s = find(req.matches[1]);
            std::string name = body_of(req).value("mode", "");
            auto mode = parse_mode(name);
            if (!mode) throw HttpError{400, "unknown mode '" + name + "' (manual, auto-read, edit, auto, plan)"};
            s->agent.mode = *mode;
            s->push({{"type", "mode"}, {"mode", std::string(mode_name(*mode))}});
            reply(res, session_json(*s));
        });

        // Trust from a paired device (docs/harness.md, Trust): only with a step-up proof the registered verifier
        // accepts. Until accounts exist none is registered and every request is refused. Each request, done or
        // refused, is a line in <state>/trust-audit.log naming the device.
        srv->Post("/api/trust", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            try {
                std::string done = remote_trust_change(token_name(req).value_or("-"), body.value("step_up", ""), body.value("action", ""), body.value("path", ""), body.value("level", ""),
                                                       body.value("lua", ""));
                reply(res, {{"done", done}});
            } catch (const std::runtime_error& e) {
                throw HttpError{403, e.what()};
            }
        });

        // The pairing exchange, on the LAN only: the phone proves it saw the code `maic server pair` printed and
        // leaves its public key; it gets ours, the pairing id and the relay to use. Nothing here reaches the
        // relay, and the relay has no counterpart to this route.
        srv->Post("/api/pair", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            std::string code = body.value("code", ""), name = body.value("name", "phone");
            auto pk = key_from_b64url(body.value("public_key", ""));
            if (!pk) throw HttpError{400, "public_key must be 32 bytes of base64url"};
            if (options.settings.server.relay.empty()) throw HttpError{409, "no relay is configured (server.relay in settings)"};
            if (auto why = claim_pairing_offer(options.state / "pairing.json", code)) throw HttpError{403, *why};
            std::lock_guard lock(pairs_mu);
            pairs.refresh();
            pairs.add(name, *pk);
            reply(res, {{"name", name}, {"pairing_id", pairs.pairing_id()}, {"relay", options.settings.server.relay}, {"public_key", b64url(pairs.keypair().pk)}}, 201);
        });

        // The panic button works from a phone; the reset does not exist here (it needs the local sudo password).
        srv->Post("/api/trip", [this](const httplib::Request& req, httplib::Response& res) {
            std::string reason = body_of(req).value("reason", "tripped from a remote client");
            trip_tripwire("remote: " + reason);
            std::vector<std::shared_ptr<Session>> all;
            {
                std::lock_guard lock(sessions_mu);
                for (const auto& [id, s] : sessions) all.push_back(s);
            }
            for (const auto& s : all) {
                std::lock_guard lock(s->mu);
                if (s->running) s->cancel = true;
                if (s->pending && !s->pending->answer) s->pending->answer = ApprovalAnswer{Approval::Trip, ""};
                s->cv.notify_all();
            }
            reply(res, {{"tripped", true}});
        });
    }
};

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Server::~Server() {
    stop();
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard lock(impl_->sessions_mu);
        for (const auto& [id, s] : impl_->sessions) all.push_back(s);
    }
    for (const auto& s : all) {
        if (s->worker.joinable()) s->worker.join();
    }
    std::lock_guard lock(impl_->sessions_mu);
    impl_->sessions.clear();
}

int Server::bind() {
    Impl& im = *impl_;
    const ServerOptions& o = im.options;
    std::string listen = o.listen;
    size_t colon = listen.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("listen address must be ADDR:PORT: " + listen);
    im.host = listen.substr(0, colon);
    if (im.host.size() > 1 && im.host.front() == '[' && im.host.back() == ']') im.host = im.host.substr(1, im.host.size() - 2);
    int port = std::stoi(listen.substr(colon + 1));
    if (im.host.empty()) im.host = "0.0.0.0";

    fs::create_directories(o.state);
    std::error_code ec;
    fs::permissions(o.state, fs::perms::owner_all, fs::perm_options::replace, ec);
    for (const auto& w : o.workspaces) im.roots.push_back(fs::weakly_canonical(w, ec));
    if (im.roots.empty()) throw std::runtime_error("no allowed workspace root");

    bool loopback = im.host == "localhost" || im.host == "::1" || im.host.rfind("127.", 0) == 0;
    fs::path cert = o.settings.server.cert, key = o.settings.server.key;
    if (cert.empty() != key.empty()) throw std::runtime_error("server.cert and server.key go together");
    // Off loopback TLS is not optional; on loopback it is used when a pair is configured.
    if (loopback && cert.empty()) {
        im.srv = std::make_unique<httplib::Server>();
    } else {
        TlsPair pair;
        if (cert.empty()) {
            bool any = im.host == "0.0.0.0" || im.host == "::";
            std::vector<std::string> hosts = any ? interface_addresses() : std::vector<std::string>{im.host};
            char name[256] = {};
            if (gethostname(name, sizeof(name) - 1) == 0 && name[0]) hosts.push_back(name);
            pair = ensure_self_signed(o.state / "cert.pem", o.state / "key.pem", hosts);
        } else {
            pair = {cert, key, cert_fingerprint(cert)};
        }
        auto ssl = std::make_unique<httplib::SSLServer>(pair.cert.c_str(), pair.key.c_str());
        if (!ssl->is_valid()) throw std::runtime_error("can't load the TLS pair " + pair.cert.string() + " / " + pair.key.string());
        im.srv = std::move(ssl);
        im.tls = tls_ = true;
        im.fingerprint = fingerprint_ = pair.fingerprint;
    }
    im.srv->new_task_queue = [] { return new httplib::ThreadPool(16); };
    im.srv->set_keep_alive_max_count(1000);
    im.routes();
    im.port = port == 0 ? im.srv->bind_to_any_port(im.host) : (im.srv->bind_to_port(im.host, port) ? port : -1);
    if (im.port <= 0) throw std::runtime_error("can't listen on " + im.host + ":" + std::to_string(port) + " (in use, or not an address of this machine)");
    return im.port;
}

void Server::run() {
    Impl& im = *impl_;
    const ServerSettings& ss = im.options.settings.server;
    if (!ss.relay.empty()) {
        bool any = im.host == "0.0.0.0" || im.host == "::";
        HomeLinkOptions h;
        h.relay = ss.relay;
        h.relay_cert = ss.relay_cert;
        h.pairs_file = im.options.state / "pairs.json";
        h.status_file = im.options.state / "relay.json";
        h.loopback = std::string(im.tls ? "https://" : "http://") + (any ? "127.0.0.1" : im.host) + ":" + std::to_string(im.port);
        im.home = std::make_unique<HomeLink>(std::move(h));
        im.home->start();
    }
    im.srv->listen_after_bind();
}

void Server::stop() {
    Impl& im = *impl_;
    im.stopping = true;
    if (im.home) im.home->stop();
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard lock(im.sessions_mu);
        for (const auto& [id, s] : im.sessions) all.push_back(s);
    }
    for (const auto& s : all) {
        std::lock_guard lock(s->mu);
        s->cancel = true;
        if (s->pending && !s->pending->answer) s->pending->answer = ApprovalAnswer{Approval::No, "the server is shutting down"};
        s->cv.notify_all();
    }
    if (im.srv) im.srv->stop();
}

}  // namespace maic::server
