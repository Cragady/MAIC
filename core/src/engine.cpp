// The engine: sessions, their event streams and the protocol dispatcher (docs/design/engine-protocol.md).
#include "maic/engine.hpp"

#include "maic/full_output.hpp"
#include "maic/paths.hpp"
#include "maic/protocol.hpp"
#include "maic/session.hpp"
#include "maic/status.hpp"
#include "maic/service.hpp"
#include "maic/tripwire.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kProtocol = 1;
constexpr size_t kMaxMessage = 1 << 20;          // the relay's frame cap; section 1
constexpr size_t kSlowQueue = 2 << 20;           // behind this, tool output becomes skips and deltas merge
constexpr size_t kTooSlow = 8 << 20;             // behind this, the connection is closed
constexpr double kRemoteOutputRate = 64 * 1024;  // bytes a second of tool output per session, remote connections

// The event types the engine sends; protocol_schema_test holds this to event.schema.json's union.
const std::vector<std::string> kEventTypes = {
    "response.created", "response.in_progress", "response.completed", "response.failed",
    "response.output_item.added", "response.output_item.done", "response.content_part.added", "response.content_part.done",
    "response.output_text.delta", "response.output_text.done", "response.reasoning_text.delta", "response.reasoning_text.done",
    "response.function_call_arguments.done", "response.shell_call_command.added", "response.shell_call_command.done",
    "response.shell_call_output_content.delta", "response.shell_call_output_content.done",
    "maic.response.cancelled", "maic.input.added", "maic.approval.requested", "maic.approval.answered", "maic.question.asked",
    "maic.question.answered", "maic.tool.output.delta", "maic.file.written", "maic.notice", "maic.todo.updated",
    "maic.session.state", "maic.session.settings", "maic.usage.updated"};

// A request the engine refuses: a JSON-RPC error whose data is OpenAI's error object.
struct RpcError : std::exception {
    RpcError(int code, std::string data_code, std::string message, std::string param)
        : code(code), data_code(std::move(data_code)), message(std::move(message)), param(std::move(param)) {}
    int code;
    std::string data_code;  // OpenAI's code where OpenAI defines the condition, maic_... otherwise; "" for null
    std::string message;
    std::string param;
    std::string type = "invalid_request_error";
    const char* what() const noexcept override { return message.c_str(); }
};

RpcError refuse(std::string code, std::string message, std::string param = "") {
    return RpcError(-32000, std::move(code), std::move(message), std::move(param));
}
RpcError bad_params(std::string message, std::string param = "") {
    return RpcError(-32602, "", std::move(message), std::move(param));
}

json error_reply(const json& id, int code, const std::string& data_code, const std::string& message, const std::string& param,
                 const std::string& type = "invalid_request_error") {
    json data = {{"type", type}, {"code", data_code.empty() ? json() : json(data_code)}, {"message", message}, {"param", param.empty() ? json() : json(param)}};
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}, {"data", data}}}};
}

std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string random_id(size_t n) {
    static const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937 rng(std::random_device{}());
    std::string out;
    for (size_t i = 0; i < n; ++i) out += chars[rng() % 36];
    return out;
}

// RFC 3339 local time with its offset, as session records are stamped.
std::string local_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    std::string s = buf;
    if (s.size() > 2) s.insert(s.size() - 2, ":");
    return s;
}

const char* origin_name(Origin o) {
    return o == Origin::Remote ? "remote" : "local";
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

json usage_json(long input, long output) {
    return {{"input_tokens", input}, {"input_tokens_details", {{"cached_tokens", 0}, {"cache_write_tokens", 0}}}, {"output_tokens", output},
            {"output_tokens_details", {{"reasoning_tokens", 0}}}, {"total_tokens", input + output}};
}

// OpenAI's shell output for a run_shell result: "exit code N\n<output>", a timeout, or a call that did not run.
json shell_output(const std::string& text) {
    json outcome;
    int code = 0;
    if (std::sscanf(text.c_str(), "exit code %d", &code) == 1) outcome = {{"type", "exit"}, {"exit_code", code}};
    else if (text.rfind("terminated: the command exceeded its timeout", 0) == 0) outcome = {{"type", "timeout"}};
    else outcome = {{"type", "exit"}, {"exit_code", -1}};
    return json::array({{{"stdout", text}, {"stderr", ""}, {"outcome", outcome}}});
}

bool inside(const fs::path& dir, const std::vector<fs::path>& roots) {
    for (const auto& root : roots) {
        auto rel = dir.lexically_relative(root);
        if (!rel.empty() && *rel.begin() != "..") return true;
    }
    return false;
}

// Messages (and the one that ends with an error) a client is sent.
struct Client {
    std::string id, name, via;
    Origin origin = Origin::Local;
    bool hello = false;
    std::function<void()> wake;

    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<json, size_t>> queue;  // the message and its size
    size_t bytes = 0;
    std::string closed;
    std::set<std::string> sessions;  // subscribed to
    bool index = false;              // subscribed to maic.index
    struct Bucket {
        double budget = kRemoteOutputRate;
        Clock::time_point at = Clock::now();
    };
    std::map<std::string, Bucket> buckets;  // per session: tool output not yet spent this second

    json by() const { return {{"client", id}, {"name", name}, {"origin", origin_name(origin)}}; }

    void push(json msg, size_t size) {
        std::function<void()> w;
        {
            std::lock_guard lock(mu);
            if (!closed.empty()) return;
            if (bytes + size > kTooSlow) {
                closed = "maic_too_slow";
                queue.clear();
                bytes = 0;
            } else {
                queue.emplace_back(std::move(msg), size);
                bytes += size;
            }
            w = wake;
        }
        cv.notify_all();
        if (w) w();
    }

    // One session event. Tool output past the remote ceiling or a 2 MiB backlog goes as a skip count under the same
    // sequence_number; a text delta behind a 2 MiB backlog merges into the queued one of its item (maic.merged_from).
    void event(const json& e, size_t size) {
        const std::string type = e.value("type", "");
        bool shell = type == "response.shell_call_output_content.delta", script = type == "maic.tool.output.delta";
        std::optional<json> changed;
        {
            std::lock_guard lock(mu);
            if (!closed.empty()) return;
            if ((shell || script) && !e.contains("/maic/skipped"_json_pointer) && !e.contains("skipped")) {
                size_t data = shell ? e["delta"].value("stdout", "").size() + e["delta"].value("stderr", "").size() : e.value("data", "").size();
                bool skip = bytes > kSlowQueue;
                if (origin == Origin::Remote) {
                    auto& b = buckets[e.value("stream_id", "")];
                    auto now = Clock::now();
                    b.budget = std::min(kRemoteOutputRate, b.budget + kRemoteOutputRate * std::chrono::duration<double>(now - b.at).count());
                    b.at = now;
                    if (!skip && static_cast<double>(data) > b.budget) skip = true;
                    if (!skip) b.budget -= static_cast<double>(data);
                }
                if (skip && data) {
                    json s = e;
                    if (shell) {
                        s["delta"] = {{"stdout", ""}, {"stderr", ""}};
                        s["maic"]["skipped"] = data;
                    } else {
                        s.erase("data");
                        s["skipped"] = data;
                    }
                    changed = std::move(s);
                }
            } else if ((type == "response.output_text.delta" || type == "response.reasoning_text.delta") && bytes > kSlowQueue && !queue.empty()) {
                json& last = queue.back().first["params"];
                if (queue.back().first.value("method", "") == "maic.event" && last.value("type", "") == type && last.value("item_id", "") == e.value("item_id", "") &&
                    last.value("content_index", -1) == e.value("content_index", -1)) {
                    long from = last.contains("/maic/merged_from"_json_pointer) ? last["maic"]["merged_from"].get<long>() : last["sequence_number"].get<long>();
                    std::string more = e.value("delta", "");
                    last["delta"] = last["delta"].get<std::string>() + more;
                    last["sequence_number"] = e["sequence_number"];
                    last["maic"]["merged_from"] = from;
                    queue.back().second += more.size();
                    bytes += more.size();
                    return;
                }
            }
        }
        const json& out = changed ? *changed : e;
        push({{"jsonrpc", "2.0"}, {"method", "maic.event"}, {"params", out}}, changed ? dump(out).size() + 48 : size + 48);
    }
};

// A person's answer, waited for by the agent's worker.
struct PendingApproval {
    json event;  // maic.approval.requested
    std::optional<ApprovalAnswer> answer;
    json by;
};
struct PendingQuestion {
    json event;  // maic.question.asked
    std::optional<std::string> answer;
    json by;
};

// The response being run: what its events carry.
struct Run {
    std::string id;
    int turn = 0;
    long created_at = 0;
    Origin origin = Origin::Local;
    json output = json::array();
    Agent::UsageReport usage_before;
};

struct Session {
    std::string id;
    fs::path workspace;
    std::unique_ptr<SessionLog> log;
    Agent agent;
    std::string created = local_now();
    std::string title;

    std::mutex mu;
    std::condition_variable cv;
    // The stream: numbered from 0 for each load.
    std::string load = random_id(4);
    long next = 0;
    std::deque<std::pair<json, size_t>> ring;
    size_t ring_bytes = 0;
    protocol::StreamChecker order;
    std::vector<std::shared_ptr<Client>> subscribers;

    std::string state = "live", activity = "idle";
    json waiting;
    std::string last_activity = local_now();
    int turns = 0;  // turns so far, the transcript's included
    bool running = false;
    std::atomic<bool> cancel{false};
    json cancel_by;
    std::thread worker;
    std::optional<Run> run;
    std::map<std::string, PendingApproval> approvals;
    std::set<std::string> answered;  // approvals and questions already answered, for maic_already_answered
    std::map<std::string, PendingQuestion> questions;
    json todo = json::array();

    Session(fs::path ws, std::string model) : workspace(std::move(ws)), agent(workspace, std::move(model)) {}
};

}  // namespace

struct Engine::Impl {
    explicit Impl(EngineOptions o) : options(std::move(o)), epoch(random_id(6)) {
        if (options.tier != "open" && options.tier != "guarded") {
            throw std::runtime_error("protocol tier " + options.tier + " is not available here (airtight needs a build with the conformance stamp)");
        }
        if (options.protocol_log.empty()) options.protocol_log = state_dir() / "engine" / "protocol.log";
        std::error_code ec;
        for (auto& w : options.workspaces) w = fs::weakly_canonical(w, ec);
        load_index();
    }

    EngineOptions options;
    std::string epoch;
    std::atomic<long> clients_made{0}, approvals_made{0}, questions_made{0};
    std::atomic<bool> stopped{false};

    std::mutex mu;  // sessions and clients; never held while taking a session's lock
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::map<std::string, std::shared_ptr<Client>> clients;

    std::mutex index_mu;  // the index entries and the clients that follow them
    std::map<std::string, json> entries;  // the local form, transcript included
    std::vector<std::weak_ptr<Client>> index_clients;

    std::mutex log_mu;
    std::set<std::string> noticed;  // fault kinds already told to local clients

    using Handler = json (Impl::*)(Client&, const json&);
    static const std::map<std::string, Handler>& handlers();

    // ---------- connections ----------

    std::shared_ptr<Client> client(const std::string& id) {
        std::lock_guard lock(mu);
        auto it = clients.find(id);
        if (it == clients.end()) throw std::runtime_error("no connection " + id);
        return it->second;
    }

    std::shared_ptr<Session> session(const std::string& id) {
        std::lock_guard lock(mu);
        auto it = sessions.find(id);
        if (it == sessions.end()) throw refuse("maic_not_found", "no session " + id + " is loaded", "session");
        return it->second;
    }

    // ---------- the guarded tier ----------

    void fault(const std::string& kind, const std::string& rule, const std::string& detail, const std::string& session = "", long seq = -1) {
        json line = {{"time", local_now()}, {"kind", kind}, {"rule", rule}, {"detail", detail}};
        if (!session.empty()) line["session"] = session;
        if (seq >= 0) line["sequence_number"] = seq;
        bool first;
        {
            std::lock_guard lock(log_mu);
            std::error_code ec;
            fs::create_directories(options.protocol_log.parent_path(), ec);
            fs::permissions(options.protocol_log.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
            int fd = open(options.protocol_log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd >= 0) {
                std::string s = dump(line) + "\n";
                ssize_t n = write(fd, s.data(), s.size());
                (void)n;
                close(fd);
            }
            first = noticed.insert(kind + " " + rule).second;
        }
        if (!first) return;
        json msg = {{"jsonrpc", "2.0"}, {"method", "maic.engine"},
                    {"params", {{"notice", "protocol: " + kind + " " + rule + ": " + detail + " (logged in " + options.protocol_log.string() + ")"}, {"level", "warn"}}}};
        std::vector<std::shared_ptr<Client>> local;
        {
            std::lock_guard lock(mu);
            for (const auto& [id, c] : clients) {
                if (c->origin == Origin::Local && c->hello) local.push_back(c);
            }
        }
        for (const auto& c : local) c->push(msg, dump(msg).size());
    }

    // ---------- the stream ----------

    // Numbers, checks and sends one event of `s`; the caller holds s.mu.
    void emit(Session& s, json e) {
        e["sequence_number"] = s.next;
        e["stream_id"] = s.id;
        if (options.tier != "open") {
            const auto& schemas = protocol::Schemas::get();
            std::string bad = schemas.event_error(e);
            if (bad.empty()) bad = schemas.event_undeclared(e);
            if (!bad.empty()) fault("event", "schema.event", e.value("type", "") + " " + bad, s.id, s.next);
            if (auto v = s.order.check(e)) fault("event", v->rule, v->detail, s.id, s.next);
        }
        size_t size = dump(e).size();
        for (const auto& c : s.subscribers) c->event(e, size);
        s.ring.emplace_back(std::move(e), size);
        s.ring_bytes += size;
        while (!s.ring.empty() && (s.ring.size() > options.ring_events || s.ring_bytes > options.ring_bytes)) {
            s.ring_bytes -= s.ring.front().second;
            s.ring.pop_front();
        }
        ++s.next;
    }

    void set_activity(Session& s, const std::string& activity, json waiting = nullptr) {
        if (s.activity == activity && s.waiting == waiting) return;
        s.activity = activity;
        s.waiting = std::move(waiting);
        s.last_activity = local_now();
        emit(s, {{"type", "maic.session.state"}, {"state", s.state}, {"activity", s.activity}, {"waiting", s.waiting}});
        index_changed(s);
    }

    // ---------- the index ----------

    json entry(Session& s) {
        Agent::UsageReport u = s.agent.usage();
        (void)u;
        json e = {{"id", s.id},
                  {"title", s.title},
                  {"workspace", s.workspace.string()},
                  {"kind", "main"},
                  {"parent", nullptr},
                  {"state", s.state},
                  {"activity", s.activity},
                  {"model", s.agent.model},
                  {"remote_model", s.agent.remote()},
                  {"mode", std::string(mode_name(s.agent.mode))},
                  {"tier", options.tier},
                  {"created", s.created},
                  {"last_activity", s.last_activity},
                  {"turns", s.turns},
                  {"unseen", false},
                  {"queued", s.agent.queued()},
                  {"response", s.run ? json(s.run->id) : json()},
                  {"waiting", s.waiting}};
        if (s.log) e["transcript"] = s.log->path().string();
        return e;
    }

    static json for_client(json e, const Client& c) {
        if (c.origin == Origin::Remote) e.erase("transcript");
        return e;
    }

    void index_changed(Session& s) {
        json e = entry(s);
        std::vector<std::shared_ptr<Client>> to;
        {
            std::lock_guard lock(index_mu);
            entries[s.id] = e;
            save_index();
            for (auto it = index_clients.begin(); it != index_clients.end();) {
                if (auto c = it->lock()) {
                    to.push_back(c);
                    ++it;
                } else {
                    it = index_clients.erase(it);
                }
            }
        }
        for (const auto& c : to) {
            json msg = {{"jsonrpc", "2.0"}, {"method", "maic.index"}, {"params", {{"entry", for_client(e, *c)}}}};
            c->push(msg, dump(msg).size());
        }
    }

    // Written through a temporary file and a rename, 0600; the caller holds index_mu.
    void save_index() {
        if (options.index_file.empty()) return;
        std::error_code ec;
        fs::create_directories(options.index_file.parent_path(), ec);
        fs::permissions(options.index_file.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
        json list = json::array();
        for (const auto& [id, e] : entries) list.push_back(e);
        fs::path tmp = options.index_file;
        tmp += ".tmp";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return;
        std::string text = dump({{"version", 1}, {"entries", list}}) + "\n";
        bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
        ok = fsync(fd) == 0 && ok;
        close(fd);
        if (ok) fs::rename(tmp, options.index_file, ec);
    }

    // Parked sessions survive a restart: what the file lists comes back as parked entries.
    void load_index() {
        if (options.index_file.empty()) return;
        std::ifstream in(options.index_file);
        if (!in) return;
        json doc = json::parse(in, nullptr, false);
        if (!doc.is_object() || !doc.contains("entries") || !doc["entries"].is_array()) return;
        for (auto e : doc["entries"]) {
            if (!e.is_object() || !e.contains("id") || !e["id"].is_string()) continue;
            e["state"] = "parked";
            e["activity"] = "idle";
            e["waiting"] = nullptr;
            e["response"] = nullptr;
            entries[e["id"].get<std::string>()] = e;
        }
    }

    // ---------- sessions ----------

    // As the TUI and headless set up their agent; auto under a dumb harness starts at edit unless dumb_auto_ok says
    // otherwise (its confirmation, maic_confirm_required, joins maic.session.set with step 6).
    void configure(Agent& a, Mode mode) {
        const Settings& st = options.settings;
        a.review_with_model = st.harness != "dumb";
        a.reviewer_model = st.reviewer_model;
        if (mode == Mode::Auto && !a.review_with_model && !st.dumb_auto_ok) mode = Mode::Edit;
        a.providers = st.providers;
        a.set_forbid(st.forbid);
        a.set_permission(st.permission);
        a.agents = st.agents;
        a.presets = st.presets;
        a.small_model = st.small_model;
        a.reviewer_budget_tokens = st.reviewer_budget_tokens;
        a.audit = st.audit;
        a.mode = mode;
        a.think = st.think;
        a.compaction.at = st.compact_at;
        a.compaction.keep_results = st.compact_keep_results;
        a.budget_tokens = st.budget_tokens;
        a.full_output = st.full_output;
        a.full_output_max_mb = static_cast<size_t>(st.full_output_max_mb);
        a.set_instruction_options(st.instructions);
    }

    fs::path workspace_for(const Client& c, const std::string& given) {
        std::error_code ec;
        fs::path ws = fs::weakly_canonical(given, ec);
        if (ec || !fs::is_directory(ws)) throw bad_params("not a directory: " + given, "workspace");
        if (c.origin == Origin::Remote && !inside(ws, options.workspaces)) {
            throw refuse("maic_forbidden_remote", "workspace outside the allowed roots (server.workspaces in settings): " + given, "workspace");
        }
        return ws;
    }

    void no_isolated_remote(const Client& c) {
        if (c.origin == Origin::Remote && options.settings.tripwire == "isolated") {
            throw refuse("maic_forbidden_remote", "this MAIC runs isolated sessions (tripwire = isolated): it takes no remote work");
        }
    }

    // A new load of a session: sequence_number 0 is its state.
    void open_session(const std::shared_ptr<Session>& s, const Client& c) {
        {
            std::lock_guard lock(mu);
            sessions[s->id] = s;
        }
        std::lock_guard lock(s->mu);
        emit(*s, {{"type", "maic.session.state"}, {"state", s->state}, {"activity", s->activity}, {"waiting", nullptr}, {"by", c.by()}});
        index_changed(*s);
    }

    json conversation(Session& s, const Client& c) {
        json meta = json::object();
        if (!s.title.empty()) meta["title"] = s.title;
        struct stat st{};
        long created = s.log && stat(s.log->path().c_str(), &st) == 0 ? static_cast<long>(st.st_ctime) : static_cast<long>(std::time(nullptr));
        return {{"id", s.id}, {"object", "conversation"}, {"metadata", meta}, {"created_at", created}, {"maic", {{"entry", for_client(entry(s), c)}}}};
    }

    json response_object(Session& s, const Run& r, const std::string& status) {
        json o = {{"id", r.id},
                  {"object", "response"},
                  {"created_at", r.created_at},
                  {"status", status},
                  {"background", true},
                  {"access_programs", nullptr},
                  {"error", nullptr},
                  {"incomplete_details", nullptr},
                  {"instructions", nullptr},
                  {"model", s.agent.model},
                  {"tools", json::array()},
                  {"output", r.output},
                  {"parallel_tool_calls", false},
                  {"metadata", json::object()},
                  {"tool_choice", "auto"},
                  {"temperature", nullptr},
                  {"top_p", nullptr},
                  {"previous_response_id", nullptr},
                  {"conversation", {{"id", s.id}}},
                  {"maic", {{"turn", r.turn}, {"origin", origin_name(r.origin)}}}};
        if (!s.title.empty()) o["metadata"]["title"] = s.title;
        return o;
    }

    json usage_update(Session& s) {
        Agent::UsageReport u = s.agent.usage();
        json out = {{"usage", usage_json(u.last.input, u.last.output)}, {"calls", u.calls}, {"context", u.last.context}, {"last_input", u.last.input},
                    {"total", {{"input", u.total_input}, {"output", u.total_output}}}};
        if (s.agent.budget_tokens > 0) out["budget"] = s.agent.budget_tokens;
        return out;
    }

    // ---------- turns ----------

    class TurnEvents;
    void run_turns(std::shared_ptr<Session> s, std::string text, Origin origin);
    void start_turn(const std::shared_ptr<Session>& s, const std::string& text, Origin origin) {
        if (s->worker.joinable()) s->worker.join();  // it set running = false and needs the lock no more
        s->running = true;
        s->cancel = false;
        s->cancel_by = nullptr;
        s->worker = std::thread([this, s, text, origin] { run_turns(s, text, origin); });
    }

    // Stops what runs: the turn is cancelled, and what waits for a person is answered `answer`.
    void interrupt(Session& s, const json& by, ApprovalAnswer answer) {
        if (!s.running) return;
        s.cancel = true;
        s.cancel_by = by;
        for (auto& [id, p] : s.approvals) {
            if (!p.answer) {
                p.answer = answer;
                p.by = by;
            }
        }
        for (auto& [id, q] : s.questions) {
            if (!q.answer) {
                q.answer = "";
                q.by = by;
            }
        }
        s.cv.notify_all();
    }

    // ---------- the methods ----------

    json hello(Client& c, const json& p) {
        if (p.value("protocol", 0) < kProtocol) {
            throw refuse("maic_unsupported_protocol", "this engine speaks protocol " + std::to_string(kProtocol) + " to " + std::to_string(kProtocol), "protocol");
        }
        c.hello = true;
        return {{"protocol", kProtocol},
                {"engine", {{"version", MAIC_VERSION}, {"epoch", epoch}}},
                {"client", c.id},
                {"origin", origin_name(c.origin)},
                {"capabilities", {"tool_output", "index"}},
                {"limits", {{"always", c.origin == Origin::Local}, {"max_message", kMaxMessage}}},
                {"tier", options.tier},
                {"path", {{"via", c.via}}}};
    }

    json engine_status(Client&, const json&) {
        auto services = load_services(root_dir() / "services");
        StatusReport r = status_report(services);
        json svc = json::array();
        for (const auto& s : r.services) svc.push_back({{"name", s.name}, {"state", s.state}, {"runtime", s.runtime}, {"where", s.where}, {"detail", s.detail}});
        const Settings& st = options.settings;
        auto [provider, model_name] = resolve_model(st.providers, st.model);
        json roots = json::array();
        for (const auto& p : options.workspaces) roots.push_back(p.string());
        size_t count;
        {
            std::lock_guard lock(mu);
            count = sessions.size();
        }
        return {{"harness", {{"tripped", r.tripped}, {"reason", r.tripwire}}},
                {"services", svc},
                {"model", st.model},
                {"provider", provider.name},
                {"remote_model", provider.remote()},
                {"mode", st.mode},
                {"sessions", count},
                {"workspaces", roots},
                {"tier", options.tier},
                {"version", MAIC_VERSION}};
    }

    json engine_trip(Client& c, const json& p) {
        std::string reason = p.value("reason", "");
        if (reason.empty()) reason = "tripped from a client";
        trip_tripwire(std::string(c.origin == Origin::Remote ? "remote: " : "") + reason);
        for (const auto& s : all_sessions()) {
            std::lock_guard lock(s->mu);
            interrupt(*s, c.by(), ApprovalAnswer{Approval::Trip, ""});
        }
        json msg = {{"jsonrpc", "2.0"}, {"method", "maic.engine"}, {"params", {{"tripped", true}, {"reason", reason}}}};
        std::vector<std::shared_ptr<Client>> all;
        {
            std::lock_guard lock(mu);
            for (const auto& [id, cl] : clients) all.push_back(cl);
        }
        for (const auto& cl : all) cl->push(msg, dump(msg).size());
        return {{"tripped", true}};
    }

    std::vector<std::shared_ptr<Session>> all_sessions() {
        std::lock_guard lock(mu);
        std::vector<std::shared_ptr<Session>> out;
        for (const auto& [id, s] : sessions) out.push_back(s);
        return out;
    }

    json index_get(Client& c, const json&) {
        std::lock_guard lock(index_mu);
        json list = json::array();
        for (const auto& [id, e] : entries) list.push_back(for_client(e, c));
        return {{"entries", list}};
    }

    json index_subscribe(Client& c, const json&) {
        std::lock_guard lock(index_mu);
        if (!c.index) index_clients.push_back(clients.at(c.id));
        c.index = true;
        return json::object();
    }

    json index_unsubscribe(Client& c, const json&) {
        std::lock_guard lock(index_mu);
        c.index = false;
        index_clients.erase(std::remove_if(index_clients.begin(), index_clients.end(), [&](const std::weak_ptr<Client>& w) {
                                auto p = w.lock();
                                return !p || p.get() == &c;
                            }),
                            index_clients.end());
        return json::object();
    }

    json session_list(Client& c, const json& p) {
        bool all = p.value("all", false);
        std::string query = p.value("query", "");
        size_t limit = p.value("limit", 50);
        std::set<std::string> loaded;
        {
            std::lock_guard lock(mu);
            for (const auto& [id, s] : sessions) loaded.insert(id);
        }
        json out = json::array();
        for (const auto& info : list_sessions()) {
            if (out.size() >= limit) break;
            std::error_code ec;
            fs::path ws = fs::weakly_canonical(info.workspace, ec);
            bool in_roots = inside(ws, options.workspaces);
            if (c.origin == Origin::Remote && !in_roots) continue;
            if (!all && !options.workspaces.empty() && !in_roots) continue;
            if (!query.empty() && info.id.find(query) == std::string::npos && info.title.find(query) == std::string::npos &&
                info.first_prompt.find(query) == std::string::npos) continue;
            json e = {{"id", info.id}, {"title", info.title}, {"workspace", info.workspace}, {"kind", info.kind}, {"started", info.started},
                      {"turns", info.turns}, {"model", info.model}, {"first_prompt", info.first_prompt}, {"loaded", loaded.count(info.id) > 0}};
            if (!info.parent.empty()) e["parent"] = info.parent;
            if (!info.agent.empty()) e["agent"] = info.agent;
            if (c.origin == Origin::Local) e["path"] = info.path.string();
            out.push_back(e);
        }
        return {{"sessions", out}};
    }

    json create_conversation(Client& c, const json& p) {
        no_isolated_remote(c);
        json m = p.value("maic", json::object());
        std::string given = m.value("workspace", "");
        if (given.empty()) given = options.workspaces.empty() ? fs::current_path().string() : options.workspaces.front().string();
        fs::path ws = workspace_for(c, given);
        std::string mode_str = m.value("mode", options.settings.mode);
        auto mode = parse_mode(mode_str);
        if (!mode) throw bad_params("unknown mode '" + mode_str + "' (manual, auto-read, edit, auto, plan)", "mode");
        auto s = std::make_shared<Session>(ws, m.value("model", options.settings.model));
        configure(s->agent, *mode);
        s->log = std::make_unique<SessionLog>(options.kind, resolve_sessions_home(options.settings, ws));
        s->id = s->log->path().stem().string();
        s->agent.set_log(s->log.get());
        if (p.contains("metadata") && p["metadata"].is_object()) {
            s->title = p["metadata"].value("title", "");
            if (!s->title.empty()) s->log->write("title", {{"text", s->title}});
        }
        open_session(s, c);
        std::lock_guard lock(s->mu);
        return conversation(*s, c);
    }

    json get_conversation(Client& c, const json& p) {
        auto s = session(p.at("conversation_id"));
        std::lock_guard lock(s->mu);
        return conversation(*s, c);
    }

    json session_resume(Client& c, const json& p) {
        no_isolated_remote(c);
        std::string given = p.at("session");
        if (c.origin == Origin::Remote && given.find('/') != std::string::npos) throw refuse("maic_forbidden_remote", "a remote client resumes by id, not by path", "session");
        auto info = find_session(given);
        if (!info) throw refuse("maic_not_found", "no session matching " + given, "session");
        {
            std::lock_guard lock(mu);
            if (auto it = sessions.find(info->id); it != sessions.end()) {
                auto s = it->second;
                std::lock_guard sl(s->mu);
                return for_client(entry(*s), c);
            }
        }
        fs::path ws = workspace_for(c, info->workspace);
        LoadedSession old = load_session(info->path);
        auto mode = parse_mode(old.mode).value_or(parse_mode(options.settings.mode).value_or(Mode::Manual));
        auto s = std::make_shared<Session>(ws, old.model.empty() ? options.settings.model : old.model);
        configure(s->agent, mode);
        s->log = std::make_unique<SessionLog>(SessionLog::Reopen{}, info->path);
        s->id = info->id;
        s->title = info->title;
        s->turns = static_cast<int>(info->turns);
        s->agent.set_log(s->log.get());
        s->agent.restore(old.messages);
        open_session(s, c);
        std::lock_guard lock(s->mu);
        return for_client(entry(*s), c);
    }

    // The replay from the ring after `starting_after`, then everything as it comes; under s.mu, so nothing falls between.
    json subscribe_locked(Session& s, Client& c, long starting_after, bool replay) {
        long first = s.ring.empty() ? s.next : s.ring.front().first["sequence_number"].get<long>();
        long from = replay ? (starting_after < 0 ? first : starting_after + 1) : s.next;
        if (from < first || from > s.next) {
            throw refuse("maic_resync", "events after " + std::to_string(starting_after) + " are not in the ring (it holds " + std::to_string(first) + " to " +
                                            std::to_string(s.next - 1) + "); attach again", "starting_after");
        }
        auto self = clients_ptr(c);
        if (std::find(s.subscribers.begin(), s.subscribers.end(), self) == s.subscribers.end()) s.subscribers.push_back(self);
        {
            std::lock_guard lock(c.mu);
            c.sessions.insert(s.id);
        }
        if (replay) {
            for (const auto& [e, size] : s.ring) {
                if (e["sequence_number"].get<long>() >= from) c.event(e, size);
            }
        }
        return {{"load", s.load}, {"sequence_number", s.next - 1}, {"activity", s.activity}, {"replay_from", from}};
    }

    std::shared_ptr<Client> clients_ptr(const Client& c) {
        std::lock_guard lock(mu);
        return clients.at(c.id);
    }

    json session_subscribe(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::lock_guard lock(s->mu);
        if (p.contains("load") && p["load"] != s->load) throw refuse("maic_resync", "the session was loaded again since (load " + s->load + "); attach again", "load");
        return subscribe_locked(*s, c, p.value("starting_after", -1L), true);
    }

    json session_unsubscribe(Client& c, const json& p) {
        auto s = session(p.at("session"));
        auto self = clients_ptr(c);
        std::lock_guard lock(s->mu);
        s->subscribers.erase(std::remove(s->subscribers.begin(), s->subscribers.end(), self), s->subscribers.end());
        std::lock_guard cl(c.mu);
        c.sessions.erase(s->id);
        return json::object();
    }

    json session_attach(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::lock_guard lock(s->mu);
        subscribe_locked(*s, c, -1, false);
        json pending = json::array(), questions = json::array();
        for (const auto& [id, a] : s->approvals) {
            if (!a.answer) pending.push_back(a.event);
        }
        for (const auto& [id, q] : s->questions) {
            if (!q.answer) questions.push_back(q.event);
        }
        // History comes from the transcript with step 11 (the line-offset index, listConversationItems).
        return {{"entry", for_client(entry(*s), c)},
                {"load", s->load},
                {"sequence_number", s->next - 1},
                {"more_before", false},
                {"items", json::array()},
                {"inflight", nullptr},
                {"pending", pending},
                {"questions", questions},
                {"todo", s->todo},
                {"usage", usage_update(*s)}};
    }

    json session_set(Client& c, const json& p) {
        auto s = session(p.at("session"));
        for (const auto& [k, v] : p.items()) {
            if (k != "session" && k != "mode") throw bad_params("maic.session.set takes mode in this build; " + k + " is not settable yet", k);
        }
        std::lock_guard lock(s->mu);
        if (p.contains("mode")) {
            std::string name = p["mode"];
            auto mode = parse_mode(name);
            if (!mode) throw bad_params("unknown mode '" + name + "' (manual, auto-read, edit, auto, plan)", "mode");
            Mode now = s->agent.mode;
            // Decision 9: a remote client tightens freely and loosens up to edit; auto needs a step-up, which fails
            // until accounts register a verifier.
            if (c.origin == Origin::Remote && *mode == Mode::Auto && now != Mode::Auto) {
                throw refuse("maic_step_up_required", "a remote client loosens a session to auto only after a step-up check, which needs accounts", "mode");
            }
            s->agent.mode = *mode;
            emit(*s, {{"type", "maic.session.settings"}, {"mode", name}, {"by", c.by()}});
            index_changed(*s);
        }
        return for_client(entry(*s), c);
    }

    // The text of OpenAI's InputParam: a string, or messages with input_text parts.
    static std::string input_text(const json& input) {
        if (input.is_string()) return input;
        std::string out;
        for (const auto& item : input) {
            if (!item.is_object() || item.value("role", "user") != "user") throw bad_params("input takes the user's messages", "input");
            const json& content = item.contains("content") ? item["content"] : json();
            if (content.is_string()) {
                out += (out.empty() ? "" : "\n") + content.get<std::string>();
                continue;
            }
            for (const auto& part : content) {
                if (part.value("type", "") != "input_text") throw bad_params("only input_text parts are read in this build (input_image arrives with maic.session.image)", "input");
                out += (out.empty() ? "" : "\n") + part.value("text", "");
            }
        }
        return out;
    }

    json response_create(Client& c, const json& p) {
        std::string sid;
        if (p.contains("conversation")) sid = p["conversation"].is_string() ? p["conversation"].get<std::string>() : p["conversation"].value("id", "");
        if (p.contains("stream_id")) {
            if (!sid.empty() && sid != p["stream_id"].get<std::string>()) throw bad_params("stream_id and conversation name different sessions", "stream_id");
            sid = p["stream_id"];
        }
        if (sid.empty()) throw bad_params("conversation or stream_id names the session", "conversation");
        std::string text = input_text(p.at("input"));
        if (text.empty()) throw bad_params("the input is empty", "input");
        auto s = session(sid);
        std::lock_guard lock(s->mu);
        long before = s->next - 1;
        json item = {{"id", "~" + std::to_string(s->next)}, {"type", "message"}, {"role", "user"}, {"content", {{{"type", "input_text"}, {"text", text}}}}};
        if (s->running) {
            // Into the running response at its next boundary, as typing mid-turn does; a remote message raises the
            // turn's origin. Step 7 splits this into response.steer and a FIFO queue on the lane.
            s->agent.post_message(text, c.origin);
            emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", true}, {"by", c.by()}});
            index_changed(*s);
            json r = s->run ? response_object(*s, *s->run, "in_progress") : json();
            if (r.is_null()) throw refuse("response_not_active", "the session is finishing its turn; send again", "conversation");
            r["maic"]["queued"] = true;
            r["maic"]["sequence_number"] = before;
            return r;
        }
        emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", false}, {"by", c.by()}});
        start_turn(s, text, c.origin);
        Run preview;
        preview.id = s->id + ".r" + std::to_string(s->turns + 1);
        preview.turn = s->turns + 1;
        preview.created_at = static_cast<long>(std::time(nullptr));
        preview.origin = c.origin;
        json r = response_object(*s, preview, "in_progress");
        r["maic"]["sequence_number"] = before;
        return r;
    }

    json cancel_response(Client& c, const json& p) {
        std::string rid = p.at("response_id");
        size_t dot = rid.rfind(".r");
        if (dot == std::string::npos) throw refuse("maic_not_found", "no response " + rid, "response_id");
        auto s = session(rid.substr(0, dot));
        std::lock_guard lock(s->mu);
        if (!s->run || s->run->id != rid) throw refuse("maic_not_found", "response " + rid + " is not running", "response_id");
        interrupt(*s, c.by(), ApprovalAnswer{Approval::No, "interrupted by the user"});
        return response_object(*s, *s->run, "cancelled");
    }

    json approval_answer(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::string id = p.at("approval");
        auto choice = parse_choice(p.at("choice"));
        if (!choice) throw bad_params("choice must be yes, no, always or trip", "choice");
        if (*choice == Approval::Always && c.origin == Origin::Remote) {
            throw refuse("maic_forbidden_remote", "a remote client answers yes, no or trip: always never covers a remote-origin call", "choice");
        }
        std::lock_guard lock(s->mu);
        auto it = s->approvals.find(id);
        if (it == s->approvals.end() || it->second.answer) {
            if (s->answered.count(id) || it != s->approvals.end()) throw refuse("maic_already_answered", "approval " + id + " was already answered", "approval");
            throw refuse("maic_not_found", "no approval " + id + " is waiting", "approval");
        }
        it->second.answer = ApprovalAnswer{*choice, p.value("feedback", "")};
        it->second.by = c.by();
        s->cv.notify_all();
        return {{"choice", choice_name(*choice)}};
    }

    json question_reply(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::string id = p.at("question");
        std::lock_guard lock(s->mu);
        auto it = s->questions.find(id);
        if (it == s->questions.end() || it->second.answer) {
            if (s->answered.count(id) || it != s->questions.end()) throw refuse("maic_already_answered", "question " + id + " was already answered", "question");
            throw refuse("maic_not_found", "no question " + id + " is waiting", "question");
        }
        it->second.answer = p.at("text").get<std::string>();
        it->second.by = c.by();
        s->cv.notify_all();
        return json::object();
    }

    // ---------- the dispatcher ----------

    json dispatch(Client& c, const json& msg) {
        json id = msg.contains("id") ? msg["id"] : json();
        bool notification = !msg.contains("id");
        auto answer = [&](json reply) { return notification ? json() : reply; };
        if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string() ||
            (msg.contains("params") && !msg["params"].is_object())) {
            return answer(error_reply(id, -32600, "", "not a JSON-RPC 2.0 request with by-name params", ""));
        }
        std::string method = msg["method"];
        const auto& table = handlers();
        auto h = table.find(method);
        if (h == table.end()) return answer(error_reply(id, -32601, "", "no method " + method, ""));
        if (!c.hello && method != "maic.hello") return answer(error_reply(id, -32600, "maic_hello_required", "nothing but maic.hello is accepted before maic.hello", ""));
        json params = msg.value("params", json::object());
        const auto& schemas = protocol::Schemas::get();
        const auto* described = schemas.method(method);
        if (c.origin == Origin::Remote && described) {
            if (!described->remote) return answer(error_reply(id, -32000, "maic_forbidden_remote", method + " is not available to a remote client", ""));
            for (const auto& name : described->local_only_params) {
                if (params.contains(name)) return answer(error_reply(id, -32000, "maic_forbidden_remote", "`" + name + "` is not available to a remote client", name));
            }
        }
        if (options.tier != "open") {
            if (std::string e = schemas.params_error(method, params); !e.empty()) fault("incoming", "schema.message", method + " params " + e);
        }
        try {
            json result = (this->*(h->second))(c, params);
            if (options.tier != "open") {
                if (std::string e = schemas.result_error(method, result); !e.empty()) fault("result", "schema.message", method + " result " + e);
            }
            return answer({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        } catch (const RpcError& e) {
            return answer(error_reply(id, e.code, e.data_code, e.message, e.param, e.type));
        } catch (const json::exception& e) {
            return answer(error_reply(id, -32602, "", std::string("params: ") + e.what(), ""));
        } catch (const std::exception& e) {
            return answer(error_reply(id, -32603, "", e.what(), "", "server_error"));
        }
    }
};

const std::map<std::string, Engine::Impl::Handler>& Engine::Impl::handlers() {
    static const std::map<std::string, Handler> table = {
        {"maic.hello", &Impl::hello},
        {"maic.engine.status", &Impl::engine_status},
        {"maic.engine.trip", &Impl::engine_trip},
        {"maic.index.get", &Impl::index_get},
        {"maic.index.subscribe", &Impl::index_subscribe},
        {"maic.index.unsubscribe", &Impl::index_unsubscribe},
        {"maic.session.list", &Impl::session_list},
        {"createConversation", &Impl::create_conversation},
        {"getConversation", &Impl::get_conversation},
        {"maic.session.resume", &Impl::session_resume},
        {"maic.session.attach", &Impl::session_attach},
        {"maic.session.subscribe", &Impl::session_subscribe},
        {"maic.session.unsubscribe", &Impl::session_unsubscribe},
        {"maic.session.set", &Impl::session_set},
        {"response.create", &Impl::response_create},
        {"cancelResponse", &Impl::cancel_response},
        {"maic.approval.answer", &Impl::approval_answer},
        {"maic.question.reply", &Impl::question_reply},
    };
    return table;
}

// The agent's callbacks for one response, as OpenAI's events: output items announced before their deltas and closed
// after them, a tool call as its call item then its output item, approvals and questions between (section 10).
// Every method takes the session's lock; ask() and question() wait on it for a person.
class Engine::Impl::TurnEvents : public AgentEvents {
public:
    TurnEvents(Impl& engine, Session& s) : e_(engine), s_(s) {}

    void on_text(std::string_view delta, bool thinking) override {
        std::lock_guard lock(s_.mu);
        if (text_ && text_->thinking != thinking) close_text("completed");
        if (!text_) open_text(thinking);
        text_->text.append(delta);
        json ev = {{"type", thinking ? "response.reasoning_text.delta" : "response.output_text.delta"}, {"item_id", text_->id}, {"output_index", text_->index},
                   {"content_index", 0}, {"delta", std::string(delta)}};
        if (!thinking) ev["logprobs"] = json::array();
        e_.emit(s_, ev);
    }

    void on_tool_proposed(const ToolCall& call) override {
        std::lock_guard lock(s_.mu);
        proposed_ = call;
    }

    void on_tool_call(const std::string& summary) override {
        std::lock_guard lock(s_.mu);
        if (!proposed_) {
            notice_locked(summary);  // a line about the call, not a call ("↳ explore on ...")
            return;
        }
        ToolCall call = std::move(*proposed_);
        proposed_.reset();
        Frame f{call.id, call.name, call.name == "run_shell", !calls_.empty()};
        if (f.child) {
            notice_locked(summary);  // a subagent's call: its items belong to its own session (step 14)
            calls_.push_back(f);
            return;
        }
        close_text("completed");
        f.index = next_index_++;
        f.item_id = "~" + std::to_string(s_.next);
        json item;
        if (f.shell) {
            std::string command = call.arguments.is_object() ? call.arguments.value("command", "") : "";
            item = {{"type", "shell_call"}, {"id", f.item_id}, {"call_id", call.id}, {"status", "in_progress"}, {"environment", nullptr},
                    {"action", {{"commands", {command}}, {"timeout_ms", nullptr}, {"max_output_length", nullptr}}}, {"maic", {{"summary", summary}}}};
            e_.emit(s_, {{"type", "response.output_item.added"}, {"output_index", f.index}, {"item", item}});
            e_.emit(s_, {{"type", "response.shell_call_command.added"}, {"output_index", f.index}, {"command_index", 0}, {"command", command}});
            e_.emit(s_, {{"type", "response.shell_call_command.done"}, {"output_index", f.index}, {"command_index", 0}, {"command", command}});
        } else {
            item = {{"type", "function_call"}, {"id", f.item_id}, {"call_id", call.id}, {"name", call.name}, {"arguments", dump(call.arguments)},
                    {"status", "in_progress"}, {"maic", {{"summary", summary}}}};
            e_.emit(s_, {{"type", "response.output_item.added"}, {"output_index", f.index}, {"item", item}});
            e_.emit(s_, {{"type", "response.function_call_arguments.done"}, {"item_id", f.item_id}, {"output_index", f.index}, {"arguments", item["arguments"]}});
        }
        item["status"] = "completed";
        e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", f.index}, {"item", item}});
        output().push_back(item);
        calls_.push_back(f);
    }

    void on_tool_output(const std::string& call_id, OutputStream stream, std::string_view chunk, size_t offset) override {
        std::lock_guard lock(s_.mu);
        if (calls_.empty()) return;
        Frame& top = calls_.front();
        open_output(top);
        if (call_id == top.call_id && top.shell) {
            json delta = {{"stdout", ""}, {"stderr", ""}};
            delta[stream == OutputStream::Stdout ? "stdout" : "stderr"] = std::string(chunk);
            e_.emit(s_, {{"type", "response.shell_call_output_content.delta"}, {"item_id", top.out_id}, {"output_index", top.out_index}, {"command_index", 0},
                         {"delta", delta}, {"maic", {{"offset", offset}}}});
            return;
        }
        json ev = {{"type", "maic.tool.output.delta"}, {"item_id", top.out_id}, {"output_index", top.out_index}, {"offset", offset}, {"data", std::string(chunk)}};
        if (call_id != top.call_id) ev["call"] = call_id;
        e_.emit(s_, ev);
    }

    void on_tool_full_output(const fs::path& file) override {
        std::lock_guard lock(s_.mu);
        if (calls_.empty()) return;
        std::error_code ec;
        calls_.back().full_output = {{"session", file.parent_path().stem().string()}, {"call", file.stem().string()}, {"bytes", fs::file_size(file, ec)},
                                     {"label", kFullOutputLabel}};
    }

    void on_tool_result(const std::string& text, bool ok) override {
        std::lock_guard lock(s_.mu);
        if (calls_.empty()) return;
        Frame f = calls_.back();
        calls_.pop_back();
        if (f.child) return;
        open_output(f);
        json item;
        if (f.shell) {
            json out = shell_output(text);
            e_.emit(s_, {{"type", "response.shell_call_output_content.done"}, {"item_id", f.out_id}, {"output_index", f.out_index}, {"command_index", 0}, {"output", out}});
            item = {{"type", "shell_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"status", "completed"}, {"output", out}, {"max_output_length", nullptr}};
        } else {
            item = {{"type", "function_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"output", text}, {"status", "completed"}};
        }
        item["maic"] = {{"ok", ok}};
        if (!f.full_output.is_null()) item["maic"]["full_output"] = f.full_output;
        e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", f.out_index}, {"item", item}});
        output().push_back(item);
    }

    void on_notice(const std::string& text) override {
        std::lock_guard lock(s_.mu);
        notice_locked(text);
    }

    void on_todo(const std::vector<TodoItem>& items) override {
        std::lock_guard lock(s_.mu);
        json list = json::array();
        for (const auto& t : items) list.push_back({{"text", t.text}, {"done", t.done}});
        s_.todo = list;
        e_.emit(s_, {{"type", "maic.todo.updated"}, {"items", list}});
    }

    void on_file_written(const fs::path& path, const std::string& tool) override {
        std::lock_guard lock(s_.mu);
        e_.emit(s_, {{"type", "maic.file.written"}, {"path", path.string()}, {"tool", tool}});
    }

    ApprovalAnswer ask(const ApprovalRequest& r) override {
        std::unique_lock lock(s_.mu);
        std::string id = "a" + std::to_string(++e_.approvals_made);
        json ev = {{"type", "maic.approval.requested"}, {"id", id}, {"tool", r.tool}, {"summary", r.summary}, {"reason", r.reason}, {"origin", origin_name(r.origin)},
                   {"always_covers", r.always_covers}, {"preview", r.preview}};
        if (!r.path.empty()) ev["path"] = r.path.string();
        if (r.proposed) ev["proposed_size"] = r.proposed->size();
        std::string call_id;
        if (!r.agent.empty()) {
            ev["thread"] = {{"agent", r.agent}};
        } else if (!calls_.empty()) {
            call_id = calls_.front().call_id;
            ev["call_id"] = call_id;
            ev["item_id"] = calls_.front().item_id;
        }
        e_.emit(s_, ev);
        ev["sequence_number"] = s_.next - 1;
        ev["stream_id"] = s_.id;
        s_.approvals[id] = PendingApproval{ev, std::nullopt, nullptr};
        e_.set_activity(s_, "waiting", {{"kind", "approval"}, {"id", id}, {"tool", r.tool}, {"summary", r.summary}});
        s_.cv.wait(lock, [&] { return s_.approvals[id].answer.has_value() || s_.cancel.load(); });
        PendingApproval p = s_.approvals[id];
        s_.approvals.erase(id);
        s_.answered.insert(id);
        ApprovalAnswer answer = p.answer.value_or(ApprovalAnswer{Approval::No, "interrupted by the user"});
        json by = p.by.is_null() ? (s_.cancel_by.is_null() ? json{{"client", "engine"}, {"name", "engine"}, {"origin", "local"}} : s_.cancel_by) : p.by;
        json done = {{"type", "maic.approval.answered"}, {"id", id}, {"choice", choice_name(answer.choice)}, {"by", by}};
        if (!call_id.empty()) done["call_id"] = call_id;
        e_.emit(s_, done);
        settle_activity();
        return answer;
    }

    std::string question(const std::string& text, const std::vector<std::string>& options) override {
        std::unique_lock lock(s_.mu);
        std::string id = "q" + std::to_string(++e_.questions_made);
        json ev = {{"type", "maic.question.asked"}, {"id", id}, {"text", text}, {"options", options}};
        e_.emit(s_, ev);
        ev["sequence_number"] = s_.next - 1;
        ev["stream_id"] = s_.id;
        s_.questions[id] = PendingQuestion{ev, std::nullopt, nullptr};
        e_.set_activity(s_, "waiting", {{"kind", "question"}, {"id", id}, {"summary", text}});
        s_.cv.wait(lock, [&] { return s_.questions[id].answer.has_value() || s_.cancel.load(); });
        PendingQuestion q = s_.questions[id];
        s_.questions.erase(id);
        s_.answered.insert(id);
        json by = q.by.is_null() ? (s_.cancel_by.is_null() ? json{{"client", "engine"}, {"name", "engine"}, {"origin", "local"}} : s_.cancel_by) : q.by;
        e_.emit(s_, {{"type", "maic.question.answered"}, {"id", id}, {"by", by}, {"withdrawn", false}});
        settle_activity();
        return q.answer.value_or("");
    }

    // The response is ending: what is still open closes, `status` for a reply cut short.
    void close_all(const std::string& status) {
        close_text(status);
        while (!calls_.empty()) {
            Frame f = calls_.back();
            calls_.pop_back();
            if (f.child || f.out_index < 0) continue;
            json item = f.shell ? json{{"type", "shell_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"status", "incomplete"}, {"output", json::array()},
                                       {"max_output_length", nullptr}}
                                : json{{"type", "function_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"output", ""}, {"status", "incomplete"}};
            e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", f.out_index}, {"item", item}});
            output().push_back(item);
        }
    }

private:
    struct Text {
        bool thinking;
        std::string id;
        long index;
        std::string text;
    };
    struct Frame {
        std::string call_id, name;
        bool shell = false, child = false;
        long index = -1;
        std::string item_id;
        long out_index = -1;
        std::string out_id;
        json full_output;
    };

    json& output() { return s_.run->output; }

    void notice_locked(const std::string& text) {
        std::string level = text.rfind("HARNESS TRIPPED", 0) == 0 || text.rfind("HALTED", 0) == 0 ? "error" : "info";
        e_.emit(s_, {{"type", "maic.notice"}, {"text", text}, {"level", level}});
    }

    void open_text(bool thinking) {
        text_ = Text{thinking, "~" + std::to_string(s_.next), next_index_++, ""};
        json item = thinking ? json{{"type", "reasoning"}, {"id", text_->id}, {"summary", json::array()}, {"content", json::array()}, {"status", "in_progress"}}
                             : json{{"type", "message"}, {"id", text_->id}, {"role", "assistant"}, {"status", "in_progress"}, {"content", json::array()}};
        e_.emit(s_, {{"type", "response.output_item.added"}, {"output_index", text_->index}, {"item", item}});
        e_.emit(s_, {{"type", "response.content_part.added"}, {"item_id", text_->id}, {"output_index", text_->index}, {"content_index", 0}, {"part", part("")}});
    }

    json part(const std::string& text) const {
        if (text_->thinking) return {{"type", "reasoning_text"}, {"text", text}};
        return {{"type", "output_text"}, {"text", text}, {"annotations", json::array()}, {"logprobs", json::array()}};
    }

    void close_text(const std::string& status) {
        if (!text_) return;
        const Text& t = *text_;
        json done = {{"type", t.thinking ? "response.reasoning_text.done" : "response.output_text.done"}, {"item_id", t.id}, {"output_index", t.index},
                     {"content_index", 0}, {"text", t.text}};
        if (!t.thinking) done["logprobs"] = json::array();
        e_.emit(s_, done);
        e_.emit(s_, {{"type", "response.content_part.done"}, {"item_id", t.id}, {"output_index", t.index}, {"content_index", 0}, {"part", part(t.text)}});
        json item = t.thinking ? json{{"type", "reasoning"}, {"id", t.id}, {"summary", json::array()}, {"content", {part(t.text)}}, {"status", status}}
                               : json{{"type", "message"}, {"id", t.id}, {"role", "assistant"}, {"status", status}, {"content", {part(t.text)}}};
        e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", t.index}, {"item", item}});
        output().push_back(item);
        text_.reset();
    }

    // The call's output item, announced when its first output or its result comes.
    void open_output(Frame& f) {
        if (f.out_index >= 0) return;
        f.out_index = next_index_++;
        f.out_id = "~" + std::to_string(s_.next);
        json item = f.shell ? json{{"type", "shell_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"status", "in_progress"}, {"output", json::array()},
                                   {"max_output_length", nullptr}}
                            : json{{"type", "function_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"output", ""}, {"status", "in_progress"}};
        e_.emit(s_, {{"type", "response.output_item.added"}, {"output_index", f.out_index}, {"item", item}});
    }

    void settle_activity() {
        bool waits = false;
        for (const auto& [id, a] : s_.approvals) waits = waits || !a.answer;
        for (const auto& [id, q] : s_.questions) waits = waits || !q.answer;
        if (!waits) e_.set_activity(s_, "working");
    }

    Impl& e_;
    Session& s_;
    std::optional<Text> text_;
    std::optional<ToolCall> proposed_;
    std::vector<Frame> calls_;  // the call running, then a subagent's inside it
    long next_index_ = 0;
};

void Engine::Impl::run_turns(std::shared_ptr<Session> s, std::string text, Origin origin) {
    for (;;) {
        TurnEvents events(*this, *s);
        {
            std::lock_guard lock(s->mu);
            Run r;
            r.turn = ++s->turns;
            r.id = s->id + ".r" + std::to_string(r.turn);
            r.created_at = static_cast<long>(std::time(nullptr));
            r.origin = origin;
            r.usage_before = s->agent.usage();
            s->run = r;
            emit(*s, {{"type", "response.created"}, {"response", response_object(*s, r, "in_progress")}});
            set_activity(*s, "working");
            emit(*s, {{"type", "response.in_progress"}, {"response", response_object(*s, r, "in_progress")}});
        }
        std::string failure;
        try {
            s->agent.submit(text, origin, events, s->cancel);
        } catch (const std::exception& e) {
            failure = e.what();
        }
        std::unique_lock lock(s->mu);
        bool cancelled = s->cancel.load();
        events.close_all(cancelled || !failure.empty() ? "incomplete" : "completed");
        Run& r = *s->run;
        r.origin = s->agent.turn_origin();
        json usage = usage_update(*s);
        usage["type"] = "maic.usage.updated";
        emit(*s, usage);
        json resp;
        std::string type;
        if (cancelled) {
            resp = response_object(*s, r, "cancelled");
            resp["maic"]["ended_by"] = "cancel";
            type = "maic.response.cancelled";
        } else if (!failure.empty()) {
            resp = response_object(*s, r, "failed");
            resp["error"] = {{"code", "server_error"}, {"message", failure}};
            resp["maic"]["ended_by"] = "error";
            type = "response.failed";
        } else {
            resp = response_object(*s, r, "completed");
            type = "response.completed";
        }
        Agent::UsageReport u = s->agent.usage();
        resp["usage"] = usage_json(u.total_input - r.usage_before.total_input, u.total_output - r.usage_before.total_output);
        resp["completed_at"] = static_cast<long>(std::time(nullptr));
        // Messages queued after the turn's last model call start the next turn on their own, as the TUI does.
        bool again = !cancelled && failure.empty() && s->agent.queued() > 0 && !stopped.load();
        resp["maic"]["final"] = true;
        emit(*s, {{"type", type}, {"response", resp}});
        s->run.reset();
        if (!again) {
            s->running = false;
            set_activity(*s, "idle");
            return;
        }
        text.clear();
        origin = Origin::Local;
        for (const auto& q : s->agent.take_queued()) {
            text += (text.empty() ? "" : "\n\n") + q.text;
            if (q.origin == Origin::Remote) origin = Origin::Remote;
        }
        index_changed(*s);
    }
}

Engine::Engine(EngineOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Engine::~Engine() {
    shutdown();
}

std::string Engine::connect(Origin origin, std::string name, std::string via, std::function<void()> wake) {
    auto c = std::make_shared<Client>();
    c->id = "c" + std::to_string(++impl_->clients_made);
    c->name = std::move(name);
    c->via = std::move(via);
    c->origin = origin;
    c->wake = std::move(wake);
    std::lock_guard lock(impl_->mu);
    impl_->clients[c->id] = c;
    return c->id;
}

void Engine::disconnect(const std::string& id) {
    std::shared_ptr<Client> c;
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->clients.find(id);
        if (it == impl_->clients.end()) return;
        c = it->second;
        impl_->clients.erase(it);
    }
    std::set<std::string> subscribed;
    {
        std::lock_guard lock(c->mu);
        subscribed = c->sessions;
        c->closed = "disconnected";
    }
    for (const auto& sid : subscribed) {
        std::shared_ptr<Session> s;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->sessions.find(sid);
            if (it == impl_->sessions.end()) continue;
            s = it->second;
        }
        std::lock_guard lock(s->mu);
        s->subscribers.erase(std::remove(s->subscribers.begin(), s->subscribers.end(), c), s->subscribers.end());
    }
    std::lock_guard lock(impl_->index_mu);
    impl_->index_clients.erase(std::remove_if(impl_->index_clients.begin(), impl_->index_clients.end(), [&](const std::weak_ptr<Client>& w) {
                                   auto p = w.lock();
                                   return !p || p == c;
                               }),
                               impl_->index_clients.end());
}

json Engine::call(const std::string& id, const json& message) {
    auto c = impl_->client(id);
    json reply_id = message.is_object() && message.contains("id") ? message["id"] : json();
    if (dump(message).size() > kMaxMessage) {
        std::lock_guard lock(c->mu);
        c->closed = "maic_too_large";
        return error_reply(reply_id, -32000, "maic_too_large", "a message is at most 1 MiB", "");
    }
    return impl_->dispatch(*c, message);
}

std::vector<json> Engine::take(const std::string& id, std::chrono::milliseconds wait) {
    auto c = impl_->client(id);
    std::unique_lock lock(c->mu);
    c->cv.wait_for(lock, wait, [&] { return !c->queue.empty() || !c->closed.empty(); });
    std::vector<json> out;
    for (auto& [msg, size] : c->queue) out.push_back(std::move(msg));
    c->queue.clear();
    c->bytes = 0;
    return out;
}

std::string Engine::closed(const std::string& id) const {
    std::shared_ptr<Client> c;
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->clients.find(id);
        if (it == impl_->clients.end()) return "disconnected";
        c = it->second;
    }
    std::lock_guard lock(c->mu);
    return c->closed;
}

void Engine::shutdown() {
    if (impl_->stopped.exchange(true)) return;
    json engine = {{"client", "engine"}, {"name", "engine"}, {"origin", "local"}};
    auto all = impl_->all_sessions();
    for (const auto& s : all) {
        std::lock_guard lock(s->mu);
        impl_->interrupt(*s, engine, ApprovalAnswer{Approval::No, "the engine is shutting down"});
    }
    for (const auto& s : all) {
        if (s->worker.joinable()) s->worker.join();
        std::lock_guard lock(s->mu);
        s->state = "parked";
        impl_->emit(*s, {{"type", "maic.session.state"}, {"state", "parked"}, {"activity", "idle"}, {"waiting", nullptr}, {"by", engine}});
        impl_->index_changed(*s);
    }
}

std::vector<std::string> Engine::methods() {
    std::vector<std::string> out;
    for (const auto& [name, h] : Impl::handlers()) out.push_back(name);
    return out;
}

std::vector<std::string> Engine::event_types() {
    return kEventTypes;
}

}  // namespace maic
