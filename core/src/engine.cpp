// The engine: sessions, their event streams and the protocol dispatcher (docs/design/engine-protocol.md).
#include "maic/engine.hpp"

#include "session_commands.hpp"

#include "maic/full_output.hpp"
#include "maic/llm.hpp"
#include "maic/paths.hpp"
#include "maic/protocol.hpp"
#include "maic/session.hpp"
#include "maic/skeleton.hpp"
#include "maic/status.hpp"
#include "maic/service.hpp"
#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
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
#include <sstream>
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
constexpr size_t kMaxPendingSteers = 16;          // accepted steers waiting for their successor (too_many_pending_steers)
const char* const kNotRun = "not run: the user redirected";

// The event types the engine sends; protocol_schema_test holds this to event.schema.json's union.
const std::vector<std::string> kEventTypes = {
    "response.created", "response.in_progress", "response.completed", "response.failed",
    "response.incomplete", "error", "response.steer.accepted", "response.steer.failed",
    "response.output_item.added", "response.output_item.done", "response.content_part.added", "response.content_part.done",
    "response.output_text.delta", "response.output_text.done", "response.reasoning_text.delta", "response.reasoning_text.done",
    "response.function_call_arguments.done", "response.shell_call_command.added", "response.shell_call_command.done",
    "response.shell_call_output_content.delta", "response.shell_call_output_content.done",
    "maic.response.cancelled", "maic.steer.applied", "maic.turn.paused", "maic.input.added", "maic.approval.requested", "maic.approval.answered", "maic.question.asked",
    "maic.question.answered", "maic.tool.output.delta", "maic.file.written", "maic.notice", "maic.todo.updated",
    "maic.session.state", "maic.session.settings", "maic.session.title", "maic.usage.updated"};

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

// A command the user runs themselves (`!cmd`, maic.session.shell): their shell, their environment, no sandbox.
// Output streams to `on_output`; cancel kills the whole process group.
int run_user_shell(const std::string& command, const fs::path& cwd, const std::atomic<bool>& cancel, const std::function<void(std::string_view)>& on_output) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDONLY);
        dup2(null_fd, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        const char* shell = std::getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/bash";
        execl(shell, shell, "-c", command.c_str(), nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[8192];
    bool killed = false;
    for (;;) {
        if (cancel.load() && !killed) {
            kill(-pid, SIGTERM);
            killed = true;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        int ready = poll(&pfd, 1, 200);
        if (ready > 0) {
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n <= 0) break;
            on_output(std::string_view(buf, static_cast<size_t>(n)));
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }
    close(fds[0]);
    if (killed) kill(-pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
    std::set<std::string> exclude;          // event types its hello filters out
    std::map<std::string, long> skip_from;  // per session: the first number filtered out since the last event sent
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
            if (exclude.count(type)) {
                skip_from.try_emplace(e.value("stream_id", ""), e["sequence_number"].get<long>());
                return;
            }
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
                    skip_from.erase(e.value("stream_id", ""));  // the merged delta's range covers them
                    return;
                }
            }
            if (auto f = skip_from.find(e.value("stream_id", "")); f != skip_from.end()) {
                if (!changed) changed = e;
                (*changed)["maic"]["filtered_from"] = f->second;
                skip_from.erase(f);
            }
        }
        const json& out = changed ? *changed : e;
        push({{"jsonrpc", "2.0"}, {"method", "maic.event"}, {"params", out}}, changed ? dump(out).size() + 48 : size + 48);
    }
};

// A person's answer, waited for by the agent's worker.
struct PendingApproval {
    json event;  // maic.approval.requested
    std::optional<std::string> proposed;  // a write's content after it, for maic.approval.proposed
    std::optional<ApprovalAnswer> answer;
    json by;
};
struct PendingQuestion {
    json event;  // maic.question.asked
    std::optional<std::string> answer;
    json by;
    bool withdrawn = false;  // a steer took its place
};

// The response being run: what its events carry.
struct Run {
    std::string id;
    int turn = 0;
    std::string previous;  // the response of the turn it continues; "" for a turn's first
    long created_at = 0;
    Origin origin = Origin::Local;
    json output = json::array();
    long next_index = 0;  // the next output_index
    Agent::UsageReport usage_before;
    long cause = -1;  // the event it answers (maic.input.added), maic.cause on its response.created
};

// A steer of section 11, from a client or a ban entry.
struct Steer {
    std::string id, action, note, trigger = "client", response;
    json by, ban;
    Origin origin = Origin::Local;    // its note's: a remote note raises the turn's origin
    std::string trim = "paragraph";   // drop: none, sentence, paragraph, all
    long at = -1;                     // drop: a byte offset chosen in the client, over trim
    size_t keep = std::string::npos;  // how much of the reply being written stays
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
    // The stream: its epoch, and numbers that go on across loads while the epoch holds (stream_start).
    std::string epoch;
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

    Settings settings;          // the session's own: its `:` commands read and change them
    SessionCommands commands;   // and keep their state here
    bool titles = false;        // small_model titles it after its first turn (sessions an in-process host opened)
    bool titled = false;
    bool shell_running = false;  // a `!cmd` (maic.session.shell)
    std::atomic<bool> shell_cancel{false};
    long shells = 0;
    int reported_calls = 0;      // the model calls the last maic.usage.updated covered
    size_t reported_queued = 0;  // the queued messages the index last showed

    // Responses and turns: ids <session>.r<k> by `responses`; queued turns reserve their number at arrival.
    long responses = 0;
    int turns_reserved = 0;
    int turn = 0;  // the running (or paused) turn's number
    Run last;      // the last response that ended: a paused turn's, what a successor continues
    struct Queued {
        Run run;
        std::string text;
        Origin origin;
    };
    std::deque<Queued> lane;  // response.create on a busy lane, run in order after the running turn

    // Steering (section 11).
    struct Text {
        bool thinking;
        std::string id;
        long index;
        std::string text;
    };
    std::optional<Text> text;          // the reply being written
    std::string running_call;          // the call_id of the tool in progress
    std::optional<Steer> steer;        // a stop-now steer the worker has yet to apply; cancel is up
    json accepted = json::array();     // accepted steers waiting for their successor: {id, previous_response_id, text}
    std::string successor_by = "steer";  // what the next steered response ends by
    std::string ended_by;              // keep or halt: the turn ends that way, and later steers answer response_not_active
    bool paused = false;
    std::optional<Queued> resume;      // the successor that resumes a paused turn, and its input
    std::optional<Steer> pause_end;    // keep or halt on a paused turn

    Session(fs::path ws, std::string model) : workspace(std::move(ws)), agent(workspace, std::move(model)) {}
};

}  // namespace

struct Engine::Impl {
    explicit Impl(EngineOptions o) : options(std::move(o)), instance(random_id(6)) {
        if (options.tier != "open" && options.tier != "guarded") {
            throw std::runtime_error("protocol tier " + options.tier + " is not available here (airtight needs a build with the conformance stamp)");
        }
        if (options.protocol_log.empty()) options.protocol_log = state_dir() / "engine" / "protocol.log";
        std::error_code ec;
        for (auto& w : options.workspaces) w = fs::weakly_canonical(w, ec);
        load_index();
    }

    EngineOptions options;
    std::string instance;
    std::atomic<long> clients_made{0}, approvals_made{0}, questions_made{0}, steers_made{0};
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
                  {"think", s.agent.think},
                  {"harness", s.agent.review_with_model ? "smart" : "dumb"},
                  {"tier", options.tier},
                  {"created", s.created},
                  {"last_activity", s.last_activity},
                  {"turns", s.turns},
                  {"unseen", false},
                  {"queued", s.agent.queued() + s.lane.size()},
                  {"response", s.run ? json(s.run->id) : s.paused ? json(s.last.id) : json()},
                  {"waiting", s.waiting}};
        if (s.log) e["transcript"] = s.log->path().string();
        return e;
    }

    static json for_client(json e, const Client& c) {
        if (c.origin == Origin::Remote) e.erase("transcript");
        return e;
    }

    void index_changed(Session& s) {
        s.reported_queued = s.agent.queued();
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

    // A new load of a session. Its first event is its state, carrying the stream's header: the epoch, the protocol
    // hash, and for a new epoch what it replaced or forked from. The transcript gets the same as `epoch` and `stream`
    // records; shutdown closes the `stream` with the number the next load goes on from.
    void open_session(const std::shared_ptr<Session>& s, const Client& c) {
        {
            std::lock_guard lock(mu);
            sessions[s->id] = s;
        }
        std::lock_guard lock(s->mu);
        StreamStart start = s->log ? stream_start(s->log->path()) : StreamStart{};
        bool fresh = start.epoch.empty();
        s->epoch = fresh ? random_id(16) : start.epoch;
        s->next = start.next;
        if (s->next > 0) s->order.join(s->next - 1);  // the epoch goes on: its numbers before this load were checked by the one that sent them
        json state = {{"type", "maic.session.state"}, {"state", s->state}, {"activity", s->activity}, {"waiting", nullptr}, {"by", c.by()},
                      {"epoch", s->epoch}, {"protocol", {{"hash", protocol::protocol_hash()}, {"canonical", kCanonical}}}};
        if (fresh) {
            json e = {{"epoch", s->epoch}};
            if (!start.previous.is_null()) e["previous"] = state["previous_epoch"] = start.previous;
            if (!start.forked_from.is_null()) e["forked_from"] = state["forked_from"] = start.forked_from;
            if (s->log) s->log->write("epoch", e);
        }
        if (s->log) s->log->write("stream", {{"epoch", s->epoch}, {"from", s->next}, {"protocol", protocol::protocol_hash()}, {"canonical", kCanonical}});
        emit(*s, state);
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
                  {"previous_response_id", r.previous.empty() ? json() : json(r.previous)},
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
        if (!u.normalized.empty()) out["normalized"] = u.normalized;
        return out;
    }

    // Mid-response: a model call finished since the last report, or a queued message was delivered.
    void refresh(Session& s) {
        if (int calls = s.agent.usage().calls; calls != s.reported_calls) {
            s.reported_calls = calls;
            json u = usage_update(s);
            u["type"] = "maic.usage.updated";
            emit(s, u);
        }
        if (s.agent.queued() != s.reported_queued) index_changed(s);
    }

    // ---------- turns ----------

    class TurnEvents;
    void run_turns(std::shared_ptr<Session> s, std::string text, Origin origin);
    // Opens the turn's first response now, under the session's lock, and runs it on the worker.
    void start_turn(const std::shared_ptr<Session>& s, Run r, std::string text, Origin origin) {
        if (s->worker.joinable()) s->worker.join();  // it set running = false and needs the lock no more
        s->running = true;
        s->cancel = false;
        s->cancel_by = nullptr;
        take_leftovers(*s, text, origin);
        open_response(*s, std::move(r));
        s->worker = std::thread([this, s, text, origin] { run_turns(s, text, origin); });
    }

    // Stops what runs: the turn is cancelled, and what waits for a person is answered `answer`. A cancel wins over
    // a steer not yet applied.
    void interrupt(Session& s, const json& by, ApprovalAnswer answer) {
        if (!s.running) return;
        s.cancel = true;
        s.cancel_by = by;
        s.steer.reset();
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
        // The event types the connection is not sent: those of a capability it lacks, and the deltas it excludes.
        const json& filter = protocol::Schemas::get().ordering().at("filter");
        std::set<std::string> exclude;
        json caps = p.value("capabilities", json::array());
        for (const auto& [cap, types] : filter.at("capabilities").items()) {
            if (std::find(caps.begin(), caps.end(), cap) == caps.end()) exclude.insert(types.begin(), types.end());
        }
        const json& filterable = filter.at("filterable");
        auto known = protocol::Schemas::get().event_types();
        for (const auto& t : p.value("exclude", json::array())) {
            if (std::find(filterable.begin(), filterable.end(), t) != filterable.end()) {
                exclude.insert(t.get<std::string>());
            } else if (std::find(known.begin(), known.end(), t.get<std::string>()) != known.end()) {
                throw refuse("maic_not_filterable", t.get<std::string>() + " cannot be filtered out: only deltas can, since their item's done event carries what they did", "exclude");
            }
        }
        {
            std::lock_guard lock(c.mu);
            c.exclude = exclude;
        }
        c.hello = true;
        // A local client names itself; a remote one is named by its token or pairing (section 2).
        if (c.origin == Origin::Local && p.contains("client") && p["client"].is_object()) {
            if (std::string name = p["client"].value("name", ""); !name.empty()) c.name = name;
        }
        return {{"protocol", kProtocol},
                {"engine", {{"version", MAIC_VERSION}, {"instance", instance}}},
                {"client", c.id},
                {"origin", origin_name(c.origin)},
                {"capabilities", {"tool_output", "index"}},
                {"exclude", exclude},
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
        s->settings = options.settings;
        s->commands.dumb_auto_ok = options.settings.dumb_auto_ok;
        configure(s->agent, *mode);
        if (options.setup) options.setup(s->agent, s->settings);
        s->log = std::make_unique<SessionLog>(options.kind, options.settings.record ? resolve_sessions_home(options.settings, ws) : runtime_sessions_dir());
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
        s->settings = options.settings;
        s->commands.dumb_auto_ok = options.settings.dumb_auto_ok;
        configure(s->agent, mode);
        if (options.setup) options.setup(s->agent, s->settings);
        s->log = std::make_unique<SessionLog>(SessionLog::Reopen{}, info->path);
        s->id = info->id;
        s->title = info->title;
        s->turns = s->turns_reserved = static_cast<int>(info->turns);
        s->responses = s->turns;
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
            c.skip_from.erase(s.id);
        }
        if (replay) {
            for (const auto& [e, size] : s.ring) {
                if (e["sequence_number"].get<long>() >= from) c.event(e, size);
            }
        }
        return {{"epoch", s.epoch}, {"sequence_number", s.next - 1}, {"activity", s.activity}, {"replay_from", from}};
    }

    std::shared_ptr<Client> clients_ptr(const Client& c) {
        std::lock_guard lock(mu);
        return clients.at(c.id);
    }

    json session_subscribe(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::lock_guard lock(s->mu);
        if (p.contains("epoch") && p["epoch"] != s->epoch) throw refuse("maic_resync", "the session's history is another epoch now (" + s->epoch + "); attach again", "epoch");
        return subscribe_locked(*s, c, p.value("starting_after", -1L), true);
    }

    json session_unsubscribe(Client& c, const json& p) {
        auto s = session(p.at("session"));
        auto self = clients_ptr(c);
        std::lock_guard lock(s->mu);
        s->subscribers.erase(std::remove(s->subscribers.begin(), s->subscribers.end(), self), s->subscribers.end());
        std::lock_guard cl(c.mu);
        c.sessions.erase(s->id);
        c.skip_from.erase(s->id);
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
                {"epoch", s->epoch},
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
            if (k != "session" && k != "mode" && k != "confirm") throw bad_params("maic.session.set takes mode in this build; " + k + " is not settable yet", k);
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
            if (*mode == Mode::Auto && now != Mode::Auto && !s->agent.review_with_model && !s->commands.dumb_auto_ok) {
                if (!p.value("confirm", false)) {
                    throw refuse("maic_confirm_required", "auto mode under a dumb harness: no model reads the conversation before the agent acts, so only the rule list "
                                                          "stands between the agent and your shell. Send confirm: true to go ahead.", "confirm");
                }
                s->commands.dumb_auto_ok = true;
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

    // A new turn's first response: its id and turn number, taken now (a queued one keeps them until it runs).
    Run reserve_turn(Session& s, Origin origin) {
        Run r;
        r.turn = ++s.turns_reserved;
        r.id = s.id + ".r" + std::to_string(++s.responses);
        r.created_at = static_cast<long>(std::time(nullptr));
        r.origin = origin;
        return r;
    }

    // The next response of the turn, continuing the last one; the turn's origin only rises.
    Run successor(Session& s, Origin origin) {
        Run r;
        r.turn = s.turn;
        r.id = s.id + ".r" + std::to_string(++s.responses);
        r.previous = s.last.id;
        r.created_at = static_cast<long>(std::time(nullptr));
        r.origin = s.last.origin == Origin::Remote ? Origin::Remote : origin;
        return r;
    }

    // `r` becomes the running response: response.created, the commit point of every accepted steer.
    void open_response(Session& s, Run r) {
        if (r.previous.empty()) {
            ++s.turns;
            s.turn = r.turn;
        }
        r.usage_before = s.agent.usage();
        // A successor answers the first steer it commits: its input, or the steer itself when it had none.
        if (r.cause < 0 && !s.accepted.empty()) r.cause = s.accepted[0].value("cause", -1L);
        s.run = std::move(r);
        s.accepted = json::array();
        json created = {{"type", "response.created"}, {"response", response_object(s, *s.run, "in_progress")}};
        if (s.run->cause >= 0) created["maic"] = {{"cause", s.run->cause}};
        emit(s, created);
        set_activity(s, "working");
        emit(s, {{"type", "response.in_progress"}, {"response", response_object(s, *s.run, "in_progress")}});
    }

    // Ends the running response: `type` with `status`, what ended it, whether the turn ends with it.
    void end_response(Session& s, const std::string& type, const std::string& status, const std::string& ended_by, bool final, json extra = json::object()) {
        Run& r = *s.run;
        if (s.agent.turn_origin() == Origin::Remote) r.origin = Origin::Remote;
        json usage = usage_update(s);
        usage["type"] = "maic.usage.updated";
        s.reported_calls = usage["calls"];
        emit(s, usage);
        json resp = response_object(s, r, status);
        for (const auto& [k, v] : extra.items()) resp[k] = v;
        if (!ended_by.empty()) resp["maic"]["ended_by"] = ended_by;
        Agent::UsageReport u = s.agent.usage();
        resp["usage"] = usage_json(u.total_input - r.usage_before.total_input, u.total_output - r.usage_before.total_output);
        resp["completed_at"] = static_cast<long>(std::time(nullptr));
        resp["maic"]["final"] = final;
        emit(s, {{"type", type}, {"response", resp}});
        s.last = std::move(r);
        s.run.reset();
    }

    // A halted turn's error, OpenAI's error event, before its maic.response.cancelled.
    void halt_error(Session& s) {
        emit(s, {{"type", "error"}, {"code", "maic_halted"}, {"message", s.settings.steering.halt_message}, {"param", nullptr}});
    }

    // What a cancel or an earlier turn left in the mailbox joins the input of the turn starting now.
    void take_leftovers(Session& s, std::string& text, Origin& origin) {
        for (const auto& q : s.agent.take_queued()) {
            text += (text.empty() ? "" : "\n\n") + q.text;
            if (q.origin == Origin::Remote) origin = Origin::Remote;
        }
    }

    json response_create(Client& c, const json& p) {
        std::string sid;
        if (p.contains("conversation")) sid = p["conversation"].is_string() ? p["conversation"].get<std::string>() : p["conversation"].value("id", "");
        if (p.contains("stream_id")) {
            if (!sid.empty() && sid != p["stream_id"].get<std::string>()) throw bad_params("stream_id and conversation name different sessions", "stream_id");
            sid = p["stream_id"];
        }
        if (sid.empty()) throw bad_params("conversation or stream_id names the session", "conversation");
        // maic.now: delivered into the running response at once, its model call abandoned (`:w now`); with no
        // input, what is already queued goes in now.
        bool now = p.contains("maic") && p["maic"].is_object() && p["maic"].value("now", false);
        std::string text = p.contains("input") || !now ? input_text(p.at("input")) : "";
        if (text.empty() && !now) throw bad_params("the input is empty", "input");
        auto s = session(sid);
        std::lock_guard lock(s->mu);
        long before = s->next - 1;
        if (text.empty()) {
            if (!s->running || !s->run) throw refuse("response_not_active", "nothing is running to deliver into", "input");
            s->agent.deliver_now();
            json r = response_object(*s, *s->run, "in_progress");
            r["maic"]["queued"] = true;
            r["maic"]["sequence_number"] = before;
            return r;
        }
        json item = {{"id", "~" + std::to_string(s->next)}, {"type", "message"}, {"role", "user"}, {"content", {{{"type", "input_text"}, {"text", text}}}}};
        if (s->paused) {
            // The paused turn resumes: a successor of its last response carries the input.
            emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", false}, {"by", c.by()}});
            if (!s->resume) {
                s->resume = Session::Queued{successor(*s, c.origin), text, c.origin};
                s->resume->run.cause = s->next - 1;
            } else {
                s->resume->text += "\n\n" + text;
            }
            s->cv.notify_all();
            json r = response_object(*s, s->resume->run, "in_progress");
            r["maic"]["sequence_number"] = before;
            return r;
        }
        if (s->running && now && s->run) {
            // Into the running response at once, as a steer: its model call is abandoned and a successor has it.
            std::string id = "st" + std::to_string(++steers_made);
            emit(*s, {{"type", "response.steer.accepted"}, {"steer", {{"id", id}, {"previous_response_id", s->run->id}}}});
            emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", true}, {"by", c.by()}});
            s->accepted.push_back({{"id", id}, {"previous_response_id", s->run->id}, {"text", text}, {"cause", s->next - 1}});
            s->agent.post_message(text, c.origin);
            s->agent.deliver_now();
            index_changed(*s);
            json r = response_object(*s, *s->run, "in_progress");
            r["maic"]["queued"] = true;
            r["maic"]["sequence_number"] = before;
            return r;
        }
        if (s->running) {
            // FIFO on the lane: a turn of its own once the running one (and those queued before it) end.
            Run r = reserve_turn(*s, c.origin);
            emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", true}, {"by", c.by()}});
            r.cause = s->next - 1;
            s->lane.push_back({r, text, c.origin});
            index_changed(*s);
            json resp = response_object(*s, r, "queued");
            resp["maic"]["queued"] = true;
            resp["maic"]["sequence_number"] = before;
            return resp;
        }
        // The pictures attached since the last message go with this one.
        if (auto pics = s->agent.pending_images(); !pics.empty()) item["maic"] = {{"images", pics}};
        emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", false}, {"by", c.by()}});
        Run run = reserve_turn(*s, c.origin);
        run.cause = s->next - 1;
        json r = response_object(*s, run, "in_progress");
        start_turn(s, std::move(run), text, c.origin);
        r["maic"]["sequence_number"] = before;
        return r;
    }

    // The session a response id names (<session id>.r<k>).
    std::shared_ptr<Session> session_of(const std::string& rid, const std::string& code, const std::string& param) {
        size_t dot = rid.rfind(".r");
        if (dot == std::string::npos) throw refuse(code, "no response " + rid, param);
        try {
            return session(rid.substr(0, dot));
        } catch (const RpcError&) {
            throw refuse(code, "no response " + rid, param);
        }
    }

    // Whether the session ever had a response of this id.
    static bool known_response(const Session& s, const std::string& rid) {
        size_t dot = rid.rfind(".r");
        if (dot == std::string::npos || rid.substr(0, dot) != s.id) return false;
        long k = std::atol(rid.c_str() + dot + 2);
        return k >= 1 && k <= s.responses;
    }

    json cancel_response(Client& c, const json& p) {
        std::string rid = p.at("response_id");
        auto s = session_of(rid, "maic_not_found", "response_id");
        std::lock_guard lock(s->mu);
        for (auto it = s->lane.begin(); it != s->lane.end(); ++it) {
            if (it->run.id != rid) continue;
            // Still waiting on the lane: it never runs, and the turns queued behind it move up a number.
            json r = response_object(*s, it->run, "cancelled");
            for (auto later = std::next(it); later != s->lane.end(); ++later) --later->run.turn;
            --s->turns_reserved;
            s->lane.erase(it);
            index_changed(*s);
            return r;
        }
        bool current = s->run && s->run->id == rid, paused = s->paused && s->last.id == rid;
        if (!s->running || !(current || paused)) throw refuse("maic_not_found", "response " + rid + " is not running", "response_id");
        interrupt(*s, c.by(), ApprovalAnswer{Approval::No, "interrupted by the user"});
        return response_object(*s, current ? *s->run : s->last, "cancelled");
    }

    // ---------- steering (section 11) ----------

    // What a steer's target must be: the running response, or a paused turn's last.
    void steer_target(Session& s, const std::string& rid, const std::string& param) {
        for (const auto& q : s.lane) {
            if (q.run.id == rid) throw refuse("response_not_active", "response " + rid + " waits on the lane; it takes no steer until it runs", param);
        }
        if (s.running && !s.ended_by.empty()) throw refuse("response_not_active", "the turn is ending (" + s.ended_by + "): it takes no more steers", param);
        if (s.running && ((s.run && s.run->id == rid) || (s.paused && s.last.id == rid))) return;
        if (known_response(s, rid)) throw refuse("response_already_completed", "response " + rid + " has ended", param);
        throw refuse("response_not_found", "no response " + rid + " on this session", param);
    }

    // The text the model gets for an action and its note.
    std::string steer_text(const Session& s, const std::string& action, const std::string& note) {
        std::string more = note.empty() ? "" : " " + note;
        if (action == "steer") {
            if (note.empty()) return "Continue from where you stopped.";
            bool stop = std::string(".!?").find(note.back()) != std::string::npos;
            return "The user redirected you: " + note + (stop ? "" : ".") + " Continue from where you stopped, following it.";
        }
        if (action == "drop") return "The user dropped the topic you had started; it was removed from your reply. Leave it and do not return to it." + more;
        if (action == "further") return "The user asks you to go deeper on what you were just saying." + more;
        if (action == "halt") return s.settings.steering.halt_message;
        return "";
    }

    // How much of a partial reply a drop keeps: up to where the paragraph or sentence being written began, none
    // of it, or all of it; `at`, a byte offset chosen in the client, overrides.
    static size_t trim_to(const std::string& text, const std::string& trim, long at) {
        if (at >= 0) return whole_chars(text, std::min(static_cast<size_t>(at), text.size()));
        if (trim == "none") return text.size();
        if (trim == "all") return 0;
        if (trim == "paragraph") {
            size_t p = text.rfind("\n\n");
            return p == std::string::npos ? 0 : p + 2;
        }
        for (size_t i = text.size(); i-- > 0;) {
            if (text[i] == '\n') return i + 1;
            if ((text[i] == '.' || text[i] == '!' || text[i] == '?') && i + 1 < text.size() && text[i + 1] == ' ') return i + 2;
        }
        return 0;
    }

    // Accepted steers a keep or halt leaves uncommitted: back out of the mailbox, and response.steer.failed.
    void fail_accepted(Session& s) {
        if (s.accepted.empty()) return;
        std::vector<std::string> texts;
        for (const auto& a : s.accepted) texts.push_back(a["text"]);
        for (const auto& q : s.agent.take_queued()) {
            auto it = std::find(texts.begin(), texts.end(), q.text);
            if (it != texts.end()) texts.erase(it);
            else s.agent.post_message(q.text, q.origin);
        }
        for (const auto& a : s.accepted) {
            emit(s, {{"type", "response.steer.failed"},
                     {"steer", {{"id", a["id"]}, {"previous_response_id", a["previous_response_id"]}, {"input", a["text"]}}},
                     {"error", {{"type", "invalid_request_error"}, {"code", "response_not_active"}, {"message", "the turn ended (" + s.ended_by + ") before this input reached the model; send it again to start the next turn"}}}});
        }
        s.accepted = json::array();
    }

    // Applies a steer under the session's lock: redirects that wait go to the mailbox for the next boundary; the rest
    // stop now (cancel rises) and the worker finishes them in TurnEvents::stopped. `tool`: cancel or wait.
    json apply_steer(Session& s, Steer st, const std::string& tool) {
        bool redirect = st.action == "steer" || st.action == "drop" || st.action == "further";
        json result = {{"steer", {{"id", st.id}, {"previous_response_id", st.response}}}};
        json applied = {{"type", "maic.steer.applied"}, {"steer", st.id},    {"action", st.action},         {"trigger", st.trigger},
                        {"by", st.by},                  {"trimmed", nullptr}, {"withdrawn", json::array()}, {"cancelled_tool", nullptr}};
        if (!st.ban.is_null()) applied["ban"] = st.ban;
        if (!st.note.empty()) applied["note"] = st.note;
        json record = {{"steer", st.id}, {"action", st.action}, {"by", st.by}, {"trigger", st.trigger}, {"note", st.note}, {"response", st.response}};
        if (!st.ban.is_null()) record["ban"] = st.ban;
        std::string say = steer_text(s, st.action, st.note);
        auto accept = [&] {
            if (s.accepted.size() >= kMaxPendingSteers) throw refuse("too_many_pending_steers", "too many steers wait for this response already", "action");
            emit(s, {{"type", "response.steer.accepted"}, {"steer", result["steer"]}});
            s.accepted.push_back({{"id", st.id}, {"previous_response_id", st.response}, {"text", say}, {"cause", s.next - 1}});
        };
        if (s.paused) {
            if (st.action == "interrupt") throw refuse("response_not_active", "the turn is paused already", "action");
            if (redirect) accept();
            emit(s, applied);
            s.log->write("steer", record);
            if (redirect) {
                if (!s.resume) s.resume = Session::Queued{successor(s, st.origin), say, st.origin};
                else s.resume->text += "\n\n" + say;
            } else {
                s.pause_end = st;
                s.ended_by = st.action;
                fail_accepted(s);
            }
            s.cv.notify_all();
            return result;
        }
        std::string pending;  // an approval or question waiting for a person
        for (const auto& [id, a] : s.approvals) {
            if (!a.answer && pending.empty()) pending = id;
        }
        for (const auto& [id, q] : s.questions) {
            if (!q.answer && pending.empty()) pending = id;
        }
        std::string running = pending.empty() ? s.running_call : "";
        if (st.action == "further" || (redirect && tool == "wait" && !running.empty())) {
            accept();
            applied["waits_for"] = !pending.empty() ? pending : !running.empty() ? running : "boundary";
            emit(s, applied);
            s.log->write("steer", record);
            s.agent.post_message(say, st.origin);
            s.successor_by = st.action;
            index_changed(s);
            return result;
        }
        if (s.steer) throw refuse("too_many_pending_steers", "a steer is stopping this response already", "action");
        if (redirect) accept();
        if (s.text && !s.text->thinking) {
            const std::string& t = s.text->text;
            st.keep = st.action == "drop" ? trim_to(t, st.trim, st.at) : st.action == "halt" ? 0 : t.size();
            if (st.action == "drop" && st.keep < t.size()) applied["trimmed"] = {{"item_id", s.text->id}, {"from", st.keep}, {"to", t.size()}};
            if (st.action == "halt") record["discarded"] = t.size();
        }
        for (auto& [id, a] : s.approvals) {
            if (a.answer) continue;
            a.answer = ApprovalAnswer{Approval::No, kNotRun, true};
            a.by = st.by;
            applied["withdrawn"].push_back(id);
        }
        for (auto& [id, q] : s.questions) {
            if (q.answer) continue;
            q.answer = "";
            q.by = st.by;
            q.withdrawn = true;
            applied["withdrawn"].push_back(id);
        }
        if (!running.empty()) applied["cancelled_tool"] = running;
        record["trimmed"] = applied["trimmed"];
        record["withdrawn"] = applied["withdrawn"];
        emit(s, applied);
        s.log->write("steer", record);
        s.steer = std::move(st);
        s.cancel = true;
        s.cv.notify_all();
        return result;
    }

    // A ban entry's steer, applied as a person's would be, by the engine on the bans' behalf.
    void ban_steer(Session& s, const BanHit& hit) {
        Steer st;
        st.id = "st" + std::to_string(++steers_made);
        st.action = hit.action;
        st.note = hit.note;
        st.trigger = "ban";
        st.by = {{"client", "engine"}, {"name", "bans"}, {"origin", origin_name(s.agent.turn_origin())}};
        st.ban = {{"list", hit.list}, {"index", hit.index}};
        st.response = s.run->id;
        st.trim = s.settings.steering.drop_trim;
        apply_steer(s, std::move(st), "cancel");
    }

    // maic.steer, and :steer: one of the six actions on the running response, by a client.
    json steer_locked(Session& s, Client& c, const json& p) {
        std::string rid = p.at("response_id"), action = p.at("action");
        auto all = SteeringSettings::steer_actions();
        if (std::find(all.begin(), all.end(), action) == all.end()) throw bad_params("action must be steer, drop, further, interrupt, keep or halt", "action");
        const SteeringSettings& st = s.settings.steering;
        if (!st.allows(action, c.origin == Origin::Remote)) {
            throw refuse("maic_steer_disabled", "the " + action + " steer is not accepted " + (c.origin == Origin::Remote ? "from a remote client " : "") + "here (steering in settings; :steering shows it)", "action");
        }
        steer_target(s, rid, "response_id");
        Steer steer;
        steer.id = "st" + std::to_string(++steers_made);
        steer.action = action;
        steer.note = p.value("note", "");
        steer.by = c.by();
        steer.response = rid;
        steer.origin = steer.note.empty() ? Origin::Local : c.origin;
        steer.trim = p.value("trim", st.drop_trim);
        steer.at = p.value("at", -1L);
        return apply_steer(s, std::move(steer), p.value("tool", st.on_running_tool));
    }

    json maic_steer(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::lock_guard lock(s->mu);
        return steer_locked(*s, c, p);
    }

    // OpenAI's response.steer: a message for the running response, delivered at its next boundary.
    json response_steer(Client& c, const json& p) {
        std::string rid = p.at("previous_response_id");
        std::string text = input_text(p.at("input"));
        if (text.empty()) throw refuse("invalid_input", "the input is empty", "input");
        auto s = session_of(rid, "response_not_found", "previous_response_id");
        std::lock_guard lock(s->mu);
        steer_target(*s, rid, "previous_response_id");
        if (s->accepted.size() >= kMaxPendingSteers) throw refuse("too_many_pending_steers", "too many steers wait for this response already", "input");
        std::string id = "st" + std::to_string(++steers_made);
        json steer = {{"id", id}, {"previous_response_id", rid}};
        emit(*s, {{"type", "response.steer.accepted"}, {"steer", steer}});
        json item = {{"id", "~" + std::to_string(s->next)}, {"type", "message"}, {"role", "user"}, {"content", {{{"type", "input_text"}, {"text", text}}}}};
        emit(*s, {{"type", "maic.input.added"}, {"item", item}, {"queued", true}, {"by", c.by()}});
        s->accepted.push_back({{"id", id}, {"previous_response_id", rid}, {"text", text}, {"cause", s->next - 1}});
        if (s->paused) {
            if (!s->resume) s->resume = Session::Queued{successor(*s, c.origin), text, c.origin};
            else s->resume->text += "\n\n" + text;
            s->cv.notify_all();
        } else {
            s->agent.post_message(text, c.origin);
            s->successor_by = "steer";
        }
        index_changed(*s);
        return {{"steer", steer}};
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

    // The title, said on the stream: `source` is auto (small_model after the first turn) or rename.
    void retitle(Session& s, const std::string& text, const std::string& source, const json& by) {
        s.title = text;
        s.titled = true;
        s.log->write("title", {{"text", text}});
        json ev = {{"type", "maic.session.title"}, {"text", text}, {"source", source}};
        if (!by.is_null()) ev["by"] = by;
        emit(s, ev);
        index_changed(s);
    }

    json update_conversation(Client& c, const json& p) {
        auto s = session(p.at("conversation_id"));
        std::string title = p.at("metadata").at("title");
        if (title.empty()) throw bad_params("metadata.title is empty", "metadata");
        std::lock_guard lock(s->mu);
        retitle(*s, title, "rename", c.by());
        return conversation(*s, c);
    }

    // A `:` command the engine owns (session_commands.cpp), or the answer to a question one asked.
    json session_command(Client& c, const json& p) {
        auto s = session(p.at("session"));
        std::lock_guard lock(s->mu);
        if (!p.contains("ask")) {
            // :steer ACTION [NOTE]: maic.steer on whatever runs; the steering settings decide who may.
            std::istringstream in(p.at("line").get<std::string>());
            std::string cmd, action, note;
            in >> cmd >> action;
            std::getline(in >> std::ws, note, '\0');
            if (cmd == "steer") {
                CommandOutput out;
                if (action.empty()) {
                    out.error(":steer ACTION [NOTE]: steer, drop, further, interrupt, keep or halt (:steering shows what is in force)");
                } else if (!s->running || (!s->run && !s->paused)) {
                    out.error("nothing is running to steer");
                } else {
                    json params = {{"response_id", s->run ? s->run->id : s->last.id}, {"action", action}};
                    if (!note.empty()) params["note"] = note;
                    try {
                        steer_locked(*s, c, params);
                        out.info(action == "interrupt" ? "pausing; Ctrl-Q resumes" : "steering: " + action);
                    } catch (const RpcError& e) {
                        out.error(e.message);
                    }
                }
                return out.json();
            }
        }
        // A question a command asked is the local person's: one answers a :cd's trust prompt, for one.
        if (c.origin == Origin::Remote && p.contains("ask")) throw refuse("maic_forbidden_remote", "a remote client cannot answer a command's question", "ask");
        if (c.origin == Origin::Remote) {
            std::string why;
            if (!SessionCommands::remote_allowed(p.at("line"), s->agent.mode.load(), why)) throw refuse("maic_forbidden_remote", why, "line");
        }
        std::function<Settings(const fs::path&)> settings_at = options.settings_at;
        if (!settings_at) settings_at = [](const fs::path& dir) { return load_settings(dir); };
        SessionCommands::Session lent{s->agent, *s->log, s->settings, s->running,
                                      [&](json fields) {
                                          fields["type"] = "maic.session.settings";
                                          fields["by"] = c.by();
                                          emit(*s, std::move(fields));
                                      },
                                      [&](const std::string& title) { retitle(*s, title, "rename", c.by()); },
                                      [&](const std::string& text) { emit(*s, {{"type", "maic.notice"}, {"text", text}, {"level", "info"}}); },
                                      settings_at};
        CommandOutput out = p.contains("ask") ? s->commands.answer(lent, p.at("ask"), p.value("key", "")) : s->commands.run(lent, p.at("line"));
        index_changed(*s);
        return out.json();
    }

    // `!cmd`: the user's own shell, unsandboxed, its output as maic.tool.output.delta with no output_index (it is
    // no response's item); then what it printed reaches the model as context, as a message typed mid-turn does.
    json session_shell(Client& c, const json& p) {
        (void)c;
        auto s = session(p.at("session"));
        if (p.value("interrupt", false)) {
            std::lock_guard lock(s->mu);
            s->shell_cancel = true;
            return {{"interrupted", s->shell_running}};
        }
        std::string command = p.at("command");
        std::string id;
        fs::path ws;
        {
            std::lock_guard lock(s->mu);
            if (s->shell_running) throw refuse("maic_busy", "a shell command is still running; Ctrl-C stops it", "command");
            s->shell_running = true;
            s->shell_cancel = false;
            id = "sh" + std::to_string(++s->shells);
            ws = s->agent.harness().workspace();
        }
        std::string output, held;
        size_t offset = 0;
        auto send = [&](std::string_view data) {
            std::lock_guard lock(s->mu);
            emit(*s, {{"type", "maic.tool.output.delta"}, {"item_id", id}, {"offset", offset}, {"data", std::string(data)}});
            offset += data.size();
        };
        int rc = run_user_shell(command, ws, s->shell_cancel, [&](std::string_view chunk) {
            output.append(chunk);
            held.append(chunk);
            size_t n = whole_chars(held, held.size());
            if (n == 0) return;
            send(std::string_view(held).substr(0, n));
            held.erase(0, n);
        });
        if (!held.empty()) send(held);
        std::string tail = rc == 0 ? "" : "\n[exit code " + std::to_string(rc) + "]";
        if (output.size() > 32 * 1024) output = output.substr(0, 32 * 1024) + "\n[truncated]";
        std::string context = "[The user ran this in their shell: `" + command + "`]\n" + output + tail;
        std::lock_guard lock(s->mu);
        if (s->running) s->agent.post_message(context);
        else s->agent.add_context(context);
        s->shell_running = false;
        return {{"exit_code", rc}, {"item_id", id}};
    }

    // A write's content after it, for a diff beside the approval (maic.approval.requested says only its size).
    json approval_proposed(Client&, const json& p) {
        auto s = session(p.at("session"));
        std::string id = p.at("approval");
        std::lock_guard lock(s->mu);
        auto it = s->approvals.find(id);
        if (it == s->approvals.end() || it->second.answer) throw refuse("maic_not_found", "no approval " + id + " is waiting", "approval");
        const auto& a = it->second;
        return {{"path", a.event.value("path", "")}, {"text", a.proposed ? json(*a.proposed) : json()}};
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
        {"response.steer", &Impl::response_steer},
        {"maic.steer", &Impl::maic_steer},
        {"maic.approval.answer", &Impl::approval_answer},
        {"maic.question.reply", &Impl::question_reply},
        {"updateConversation", &Impl::update_conversation},
        {"maic.session.command", &Impl::session_command},
        {"maic.session.shell", &Impl::session_shell},
        {"maic.approval.proposed", &Impl::approval_proposed},
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
        if (s_.steer) return;  // a steer stopped the reply where it stood; what streams until the call ends is not shown
        if (s_.text && s_.text->thinking != thinking) close_text("completed");
        if (!s_.text) open_text(thinking);
        s_.text->text.append(delta);
        json ev = {{"type", thinking ? "response.reasoning_text.delta" : "response.output_text.delta"}, {"item_id", s_.text->id}, {"output_index", s_.text->index},
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
        e_.refresh(s_);
        if (!proposed_) {
            // A line about the call, not a call ("↳ explore on ...").
            e_.emit(s_, {{"type", "maic.notice"}, {"text", summary}, {"level", "info"}, {"kind", "tool_call"}});
            return;
        }
        ToolCall call = std::move(*proposed_);
        proposed_.reset();
        Frame f{call.id, call.name, call.name == "run_shell", !calls_.empty()};
        if (f.child) {
            // A subagent's call: its items belong to its own session (step 14); a notice says it once
            // on_tool_started has named the tool and its path.
            f.pending = summary;
            calls_.push_back(f);
            return;
        }
        close_text("completed");
        f.index = s_.run->next_index++;
        f.item_id = "~" + std::to_string(s_.next);
        s_.running_call = call.id;
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

    void on_tool_started(const std::string& tool, const std::string& path, const std::string&) override {
        std::lock_guard lock(s_.mu);
        if (!calls_.empty() && calls_.back().child) say_child_call(calls_.back(), tool, path);
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
        if (!f.child) s_.running_call.clear();
        if (f.child) {
            say_child_call(f, "", "");
            json ev = {{"type", "maic.notice"}, {"text", text}, {"level", "info"}, {"kind", "tool_result"}, {"ok", ok}};
            if (!f.full_output.is_null()) ev["full_output"] = f.full_output;
            e_.emit(s_, ev);
            return;
        }
        open_output(f);
        if (s_.steer && s_.steer->action == "halt") {
            // A halt discards what the call printed: the item closes empty.
            json item = f.shell ? json{{"type", "shell_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"status", "incomplete"}, {"output", json::array()},
                                       {"max_output_length", nullptr}}
                                : json{{"type", "function_call_output"}, {"id", f.out_id}, {"call_id", f.call_id}, {"output", ""}, {"status", "incomplete"}};
            item["maic"] = {{"ok", false}, {"status", "discarded"}};
            e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", f.out_index}, {"item", item}});
            output().push_back(item);
            return;
        }
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
        e_.refresh(s_);
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
        s_.approvals[id] = PendingApproval{ev, r.proposed, std::nullopt, nullptr};
        e_.set_activity(s_, "waiting", {{"kind", "approval"}, {"id", id}, {"tool", r.tool}, {"summary", r.summary}});
        s_.cv.wait(lock, [&] { return s_.approvals[id].answer.has_value() || s_.cancel.load(); });
        PendingApproval p = s_.approvals[id];
        s_.approvals.erase(id);
        s_.answered.insert(id);
        ApprovalAnswer answer = p.answer.value_or(ApprovalAnswer{Approval::No, "interrupted by the user"});
        json by = p.by.is_null() ? (s_.cancel_by.is_null() ? json{{"client", "engine"}, {"name", "engine"}, {"origin", "local"}} : s_.cancel_by) : p.by;
        json done = {{"type", "maic.approval.answered"}, {"id", id}, {"choice", answer.withdrawn ? "withdrawn" : choice_name(answer.choice)}, {"by", by}};
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
        e_.emit(s_, {{"type", "maic.question.answered"}, {"id", id}, {"by", by}, {"withdrawn", q.withdrawn}});
        settle_activity();
        return q.answer.value_or("");
    }

    // Messages typed during the turn reach the model at this boundary: the response ends steered, and a successor,
    // the commit point of the steers accepted for it, carries them.
    void on_delivered(const std::vector<std::string>&, bool remote) override {
        std::lock_guard lock(s_.mu);
        close_all("incomplete");  // a reply abandoned for :w now
        std::string by = s_.successor_by;
        s_.successor_by = "steer";
        e_.end_response(s_, "response.incomplete", "incomplete", by, false, {{"incomplete_details", {{"reason", "steered"}}}});
        e_.open_response(s_, e_.successor(s_, remote ? Origin::Remote : Origin::Local));
    }

    // The turn stopped: a cancel ends it; a steer (a client's, or a ban entry's that fired) is finished here.
    Redirect stopped(const BanHit* ban) override {
        std::lock_guard lock(s_.mu);
        if (ban && !s_.steer && !s_.cancel.load()) e_.ban_steer(s_, *ban);
        if (!s_.steer) return {};
        Steer st = std::move(*s_.steer);
        s_.steer.reset();
        s_.cancel = false;
        Redirect r;
        r.unrun = st.action == "halt" ? "[discarded: the user halted this turn]" : kNotRun;
        if (s_.text) {
            if (!s_.text->thinking && st.action != "halt") r.kept = s_.text->text.substr(0, std::min(st.keep, s_.text->text.size()));
            close_text("incomplete", st.action == "drop" ? st.keep : std::string::npos, st.action == "halt");
        }
        if (st.action == "steer" || st.action == "drop") {
            r.then = Redirect::Then::Continue;
            s_.agent.post_message(e_.steer_text(s_, st.action, st.note), st.origin);
            s_.successor_by = st.action;
        } else if (st.action == "interrupt") {
            r.then = Redirect::Then::Pause;
            close_all("incomplete");
            e_.end_response(s_, "maic.response.cancelled", "cancelled", "interrupt", false);
            s_.paused = true;
            e_.emit(s_, {{"type", "maic.turn.paused"}, {"turn", s_.turn}, {"steer", st.id}, {"response_id", s_.last.id}});
            e_.set_activity(s_, "waiting", {{"kind", "steer"}, {"id", st.id}, {"summary", "paused"}});
        } else {
            r.then = st.action == "keep" ? Redirect::Then::Keep : Redirect::Then::Halt;
            if (st.action == "halt") r.say = s_.settings.steering.halt_message;
            s_.ended_by = st.action;
            e_.fail_accepted(s_);
        }
        return r;
    }

    // The response is ending: what is still open closes, `status` for a reply cut short.
    void close_all(const std::string& status) {
        close_text(status);
        s_.running_call.clear();
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
    struct Frame {
        std::string call_id, name;
        bool shell = false, child = false;
        long index = -1;
        std::string item_id;
        long out_index = -1;
        std::string out_id;
        json full_output;
        std::string pending;  // a subagent's call not yet said
    };

    void say_child_call(Frame& f, const std::string& tool, const std::string& path) {
        if (f.pending.empty()) return;
        json ev = {{"type", "maic.notice"}, {"text", f.pending}, {"level", "info"}, {"kind", "tool_call"}};
        if (!tool.empty()) ev["tool"] = tool;
        if (!path.empty()) ev["path"] = path;
        e_.emit(s_, ev);
        f.pending.clear();
    }

    json& output() { return s_.run->output; }

    void notice_locked(const std::string& text) {
        std::string level = text.rfind("HARNESS TRIPPED", 0) == 0 || text.rfind("HALTED", 0) == 0 ? "error" : "info";
        e_.emit(s_, {{"type", "maic.notice"}, {"text", text}, {"level", level}});
    }

    void open_text(bool thinking) {
        s_.text = Session::Text{thinking, "~" + std::to_string(s_.next), s_.run->next_index++, ""};
        const auto& t = *s_.text;
        json item = thinking ? json{{"type", "reasoning"}, {"id", t.id}, {"summary", json::array()}, {"content", json::array()}, {"status", "in_progress"}}
                             : json{{"type", "message"}, {"id", t.id}, {"role", "assistant"}, {"status", "in_progress"}, {"content", json::array()}};
        e_.emit(s_, {{"type", "response.output_item.added"}, {"output_index", t.index}, {"item", item}});
        e_.emit(s_, {{"type", "response.content_part.added"}, {"item_id", t.id}, {"output_index", t.index}, {"content_index", 0}, {"part", part("")}});
    }

    json part(const std::string& text) const {
        if (s_.text->thinking) return {{"type", "reasoning_text"}, {"text", text}};
        return {{"type", "output_text"}, {"text", text}, {"annotations", json::array()}, {"logprobs", json::array()}};
    }

    // The reply being written closes: `keep` bytes of it when a drop trimmed it (the done events carry what stays,
    // maic.trimmed the range removed), all of it marked discarded when a halt threw it away.
    void close_text(const std::string& status, size_t keep = std::string::npos, bool discarded = false) {
        if (!s_.text) return;
        const Session::Text& t = *s_.text;
        std::string shown = keep < t.text.size() ? t.text.substr(0, keep) : t.text;
        json done = {{"type", t.thinking ? "response.reasoning_text.done" : "response.output_text.done"}, {"item_id", t.id}, {"output_index", t.index},
                     {"content_index", 0}, {"text", shown}};
        if (!t.thinking) done["logprobs"] = json::array();
        if (keep < t.text.size()) done["maic"] = {{"trimmed", {{"from", keep}, {"to", t.text.size()}}}};
        e_.emit(s_, done);
        e_.emit(s_, {{"type", "response.content_part.done"}, {"item_id", t.id}, {"output_index", t.index}, {"content_index", 0}, {"part", part(shown)}});
        json item = t.thinking ? json{{"type", "reasoning"}, {"id", t.id}, {"summary", json::array()}, {"content", {part(shown)}}, {"status", status}}
                               : json{{"type", "message"}, {"id", t.id}, {"role", "assistant"}, {"status", status}, {"content", {part(shown)}}};
        if (discarded) item["maic"] = {{"status", "discarded"}};
        e_.emit(s_, {{"type", "response.output_item.done"}, {"output_index", t.index}, {"item", item}});
        output().push_back(item);
        s_.text.reset();
    }

    // The call's output item, announced when its first output or its result comes.
    void open_output(Frame& f) {
        if (f.out_index >= 0) return;
        f.out_index = s_.run->next_index++;
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
    std::optional<ToolCall> proposed_;
    std::vector<Frame> calls_;  // the call running, then a subagent's inside it
};

// The worker of a session's turns. The first turn's first response is open already (start_turn); each response
// runs the agent once, then ends by what stopped it: a paused turn waits for its resume or its end, steers that came
// as a reply ended continue in a successor, and queued turns come off the lane in order.
void Engine::Impl::run_turns(std::shared_ptr<Session> s, std::string text, Origin origin) {
    for (;;) {
        std::string first_text = text;
        bool cancelled = false;
        for (;;) {
            TurnEvents events(*this, *s);
            std::string failure;
            try {
                s->agent.submit(text, origin, events, s->cancel);
            } catch (const std::exception& e) {
                failure = failure_text(s->agent, e);
            }
            std::unique_lock lock(s->mu);
            if (s->steer) {
                // Accepted as the reply ended: applied now, over what the history holds already.
                lock.unlock();
                Redirect r = events.stopped(nullptr);
                lock.lock();
                if (r.then == Redirect::Then::Halt) s->agent.add_context(r.say);
            }
            if (s->paused) {
                s->cv.wait(lock, [&] { return s->resume || s->pause_end || s->cancel.load(); });
                s->paused = false;
                if (s->resume && !s->cancel.load()) {
                    Session::Queued next = std::move(*s->resume);
                    s->resume.reset();
                    text = next.text;
                    origin = next.run.origin;
                    take_leftovers(*s, text, origin);  // a `!cmd` run while it was paused
                    open_response(*s, std::move(next.run));
                    continue;
                }
                // Ended while paused: a successor opens and ends at once, so the turn closes on a final event.
                s->resume.reset();
                open_response(*s, successor(*s, Origin::Local));
                if (s->pause_end && !s->cancel.load() && s->pause_end->action == "keep") {
                    end_response(*s, "response.completed", "completed", "keep", true);
                } else if (s->pause_end && !s->cancel.load()) {
                    halt_error(*s);
                    end_response(*s, "maic.response.cancelled", "cancelled", "halt", true);
                    s->agent.add_context(s->settings.steering.halt_message);
                } else {
                    end_response(*s, "maic.response.cancelled", "cancelled", "cancel", true);
                    cancelled = true;
                }
                s->pause_end.reset();
                break;
            }
            cancelled = s->cancel.load();
            events.close_all(cancelled || !failure.empty() ? "incomplete" : "completed");
            if (s->ended_by == "halt") {
                halt_error(*s);
                end_response(*s, "maic.response.cancelled", "cancelled", "halt", true);
            } else if (s->ended_by == "keep") {
                end_response(*s, "response.completed", "completed", "keep", true);
            } else if (cancelled) {
                end_response(*s, "maic.response.cancelled", "cancelled", "cancel", true);
            } else if (!failure.empty()) {
                end_response(*s, "response.failed", "failed", "error", true, {{"error", {{"code", "server_error"}, {"message", failure}}}});
            } else if (s->agent.queued() > 0 && !stopped.load()) {
                // Steers that came after the turn's last model call: an automatic successor carries them.
                end_response(*s, "response.completed", "completed", "", false);
                text.clear();
                origin = Origin::Local;
                take_leftovers(*s, text, origin);
                Run next = successor(*s, origin);
                origin = next.origin;
                open_response(*s, std::move(next));
                continue;
            } else {
                end_response(*s, "response.completed", "completed", "", true);
            }
            break;
        }
        std::unique_lock lock(s->mu);
        s->ended_by.clear();
        s->successor_by = "steer";
        if (s->titles && !s->titled && !cancelled && !stopped.load() && !s->settings.small_model.empty()) {
            // After the first turn, a title from small_model. A remote one never titles a local session, so
            // nothing leaves the machine that would not have anyway.
            s->titled = true;
            std::optional<std::pair<Provider, std::string>> pick;
            try {
                auto picked = resolve_model(s->agent.providers, s->settings.small_model);
                if (!picked.first.remote() || s->agent.remote()) pick = picked;
            } catch (const std::exception&) {
            }
            if (pick) {
                lock.unlock();
                std::string title;
                try {
                    title = generate_title(pick->first, pick->second, first_text);
                } catch (const std::exception&) {
                }
                lock.lock();
                if (!title.empty()) retitle(*s, title, "auto", nullptr);
            }
        }
        if (s->lane.empty() || stopped.load()) {
            s->running = false;
            set_activity(*s, "idle");
            return;
        }
        // The next turn on the lane: what a cancel left in the mailbox joins it.
        Session::Queued next = std::move(s->lane.front());
        s->lane.pop_front();
        s->cancel = false;
        s->cancel_by = nullptr;
        text = next.text;
        origin = next.origin;
        take_leftovers(*s, text, origin);
        next.run.origin = origin;
        open_response(*s, std::move(next.run));
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

std::string Engine::open_local(const std::string& client, LocalSession ls) {
    auto c = impl_->client(client);
    if (c->origin != Origin::Local) throw std::runtime_error("open_local: " + client + " is not a local connection");
    auto s = std::make_shared<Session>(ls.workspace, ls.settings.model);
    s->settings = std::move(ls.settings);
    s->commands.dumb_auto_ok = s->settings.dumb_auto_ok;
    s->titles = ls.titles;
    s->log = std::move(ls.log);
    s->id = s->log->path().stem().string();
    if (ls.setup) ls.setup(s->agent, *s->log);
    impl_->open_session(s, *c);
    return s->id;
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
        s->shell_cancel = true;
    }
    for (const auto& s : all) {
        if (s->worker.joinable()) s->worker.join();
        std::lock_guard lock(s->mu);
        s->state = "parked";
        impl_->emit(*s, {{"type", "maic.session.state"}, {"state", "parked"}, {"activity", "idle"}, {"waiting", nullptr}, {"by", engine}});
        if (s->log) s->log->write("stream", {{"epoch", s->epoch}, {"next", s->next}, {"closed", true}});
        impl_->index_changed(*s);
    }
    // Every connection ends: a transport waiting in take() returns with what is left.
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lock(impl_->mu);
        for (const auto& [id, c] : impl_->clients) clients.push_back(c);
    }
    for (const auto& c : clients) {
        {
            std::lock_guard lock(c->mu);
            if (c->closed.empty()) c->closed = "maic_shutdown";
        }
        c->cv.notify_all();
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

void configure_agent(Agent& a, const Settings& st) {
    a.providers = st.providers;
    set_context(a.providers, st.context);
    set_context(a.providers, st.context_2, "llamacpp-2");
    a.think = st.think;
    a.review_with_model = st.harness != "dumb";
    a.audit = st.audit;
    a.reviewer_model = st.reviewer_model;
    a.small_model = st.small_model;
    a.reviewer_budget_tokens = st.reviewer_budget_tokens;
    a.presets = st.presets;
    a.compaction.at = st.compact_at;
    a.compaction.keep_results = st.compact_keep_results;
    a.budget_tokens = st.budget_tokens;
    a.full_output = st.full_output;
    a.full_output_max_mb = static_cast<size_t>(st.full_output_max_mb);
    a.set_instruction_options(st.instructions);
    a.load_instruction_files = st.load_instructions;
    a.system_prefix = resolve_system_prompt(st.system_prompt);
    a.prefill = resolve_system_prompt(st.prefill);
    a.rules = st.rules;
    a.set_permission(st.permission);
    a.agents = st.agents;
    a.set_forbid(st.forbid);
    a.bans = st.bans;
    apply_sampling(a, st);
}

void apply_sampling(Agent& a, const Settings& st, const json& live) {
    auto [provider, name] = resolve_model(a.providers, a.model);
    json s = st.sampling.is_object() ? st.sampling : json::object();
    json per_provider = provider.options.value("sampling", json::object());
    for (const auto& [k, v] : per_provider.items()) s[k] = v;
    for (const auto& [k, v] : live.items()) {
        if (v.is_null()) s.erase(k);
        else s[k] = v;
    }
    a.sampling = s;
    a.operator_note_in_turn = provider.options.value("operator_note", provider.kind != "anthropic");
}

std::string failure_text(const Agent& agent, const std::exception& e) {
    std::string text = e.what();
    if (const auto* api = dynamic_cast<const ApiError*>(&e); api && is_usage_limit(*api)) {
        // The session never switches by itself: say where the next tier is.
        auto p = preset_for_model(agent.presets, agent.model);
        auto next = p ? on_limit_pick(agent.presets, *p) : std::nullopt;
        text += "\n" + (p ? p->name : agent.model) + " hit its usage limit; " +
                (next ? "`:model " + next->name + "` continues on the next tier (its on_limit)" : "`:model` lists the other models");
    } else if (dynamic_cast<const TransportError*>(&e)) {
        std::string hint = unreachable_hint(resolve_model(agent.providers, agent.model).first, load_services(root_dir() / "services"));
        if (!hint.empty()) text += "\n" + hint;
    } else if (const auto* api = dynamic_cast<const ApiError*>(&e); api && api->status == 500 && text.find("failed to load") != std::string::npos) {
        auto [provider, name] = resolve_model(agent.providers, agent.model);
        if (is_llama_server(provider.name)) {
            auto services = load_services(root_dir() / "services");
            GpuReport g = gpu_report(services);
            std::string why = "llama.cpp could not load " + name + ".";
            bool oom = false;
            for (const auto& def : services) {
                if (def.name != provider.name) continue;
                std::ifstream in(service_log_path(def));
                std::deque<std::string> tail;
                for (std::string line; std::getline(in, line);) {
                    tail.push_back(line);
                    if (tail.size() > 60) tail.pop_front();
                }
                for (const auto& l : tail) oom = oom || l.find("out of memory") != std::string::npos || l.find("failed to allocate") != std::string::npos;
            }
            if (oom) {
                why += " The card ran out of memory while loading it";
                if (g.comfyui_running && g.comfyui_vram_used > 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), " (ComfyUI holds %.1f GB)", static_cast<double>(g.comfyui_vram_used) / (1 << 30));
                    why += buf;
                }
                for (const auto& s : g.servers) {
                    if (s.name != provider.name && !s.models.empty()) why += " (" + s.name + " holds " + s.models.front() + ")";
                }
                why += ". Free it with `maic gpu free` (or stop ComfyUI), lower the context (`" + std::string(provider.name == "llamacpp" ? ":ctx 8192" : ":ctx2 4096") +
                       "`), or use the smaller model; a model with a vision projector needs about 1 GB more. `maic gpu` says what fits.";
            } else {
                why += " `maic logs " + provider.name + "` has the reason.";
            }
            text += "\n" + why;
        }
    } else if (const auto* api = dynamic_cast<const ApiError*>(&e); api && api->status == 400 && text.find("not found") != std::string::npos) {
        auto [provider, name] = resolve_model(agent.providers, agent.model);
        if (is_llama_server(provider.name)) {
            text += "\nllama.cpp serves the GGUFs under " + llamacpp_models_root().string() + " by file name (:models lists them, maic vendor model fetches one)";
            if (name.find(':') != std::string::npos) text += "; a name like " + name + " is a tag, not a file name here";
        }
    }
    return text;
}

}  // namespace maic
