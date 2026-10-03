#include "tui.hpp"

#include "audit_trail.hpp"
#include "commands.hpp"
#include "daemon.hpp"
#include "editor.hpp"
#include "highlight.hpp"
#include "nvim_host.hpp"
#include "maid/agent.hpp"
#include "maid/engine.hpp"
#include "maid/artifacts.hpp"
#include "maid/clipboard.hpp"
#include "maid/full_output.hpp"
#include "maid/helper.hpp"
#include "maid/image.hpp"
#include "maid/lazy_lock.hpp"
#include "maid/models.hpp"
#include "maid/places.hpp"
#include "maid/protocol.hpp"
#include "maid/vendor.hpp"
#include "maid/nvim_host.hpp"
#include "maid/nvim_keymaps.hpp"
#include "maid/nvim_setup.hpp"
#include "maid/paths.hpp"
#include "maid/service.hpp"
#include "maid/settings.hpp"
#include "maid/status.hpp"
#include "maid/theme.hpp"
#include "maid/tools.hpp"
#include "maid/tripwire.hpp"
#include "maid/trust.hpp"
#include "style.hpp"
#include "view.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace maid {

namespace {

using namespace ftxui;

std::string human_bytes(uintmax_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 4) v /= 1024, ++u;
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

// Hard-wraps styled lines into rows of `width` columns, remembering each row's byte range in the text.
struct InputRow {
    StyledLine spans;
    size_t begin, end;
};

std::vector<InputRow> chunk_rows(const std::vector<StyledLine>& lines, size_t width) {
    std::vector<InputRow> rows;
    size_t offset = 0;
    for (const auto& line : lines) {
        InputRow row{{}, offset, offset};
        size_t col = 0;
        for (const auto& span : line) {
            for (size_t i = 0; i < span.text.size();) {
                size_t n = utf8_next(span.text, i);
                if (col == width) {
                    rows.push_back(row);
                    row = {{}, row.end, row.end};
                    col = 0;
                }
                if (!row.spans.empty() && row.spans.back().flags == span.flags) row.spans.back().text += span.text.substr(i, n - i);
                else row.spans.push_back({span.text.substr(i, n - i), span.flags});
                row.end += n - i;
                ++col;
                i = n;
            }
        }
        rows.push_back(row);
        offset = row.end + 1;  // the newline
    }
    return rows;
}

std::filesystem::path session_home_dir(const Settings& settings) {
    return resolve_sessions_home(settings, std::filesystem::current_path());
}

// An approval the engine is waiting on (maid.approval.requested).
struct PendingApproval {
    std::string id;
    nlohmann::json event;
    bool typing = false;   // after N: a sentence for the model is being typed
    std::string feedback;
};

// A question one of the engine's `:` commands asks before it goes on (entering auto under a dumb harness, a
// directory's trust on :cd): one of `keys`, Esc and Ctrl-C giving "n".
struct PendingConfirm {
    std::string id;
    std::string title;
    std::vector<std::string> lines;
    std::string keys;
    std::function<void(const std::string&)> local;  // a question the TUI asks itself: the key goes here, not to the engine
};

// The session switcher (:switch, and :bg, :park or :stop on this session): the other sessions the engine holds,
// with what each is doing and where it works, and a new one; Enter goes there.
struct Switcher {
    std::string title;
    std::string leave;             // what happens to this session: "" for the leave.switch setting's choice
    std::vector<std::string> ids;  // "" is a new session here
    size_t sel = 0;
};

// The question tool: shown like an approval; a number picks an option, typed text is a free answer.
struct PendingQuestion {
    std::string id;
    std::string text;
    std::vector<std::string> options;
    std::string typed;
};

// What the welcome says about the session, read from its agent while the engine sets it up.
struct Startup {
    std::string model;
    bool remote = false;
    Provider provider;
    std::vector<std::string> instructions, tools, tool_notices;
    std::vector<std::pair<Kind, std::string>> context;  // --context files attached before the first turn
};

enum class Focus { Input, Conversation };

// The TUI is a client of an engine (docs/design/engine-protocol.md, build steps 6 and 13): the daemon's when one
// answers and nothing on the command line needs this process's own, else one in this process. Its session's turns,
// approvals, questions, `!cmd` and the `:` commands that act on the session go through the engine, and what it
// shows comes from the engine's events. The view, the editor, themes, the nvim host and the machine's services
// stay here.
class App {
public:
    App(ScreenInteractive& screen, Settings settings, const TuiOptions& options, bool attach, std::shared_ptr<HostNvim> host, std::string host_refused,
        std::function<Settings(const std::filesystem::path&)> settings_at)
        : settings_at_(std::move(settings_at)), host_(std::move(host)), host_refused_(std::move(host_refused)), screen_(screen), settings_(std::move(settings)),
          editor_(&register_), view_(&register_) {
        view_.set_markdown(settings_.markdown);
        editor_.set_leader(settings_.leader);
        editor_.set_enter_sends(settings_.enter_sends);
        {
            // Earlier sessions' prompts, so ↑ / Ctrl-P reach them after a restart (the last 500).
            std::vector<std::string> items;
            std::ifstream f(state_dir() / "prompt-history.jsonl");
            for (std::string l; std::getline(f, l);) {
                auto j = nlohmann::json::parse(l, nullptr, false);
                if (j.is_object() && j.contains("text")) items.push_back(j.value("text", ""));
            }
            if (items.size() > 500) items.erase(items.begin(), items.end() - 500);
            editor_.set_history(std::move(items));
        }
        view_.set_leader(settings_.leader);
        view_.set_timestamps(settings_.timestamps);
        confined_ = settings_.tripwire == "isolated";
        open_recording();
        for (const auto& n : recover_relocations()) view_.append(Kind::Notice, n);
        if (attach && open_in_daemon(options)) return;
        const auto& resume = options.resume;
        bool append = options.append;
        const auto& fork_at = options.fork_at;
        const auto& context = options.context;

        auto log = !resume ? std::make_unique<SessionLog>("tui", settings_.record ? session_home_dir(settings_) : runtime_sessions_dir())
                   : append && settings_.record ? std::make_unique<SessionLog>(SessionLog::Reopen{}, *resume)
                                                : std::make_unique<SessionLog>(SessionLog::Fork{}, *resume, fork_at.value_or(count_records(*resume)), "tui",
                                                                               settings_.record ? session_home_dir(settings_) : runtime_sessions_dir());
        set_tripwire_scope(settings_.tripwire, log->path().string() + ".tripped");
        for (const auto& n : log->recovered()) view_.append(Kind::Notice, n);
        LoadedSession old;
        if (resume) {
            old = load_session(append && settings_.record ? log->path() : *resume, fork_at.value_or(~size_t(0)));
            for (const auto& t : old.transcript) {
                if (t.type == "user") view_.append(Kind::User, t.text);
                else if (t.type == "assistant") view_.append(Kind::Assistant, t.text);
                else if (t.type == "tool_call") view_.append(Kind::Tool, t.text);
                else if (t.type == "tool_result") view_.append(t.ok ? Kind::ToolOk : Kind::ToolErr, t.text);
                else view_.append(Kind::Notice, t.text);
            }
        }
        size_t entries = old.transcript.size();

        EngineOptions eo;
        eo.settings = settings_;
        eo.tier = settings_.protocol_tier;
        eo.kind = "tui";
        eo.titles = true;
        // :cd reads the new directory's settings as a start there would, flags included; the TUI keeps that copy.
        eo.settings_at = [this](const std::filesystem::path& ws) {
            Settings st = settings_at_(ws);
            cd_settings_ = st;
            return st;
        };
        // Sessions :new, :fork and :switch open are set up as this one is.
        eo.setup = [this](Agent& agent, const Settings& st) {
            configure_agent(agent, st);
            if (confined_) agent.set_confined(true);
            agent.reload_instructions();
            agent.set_nvim_host(host_);
        };
        engine_ = std::make_unique<Engine>(std::move(eo));
        client_ = engine_->connect(Origin::Local, "maid", "in-process", [this] { wake_pump(); });
        hello();

        bool want_auto = false;
        LocalSession ls;
        ls.workspace = std::filesystem::current_path();
        ls.settings = settings_;
        ls.log = std::move(log);
        ls.setup = [&](Agent& agent, SessionLog& l) {
            configure_agent(agent, settings_);
            if (auto m = parse_mode(settings_.mode)) {
                // Auto under a dumb harness is confirmed first; until then the session starts one step safer.
                want_auto = *m == Mode::Auto && !agent.review_with_model && !settings_.dumb_auto_ok;
                agent.mode = want_auto ? Mode::Edit : *m;
            }
            if (confined_) agent.set_confined(true);
            agent.reload_instructions();
            agent.set_nvim_host(host_);
            agent.set_log(&l);
            if (resume) agent.restore(std::move(old.messages));
            startup_.model = agent.model;
            startup_.remote = agent.remote();
            startup_.provider = resolve_model(agent.providers, agent.model).first;
            for (const auto& f : agent.instructions()) startup_.instructions.push_back(f.path.string());
            for (const auto& t : agent.tools()) startup_.tools.push_back(t.name);
            for (const auto& t : agent.script_tools()) startup_.tools.push_back(t.name);
            startup_.tool_notices = agent.tool_notices();
            for (const auto& f : context) {
                try {
                    startup_.context.emplace_back(Kind::Notice, agent.add_context_file(f));
                } catch (const std::exception& e) {
                    startup_.context.emplace_back(Kind::Error, e.what());
                }
            }
        };
        session_ = engine_->open_local(client_, std::move(ls));
        follow_first();
        if (resume) {
            view_.append(Kind::Notice, "resumed session " + resume->stem().string() + " (" + std::to_string(entries) + " entries" +
                                           (fork_at ? ", forked at record " + std::to_string(*fork_at) : "") + ")" +
                                           (append ? ", continuing in the same file" : ", continuing in a new file that points at it"));
        }
        if (want_auto) command("mode auto");
        if (!asking()) command("trust imports --pending");
    }

    ~App() { shutdown(); }

    // The session in the daemon: created in this directory or resumed, over the protocol as maid.nvim does it.
    // False, with the reason shown, when the daemon is not there or refuses; the session then opens in this process.
    bool open_in_daemon(const TuiOptions& options) {
        daemon_ = DaemonClient::connect([this] { wake_pump(); });
        if (!daemon_) return false;
        hello();
        nlohmann::json reply;
        if (options.resume) {
            reply = call("maid.session.resume", {{"session", options.resume->string()}});
        } else {
            nlohmann::json m = {{"workspace", std::filesystem::current_path().string()}};
            if (options.mode) m["mode"] = *options.mode;
            reply = call("createConversation", {{"maid", m}});
        }
        if (reply.contains("error")) {
            view_.append(Kind::Error, "the daemon refused this session (" + reply["error"].value("message", "") + "); it runs in this process instead");
            daemon_->close();
            daemon_.reset();
            return false;
        }
        session_ = result(reply).value("id", "");
        follow_first();
        nlohmann::json about = result(call("maid.session.describe", {{"session", session_}}));
        for (const auto& f : about.value("instructions", nlohmann::json::array())) startup_.instructions.push_back(f);
        for (const auto& t : about.value("tools", nlohmann::json::array())) startup_.tools.push_back(t);
        for (const auto& n : about.value("tool_notices", nlohmann::json::array())) startup_.tool_notices.push_back(n);
        const nlohmann::json& e = index_[session_];
        startup_.model = e.value("model", "");
        startup_.remote = e.value("remote_model", false);
        try {
            startup_.provider = resolve_model(settings_.providers, startup_.model).first;
        } catch (const std::exception&) {
            // a model this directory's settings do not know: no service hint
        }
        view_.append(Kind::Notice, std::string(options.resume ? "resumed session " + session_ + " " : "") +
                                       "in the daemon (maid daemon status): the session outlives this window, and :q leaves it there");
        if (options.model) command("model " + *options.model);
        // Auto under a dumb harness is confirmed first; the daemon opened it one step safer, as this process would.
        bool asked_auto = (options.mode ? *options.mode : settings_.mode) == "auto";
        if (asked_auto && e.value("mode", "") == "edit" && e.value("harness", "") == "dumb") command("mode auto");
        if (!asking()) command("trust imports --pending");
        return true;
    }

    void welcome();
    void startup_notice(const std::string& t) { view_.append(Kind::Notice, t); }
    void attach_image(const std::filesystem::path& f) { command("image " + f.string()); }
    std::string transcript_path() const { return transcript_; }
    std::string exit_note() const { return exit_note_; }
    void send(const std::string& text) { submit(text, false); }
    void attach_context() {
        for (const auto& [kind, text] : startup_.context) view_.append(kind, text);
    }
    Element render();
    bool handle(Event e);

private:
    // ---------- the engine connection ----------
    void hello() { call("maid.hello", {{"protocol", 1}, {"client", {{"name", "maid"}, {"version", MAID_VERSION}}}, {"capabilities", {"tool_output"}}}); }
    // The session just opened: its snapshot, the index, then the events from here on.
    void follow_first() {
        nlohmann::json snap = result(call("maid.session.attach", {{"session", session_}}));
        call("maid.index.subscribe");
        for (const auto& e : result(call("maid.index.get")).value("entries", nlohmann::json::array())) index_[e.value("id", "")] = e;
        take_events();
        follow_entry(snap.value("entry", nlohmann::json::object()));
        usage_ = snap.value("usage", nlohmann::json::object());
        for (const auto& t : snap.value("todo", nlohmann::json::array())) todo_.push_back({t.value("text", ""), t.value("done", false)});
        pump_ = std::thread([this] { pump(); });
    }
    nlohmann::json call(const std::string& method, nlohmann::json params = nlohmann::json::object(), bool from_ui = true);
    static nlohmann::json result(const nlohmann::json& reply) { return reply.value("result", nlohmann::json::object()); }
    void command(const std::string& line);              // a `:` command the engine owns, its answer shown
    void show(const nlohmann::json& reply);              // a maid.session.command answer: lines, a question, text to send
    void wake_pump();
    void pump();
    void take_events();  // what the engine has queued, into the inbox, in order
    void drain();        // the inbox applied, on the UI thread
    void apply(const nlohmann::json& message);
    void on_event(const nlohmann::json& e);
    void follow_entry(const nlohmann::json& e);
    void follow_workspace(const std::filesystem::path& to);
    void live(const std::string& key, int stream, const std::string& data, size_t offset);
    void after_turn();
    // Sessions (`:h sessions`): the view follows the one in this client's focus.
    void show_session(const std::string& id);
    void session_verb(const std::string& cmd, const std::string& arg);
    enum class Go { New, Fork, To };
    void go(Go how, const std::string& target, std::string as, const std::string& dir, bool sure = false);
    void end_other(const std::string& id, const std::string& verb, bool sure = false);
    void open_switcher(const std::string& title, const std::string& leave);
    std::string resolve_session(const std::string& given) const;
    std::string session_line(const nlohmann::json& e) const;
    Element render_switcher();
    bool handle_switcher(const Event& e);
    std::map<std::string, nlohmann::json> index_;  // the engine's sessions, from maid.index
    std::optional<Switcher> switcher_;
    bool quit_warned_ = false;  // :q was told other sessions are working
    void open_recording();
    void record(const char* dir, const nlohmann::json& msg);
    std::function<Settings(const std::filesystem::path&)> settings_at_;
    std::optional<Settings> cd_settings_;  // what :cd read in the new directory, until its maid.session.settings arrives
    std::unique_ptr<Engine> engine_;            // this process's own, when the daemon does not hold the sessions
    std::unique_ptr<DaemonClient> daemon_;      // the daemon's, when it does
    bool daemon_gone_said_ = false;
    std::string client_, session_;
    std::atomic<long> next_id_{1};
    std::mutex inbox_mu_;  // the inbox, and the order calls and events are recorded in
    std::vector<nlohmann::json> inbox_;
    std::mutex pump_mu_;
    std::condition_variable pump_cv_;
    bool pump_wake_ = false, pump_stop_ = false;
    std::thread pump_;
    std::atomic<bool> drain_posted_{false};
    std::unique_ptr<protocol::Recorder> recorder_;  // MAID_PROTOCOL_RECORD: this connection's exchange
    Startup startup_;

    // The session as the engine reports it.
    std::string ws_, transcript_, model_, mode_ = "manual";
    bool remote_ = false, think_ = false, dumb_ = false, confined_ = false;
    std::string tier_ = "guarded";  // the session's protocol tier: open shows in the status strip, guarded only in :status
    size_t queued_ = 0;
    nlohmann::json usage_ = nlohmann::json::object();
    std::string response_;   // the running response (a paused turn's last)
    bool paused_ = false;    // an interrupt (Ctrl-S) paused the turn
    bool pause_menu_ = false;  // its menu is up: Ctrl-Q, s, d, f, k, h; Esc leaves it to type a message
    long turn_seq_ = -1;     // the stream's position before the turn this client started: an idle at or before it is an earlier turn's
    std::chrono::steady_clock::time_point response_t0_;
    bool last_cancelled_ = false;
    std::map<std::string, nlohmann::json> approvals_seen_;  // for MaidApproval's verdict

    void post(Kind k, std::string text) {
        view_.append(k, std::move(text));
        screen_.PostEvent(Event::Custom);
    }
    bool asking() const { return approval_.has_value() || question_.has_value() || confirm_.has_value(); }
    void request_mode(Mode m) { command("mode " + std::string(mode_name(m))); }
    std::string todo_text() const {
        if (todo_.empty()) return "";
        size_t done = 0;
        std::string out;
        for (const auto& t : todo_) {
            done += t.done;
            out += std::string("\n") + (t.done ? "[x] " : "[ ] ") + t.text;
        }
        return "todo: " + std::to_string(done) + "/" + std::to_string(todo_.size()) + " done" + out;
    }

    Element render_input(size_t width, int& rows);
    std::vector<StyledLine> input_lines(const std::string& text);  // the built-in markdown highlighter, or nvim's
    std::unique_ptr<NvimHighlighter> nvim_hl_;  // highlight = "nvim": started on first use, reaped on exit
    bool nvim_hl_failed_ = false;
    Element render_palette(int& rows);
    std::vector<std::string> palette_entries();  // what the palette lists for the current command line
    std::vector<std::string> installed_models();  // the local providers' models, cached for a while (a localhost call)
    std::vector<std::string> models_cache_;
    std::chrono::steady_clock::time_point models_cached_at_{};
    void theme_command(const std::string& arg);  // :theme, :theme NAME, :theme reload, :theme nvim:NAME
    // The host nvim (maid.nvim, docs/nvim.md): connected before the settings were read, or refused with a reason.
    std::shared_ptr<HostNvim> host_;
    std::string host_refused_;
    std::atomic<bool> follow_theme_{false};  // the theme follows the host's colorscheme
    bool host_up() const { return host_ && host_->connected(); }
    void start_host();
    void follow_host_theme(bool announce);  // on the host's handler thread: read its colorscheme, apply it here
    void fire(const std::string& event, nlohmann::json data);  // a User autocmd in the host, with the session id
    void nvim_command(const std::string& arg);  // :nvim, :nvim theme
    void interrupt_from_host();                 // maid.nvim's :MaidInterrupt: the first Ctrl-C, or a notice when idle
    void open_file(const std::filesystem::path& path);  // in the host, or in $EDITOR in MAID's place
    void paste_input(const std::string& text);  // appended to the input, never sent
    bool pasting_ = false;  // inside a bracketed paste (maid.nvim's fallback when MAID is not connected)
    std::string paste_buf_;
    void use_theme(const Theme& theme);
    std::vector<std::string> nvim_colors();  // nvim's colorschemes for :theme nvim:<Tab>, asked once in the background
    std::mutex nvim_colors_mu_;
    std::vector<std::string> nvim_colors_;
    bool nvim_colors_asked_ = false;
    std::thread nvim_colors_thread_;
    std::atomic<bool> theme_busy_{false};
    std::thread theme_thread_;  // a :theme nvim:NAME import
    void complete_command();
    Element render_top_status();
    Element render_bottom_status();
    Element render_approval();
    Element render_question();

    bool handle_approval(const Event& e);
    bool handle_question(const Event& e);
    void act(const Editor::Result& r);
    void answer(Approval a, std::string feedback = "");
    void answer_question(std::string text);
    void cancel_turn();
    void steer(const std::string& action);  // maid.steer on the running response; the input, if any, is its note
    void submit(std::string text, bool now);
    void start_turn(const std::string& text);
    std::string take_dropped_image(const std::string& text);
    bool attach(const std::filesystem::path& file);  // :image FILE, quietly: true when it is attached
    void run_command(const std::string& line);
    void run_shell(const std::string& command);
    void run_lua(const std::string& code, bool from_file);
    void set_focus(Focus f);
    void edit_externally();
    void quit(const std::string& as = "", bool sure = false);
    std::string quit_as_;  // :q's --bg, --park or --stop, for maid.session.leave
    void shutdown();

    ScreenInteractive& screen_;
    Settings settings_;
    std::filesystem::path previous_ws_;  // the workspace before the last :cd
    std::string log_path() const { return transcript_ + (settings_.record ? "" : "  (temporary: --no-record)"); }
    std::string register_;
    Editor editor_;
    View view_;
    Focus focus_ = Focus::Input;

    std::optional<PendingApproval> approval_;
    std::optional<PendingQuestion> question_;
    std::optional<PendingConfirm> confirm_;
    Element render_confirm();
    bool handle_confirm(const Event& e);
    Element render_pause_menu();
    bool handle_pause_menu(const Event& e);
    std::vector<TodoItem> todo_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> shell_busy_{false};
    std::thread shell_thread_;
    bool stopped_ = false;

    std::string status_msg_;
    int tool_calls_ = 0;       // this response
    std::string live_call_;    // the call the live entry shows, and the next offset per stream
    size_t live_next_[2] = {0, 0};
    std::atomic<bool> redraw_posted_{false};
    std::atomic<bool> quit_when_idle_{false};  // :wq
    std::string exit_note_;
    bool lua_mode_ = false;     // :lua with no argument: sends go to Lua until :chat (or :lua again)
    bool ctrl_w_pending_ = false;
    bool ctrl_x_pending_ = false;
    size_t palette_sel_ = 0;
    std::string palette_for_;  // the command line the selection belongs to
    bool quit_armed_ = false;
    int view_height_ = 10;
    std::unique_ptr<LazyLockWatch> lazy_lock_;  // nvim's lazy-lock.json; null when lazy_lock_notice is off
    void maybe_check_keymaps(const LazyLockState& lock);  // the keymap check, once per lazy-lock.json content
    std::thread keymap_thread_;
    std::atomic<bool> keymap_cancel_{false}, keymap_running_{false};
    std::string keymap_checked_lock_;  // the lock file hash maybe_check_keymaps last looked at (UI thread)
};

void App::welcome() {
    std::string remote = startup_.remote ? "  ·  REMOTE" : "  ·  local";
    view_.append(Kind::Notice, "MAID  ·  workspace " + ws_ + "  ·  model " + startup_.model + remote);
    std::string files;
    for (const auto& f : startup_.instructions) files += (files.empty() ? "" : ", ") + f;
    if (!settings_.theme_error.empty()) view_.append(Kind::Error, settings_.theme_error + "; the default theme is in use (:theme reload after fixing it)");
    if (session_tripped()) view_.append(Kind::Error, "this session is tripped (its own lock, from an earlier run): :unlock removes it");
    if (settings_.lazy_lock_notice && !settings_.bare) {
        lazy_lock_ = std::make_unique<LazyLockWatch>(lazy_lock_path(settings_.lazy_lock));
        if (std::string n = lazy_lock_notice(lazy_lock_->check()); !n.empty()) view_.append(Kind::Notice, n);
    }
    if (!settings_.bare) maybe_check_keymaps(lazy_lock_ ? lazy_lock_->check() : lazy_lock_state(lazy_lock_path(settings_.lazy_lock)));
    view_.append(Kind::Notice, "session transcript: " + log_path() + (files.empty() ? "" : "\ninstructions: " + files));
    if (!startup_.tools.empty()) {
        std::string names;
        for (const auto& t : startup_.tools) names += (names.empty() ? "" : ", ") + t;
        view_.append(Kind::Notice, "tools: " + names + "  (:tools lists them)");
    }
    for (const auto& n : startup_.tool_notices) view_.append(Kind::Error, n);
    start_host();
    view_.append(Kind::Notice, std::string("Press i to type, ") + (settings_.enter_sends ? "Enter to send a one-line input (Shift+Enter or Alt+Enter for a new line, :w sends any)" : "Alt+Enter (or :w) to send, Enter for a new line") +
                                   ". Esc = normal mode: j/k scroll, u/Ctrl-R undo/redo, :e opens nvim, Ctrl-W k = conversation window, :help for everything.");
    if (startup_.remote) view_.append(Kind::Error, "This model runs off this machine: prompts, files the agent reads and command output are sent to it.");
    // The service behind the current model, if any: say so when it is down. Never a service the model does not use.
    try {
        const Provider& provider = startup_.provider;
        if (!provider.remote()) {
            std::vector<std::string> problems;
            std::string hint = unreachable_hint(provider, load_services(root_dir() / "services", &problems));
            for (const auto& p : problems) view_.append(Kind::Error, p);
            if (hint.find("is not running") != std::string::npos) view_.append(Kind::Error, hint.substr(0, hint.find(':')) + ": :up " + hint.substr(0, hint.find(' ')));
        }
    } catch (const std::exception& e) {
        view_.append(Kind::Error, e.what());
    }
}

// ---------- the engine connection ----------

// Calls from the UI thread hold the inbox while they run, so a call and the events it causes are recorded in that
// order; a call from another thread (a `!cmd`, which runs until the command ends) is recorded when it returns.
nlohmann::json App::call(const std::string& method, nlohmann::json params, bool from_ui) {
    nlohmann::json msg = {{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}, {"params", std::move(params)}};
    std::unique_lock lock(inbox_mu_, std::defer_lock);
    if (from_ui) lock.lock();
    nlohmann::json reply = daemon_ ? daemon_->call(msg) : engine_->call(client_, msg);
    if (!from_ui) lock.lock();
    record("in", msg);
    record("out", reply);
    return reply;
}

void App::command(const std::string& line) {
    nlohmann::json reply = call("maid.session.command", {{"session", session_}, {"line", line}});
    drain();
    show(reply);
}

void App::show(const nlohmann::json& reply) {
    if (reply.contains("error")) {
        post(Kind::Error, reply["error"].value("message", "the engine refused it"));
        return;
    }
    const nlohmann::json& r = reply["result"];
    for (const auto& l : r.value("lines", nlohmann::json::array())) post(l.value("level", "info") == "info" ? Kind::Notice : Kind::Error, l.value("text", ""));
    if (r.contains("ask")) {
        const auto& a = r["ask"];
        confirm_ = PendingConfirm{a.value("id", ""), a.value("title", ""), a.value("lines", std::vector<std::string>{}), a.value("keys", "yn")};
        screen_.PostEvent(Event::Custom);
    }
    if (r.contains("send")) submit(r["send"], false);
}

// The engine calls this from whichever thread sent; it must not call back into the engine.
void App::wake_pump() {
    {
        std::lock_guard lock(pump_mu_);
        pump_wake_ = true;
    }
    pump_cv_.notify_one();
}

// Moves what the engine queues for this client into the inbox as it comes, so the engine's queue never backs up
// behind a busy screen, and asks the UI thread to apply it.
void App::pump() {
    for (;;) {
        {
            std::unique_lock lock(pump_mu_);
            pump_cv_.wait(lock, [this] { return pump_wake_ || pump_stop_; });
            if (pump_stop_) return;
            pump_wake_ = false;
        }
        take_events();
        if (!drain_posted_.exchange(true)) {
            screen_.Post([this] { drain(); });
            screen_.PostEvent(Event::Custom);
        }
    }
}

void App::take_events() {
    std::lock_guard lock(inbox_mu_);
    for (auto& m : daemon_ ? daemon_->take() : engine_->take(client_, std::chrono::milliseconds(0))) {
        record("out", m);
        inbox_.push_back(std::move(m));
    }
}

void App::drain() {
    drain_posted_ = false;
    take_events();
    if (daemon_ && daemon_->gone() && !daemon_gone_said_) {
        daemon_gone_said_ = true;
        post(Kind::Error, "the daemon has stopped: its sessions are parked. Quit, and maid -c (or maid -r) resumes this one");
    }
    std::vector<nlohmann::json> batch;
    {
        std::lock_guard lock(inbox_mu_);
        batch.swap(inbox_);
    }
    for (const auto& m : batch) apply(m);
}

void App::apply(const nlohmann::json& m) {
    std::string method = m.value("method", "");
    const nlohmann::json& p = m.contains("params") ? m["params"] : nlohmann::json::object();
    if (method == "maid.event") {
        if (p.value("stream_id", "") == session_) on_event(p);  // else a session this view left, its last events before the switch
    } else if (method == "maid.index") {
        if (p.contains("removed")) index_.erase(p["removed"].get<std::string>());
        if (!p.contains("entry")) return;
        std::string id = p["entry"].value("id", "");
        index_[id] = p["entry"];
        if (id == session_) follow_entry(p["entry"]);
        screen_.PostEvent(Event::Custom);
    } else if (method == "maid.engine" && p.contains("notice")) {
        post(p.value("level", "warn") == "info" ? Kind::Notice : Kind::Error, p["notice"]);
    }
}

void App::follow_entry(const nlohmann::json& e) {
    if (e.contains("workspace") && ws_.empty()) ws_ = e["workspace"];
    transcript_ = e.value("transcript", transcript_);
    model_ = e.value("model", model_);
    mode_ = e.value("mode", mode_);
    remote_ = e.value("remote_model", remote_);
    think_ = e.value("think", think_);
    if (e.contains("harness")) dumb_ = e["harness"] == "dumb";
    tier_ = e.value("tier", tier_);
    queued_ = e.value("queued", queued_);
    screen_.PostEvent(Event::Custom);
}

// :cd moved the session: the process follows it, and the view takes the new directory's settings as a start there
// would, but the session's safety stays its own.
void App::follow_workspace(const std::filesystem::path& to) {
    previous_ws_ = ws_;
    ws_ = to.string();
    std::error_code ec;
    std::filesystem::current_path(to, ec);
    Settings next = cd_settings_ ? std::move(*cd_settings_) : settings_at_(to);
    cd_settings_.reset();
    next.tripwire = settings_.tripwire;
    next.allow_isolated = settings_.allow_isolated;
    next.forbid = settings_.forbid;
    next.record = settings_.record;
    next.harness = settings_.harness;
    next.dumb_auto_ok = settings_.dumb_auto_ok;
    next.bare = settings_.bare;
    auto changed = [&](const char* k) { return settings_.layered.value(k, nlohmann::json()) != next.layered.value(k, nlohmann::json()); };
    if (changed("markdown")) view_.set_markdown(next.markdown);
    if (changed("leader")) editor_.set_leader(next.leader), view_.set_leader(next.leader);
    if (changed("enter_sends")) editor_.set_enter_sends(next.enter_sends);
    if (changed("timestamps")) view_.set_timestamps(next.timestamps);
    settings_ = std::move(next);
}

// A running command's output: the live entry under its call keeps the last lines; a gap in a stream's offsets is
// output the screen never got.
void App::live(const std::string& key, int stream, const std::string& data, size_t offset) {
    if (key != live_call_) {
        live_call_ = key;
        live_next_[0] = live_next_[1] = 0;
    }
    size_t& next = live_next_[stream];
    std::string text;
    if (offset > next) text = "\n[" + std::to_string(offset - next) + " bytes not shown: the screen fell behind]\n";
    next = offset + data.size();
    text += data;
    view_.live_output(text);
    if (!redraw_posted_.exchange(true)) screen_.PostEvent(Event::Custom);
}

void App::on_event(const nlohmann::json& e) {
    const std::string type = e.value("type", "");
    long seq = e.value("sequence_number", -1L);
    if (type == "maid.session.state") {
        bool was = busy_;
        if (e.value("activity", "idle") != "idle") busy_ = true;
        else if (seq > turn_seq_) busy_ = false;
        if (was && !busy_) screen_.Post([this] { after_turn(); });
    } else if (type == "maid.input.added") {
        std::string text;
        for (const auto& part : e["item"].value("content", nlohmann::json::array())) text += part.value("text", "");
        size_t pics = e["item"].contains("maid") ? e["item"]["maid"].value("images", nlohmann::json::array()).size() : 0;
        view_.append(Kind::User, text + (pics == 0 ? "" : "\n(with " + std::to_string(pics) + " image" + (pics == 1 ? "" : "s") + ")"));
    } else if (type == "response.created") {
        response_ = e["response"].value("id", "");
        paused_ = pause_menu_ = false;
        // A successor (a steer's, or a paused turn resumed) continues the turn: its footer comes at the turn's end.
        if (!e["response"]["previous_response_id"].is_string()) {
            response_t0_ = std::chrono::steady_clock::now();
            tool_calls_ = 0;
            fire("MaidTurnStart", {{"model", e["response"].value("model", model_)}});
        }
    } else if (type == "maid.steer.applied") {
        std::string action = e.value("action", ""), note = e.value("note", "");
        std::string line = "↯ " + action + (e.value("trigger", "") == "ban" ? " (a ban's steer)" : "") + (note.empty() ? "" : ": " + note);
        if (e.contains("waits_for")) line += "  (at the next step)";
        if (!e["withdrawn"].empty()) line += "  (withdrew " + std::to_string(e["withdrawn"].size()) + " waiting)";
        view_.append(Kind::Notice, line);
    } else if (type == "maid.turn.paused") {
        paused_ = pause_menu_ = true;
    } else if (type == "error") {
        view_.append(Kind::Notice, "halted: " + e.value("message", ""));
    } else if (type == "response.output_text.done") {
        if (e.contains("maid") && e["maid"].contains("trimmed")) view_.replace_last(Kind::Assistant, e.value("text", ""));
    } else if (type == "response.output_text.delta" || type == "response.reasoning_text.delta") {
        view_.append_to_last(type == "response.output_text.delta" ? Kind::Assistant : Kind::Thinking, e.value("delta", ""));
    } else if (type == "response.output_item.added") {
        const nlohmann::json& item = e["item"];
        std::string kind = item.value("type", "");
        if (kind != "function_call" && kind != "shell_call") return;
        ++tool_calls_;
        std::string summary = item.contains("maid") ? item["maid"].value("summary", "") : "";
        view_.append(Kind::Tool, summary);
        if (host_up()) {
            std::string tool = "run_shell", path;
            if (kind == "function_call") {
                std::string name = item.value("name", "");
                tool = canonical_tool_name(name).empty() ? name : canonical_tool_name(name);
                auto args = nlohmann::json::parse(item.value("arguments", "{}"), nullptr, false);
                for (const char* key : {"path", "from"}) {
                    if (path.empty() && args.is_object() && args.contains(key) && args[key].is_string()) path = args[key];
                }
            }
            try {
                if (!path.empty()) path = resolve_path(ws_, path).string();
            } catch (const std::exception&) {
            }
            fire("MaidToolCall", {{"tool", tool}, {"path", path}, {"summary", summary}});
        }
    } else if (type == "response.shell_call_output_content.delta") {
        const nlohmann::json& d = e["delta"];
        std::string out = d.value("stdout", ""), err = d.value("stderr", "");
        size_t offset = e.contains("maid") ? e["maid"].value("offset", size_t(0)) : 0;
        if (!out.empty()) live(e.value("item_id", ""), 0, out, offset);
        else if (!err.empty()) live(e.value("item_id", ""), 1, err, offset);
    } else if (type == "maid.tool.output.delta") {
        if (!e.contains("data")) return;  // a skip: the next chunk's offset shows the gap
        if (!e.contains("output_index")) {
            // A `!cmd` of this session's: it streams under the command's line.
            view_.append_to_last(Kind::ToolOk, e["data"].get<std::string>());
            screen_.PostEvent(Event::Custom);
            return;
        }
        live(e.value("call", e.value("item_id", "")), 0, e["data"], e.value("offset", size_t(0)));
    } else if (type == "response.output_item.done") {
        const nlohmann::json& item = e["item"];
        std::string kind = item.value("type", "");
        if (kind == "message" && item.contains("maid") && item["maid"].value("status", "") == "discarded") {
            view_.append(Kind::Notice, "(the reply above was discarded: the model never sees it)");
            return;
        }
        if ((kind != "function_call_output" && kind != "shell_call_output") || !item.contains("maid")) return;  // a call that never ran keeps its live lines
        std::string text = kind == "function_call_output" ? item.value("output", "")
                                                          : item["output"].empty() ? "" : item["output"][0].value("stdout", "");
        std::string full;
        if (item["maid"].contains("full_output")) {
            const auto& f = item["maid"]["full_output"];
            // Opening the fold shows the whole output as it looked when it ended, at most its last MiB.
            full = std::string("[") + kFullOutputLabel + ": maid sessions output " + f.value("session", "") + " " + f.value("call", "") + "]\n" +
                   full_output_screen(full_output_path(transcript_, f.value("call", "")), 1 << 20);
        }
        view_.finish_live(item["maid"].value("ok", false) ? Kind::ToolOk : Kind::ToolErr, text, std::move(full));
    } else if (type == "maid.notice") {
        std::string kind = e.value("kind", ""), text = e.value("text", "");
        if (kind == "tool_call") {
            // A subagent's call, or a line about one ("↳ explore on ...").
            ++tool_calls_;
            view_.append(Kind::Tool, text);
            if (e.contains("tool") && host_up()) {
                std::string path = e.value("path", "");
                try {
                    if (!path.empty()) path = resolve_path(ws_, path).string();
                } catch (const std::exception&) {
                }
                fire("MaidToolCall", {{"tool", e["tool"]}, {"path", path}, {"summary", text.rfind("↳ ", 0) == 0 ? text.substr(std::strlen("↳ ")) : text}});
            }
        } else if (kind == "tool_result") {
            std::string full;
            if (e.contains("full_output")) {
                const auto& f = e["full_output"];
                full = std::string("[") + kFullOutputLabel + ": maid sessions output " + f.value("session", "") + " " + f.value("call", "") + "]\n" +
                       full_output_screen(full_output_path(transcript_, f.value("call", "")), 1 << 20);
            }
            view_.finish_live(e.value("ok", false) ? Kind::ToolOk : Kind::ToolErr, text, std::move(full));
        } else {
            view_.append(Kind::Notice, text);
        }
    } else if (type == "maid.task.created") {
        view_.append(Kind::Notice, "⧉ the " + e.value("agent", "") + " agent works in the background: " + e.value("prompt_head", "") + "  (:switch shows it)");
    } else if (type == "maid.task.completed" || type == "maid.task.failed") {
        bool ok = type == "maid.task.completed";
        std::string steps = std::to_string(e.value("steps", 0)), tokens = std::to_string(e.value("tokens", 0L));
        view_.append(ok ? Kind::Notice : Kind::Error, "⧉ the " + e.value("agent", "") + " task " + (ok ? "finished" : e.value("reason", "failed")) + " (" + steps + " steps, " + tokens +
                                                          " tokens); its answer goes to the agent  (:switch " + e.value("task", "") + " reads it)");
    } else if (type == "maid.approval.requested") {
        approval_ = PendingApproval{e.value("id", ""), e};
        nlohmann::json data = {{"tool", e.value("tool", "")}, {"path", e.value("path", "")}, {"summary", e.value("summary", "")}, {"reason", e.value("reason", "")}, {"verdict", "pending"}};
        approvals_seen_[e.value("id", "")] = data;
        fire("MaidApproval", data);
    } else if (type == "maid.approval.answered") {
        std::string id = e.value("id", "");
        if (approval_ && approval_->id == id) approval_.reset();
        if (auto it = approvals_seen_.find(id); it != approvals_seen_.end()) {
            it->second["verdict"] = e.value("choice", "no");
            fire("MaidApproval", it->second);
            approvals_seen_.erase(it);
        }
    } else if (type == "maid.question.asked") {
        question_ = PendingQuestion{e.value("id", ""), e.value("text", ""), e.value("options", std::vector<std::string>{}), ""};
    } else if (type == "maid.question.answered") {
        if (question_ && question_->id == e.value("id", "")) question_.reset();
    } else if (type == "maid.todo.updated") {
        todo_.clear();
        for (const auto& t : e.value("items", nlohmann::json::array())) todo_.push_back({t.value("text", ""), t.value("done", false)});
    } else if (type == "maid.file.written") {
        if (!host_up()) return;
        host_checktime(*host_);
        fire("MaidFileWritten", {{"tool", e.value("tool", "")}, {"path", e.value("path", "")}});
    } else if (type == "maid.usage.updated") {
        usage_ = e;
    } else if (type == "maid.session.settings") {
        if (e.contains("mode")) mode_ = e["mode"];
        if (e.contains("model")) model_ = e["model"];
        if (e.contains("remote_model")) remote_ = e["remote_model"];
        if (e.contains("think")) think_ = e["think"];
        if (e.contains("harness")) dumb_ = e["harness"] == "dumb";
        if (e.contains("tier")) tier_ = e["tier"];
        if (e.contains("workspace")) follow_workspace(e["workspace"].get<std::string>());
    } else if (type == "maid.session.title") {
        std::string text = e.value("text", "");
        view_.append(Kind::Notice, e.value("source", "") == "auto" ? "titled: " + text + "  (:rename changes it)" : "titled: " + text);
    } else if (type == "response.completed" || type == "response.failed" || type == "maid.response.cancelled" || type == "response.incomplete") {
        const nlohmann::json& r = e["response"];
        if (!r["maid"].value("final", true)) {
            if (type == "maid.response.cancelled") view_.append(Kind::Notice, "paused · Ctrl-Q resumes · s steer · d drop · f further · k keep · h halt · Esc types a message");
            screen_.PostEvent(Event::Custom);
            return;  // the turn goes on
        }
        if (type == "response.failed") view_.append(Kind::Error, r.contains("error") && r["error"].is_object() ? r["error"].value("message", "") : "");
        last_cancelled_ = type == "maid.response.cancelled";
        // The footer: model, how long the response took, how many tools ran.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - response_t0_).count();
        char dur[32];
        if (secs < 1) snprintf(dur, sizeof(dur), "%.0fms", secs * 1000);
        else if (secs < 60) snprintf(dur, sizeof(dur), "%.1fs", secs);
        else if (secs < 3600) snprintf(dur, sizeof(dur), "%dm %ds", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
        else snprintf(dur, sizeof(dur), "%dh %dm", static_cast<int>(secs) / 3600, (static_cast<int>(secs) % 3600) / 60);
        std::string model = r.value("model", model_);
        view_.append(Kind::Notice, "▣ " + model + " · " + dur + (tool_calls_ ? " · " + std::to_string(tool_calls_) + (tool_calls_ == 1 ? " tool call" : " tool calls") : "") +
                                       (last_cancelled_ ? " · interrupted" : ""));
        fire("MaidTurnEnd", {{"model", model}, {"tool_calls", tool_calls_}, {"seconds", secs}, {"interrupted", last_cancelled_}});
    }
    screen_.PostEvent(Event::Custom);
}

// The session went idle after a turn: the checks that wait for one, and :wq's quit.
void App::after_turn() {
    if (lazy_lock_) maybe_check_keymaps(lazy_lock_->check());
    if (!asking()) command("trust imports --pending");
    if (quit_when_idle_.load() && !last_cancelled_) {
        quit_when_idle_ = false;
        quit();
    }
}

// MAID_PROTOCOL_RECORD=DIR keeps this connection's exchange as DIR/tui-<pid>.jsonl for `maid protocol check`, checked
// as it goes: a violation is shown once. The test suite runs every case this way.
void App::open_recording() {
    const char* dir = std::getenv("MAID_PROTOCOL_RECORD");
    if (dir && *dir) recorder_ = std::make_unique<protocol::Recorder>(std::filesystem::path(dir) / ("tui-" + std::to_string(getpid()) + ".jsonl"));
}

// Under inbox_mu_.
void App::record(const char* dir, const nlohmann::json& msg) {
    if (!recorder_) return;
    if (auto v = recorder_->add(dir, client_, msg)) view_.append(Kind::Error, "protocol: " + protocol::describe(*v));
}

// When lazy-lock.json is not the one the keymap check last ran at (a plugin update, or no check yet), the check runs
// once in the background and the new collisions, if any, become a notice (docs/nvim.md). A machine without nvim or
// without a lock file stays quiet.
void App::maybe_check_keymaps(const LazyLockState& lock) {
    if (lock.hash.empty() || lock.hash == keymap_checked_lock_ || keymap_running_) return;
    keymap_checked_lock_ = lock.hash;
    KeymapRecord before = load_keymap_record();
    if (before.exists && before.lock_hash == lock.hash) return;
    if (keymap_thread_.joinable()) keymap_thread_.join();
    keymap_running_ = true;
    keymap_thread_ = std::thread([this, before, hash = lock.hash] {
        KeymapReport r = run_keymap_check("", &keymap_cancel_);
        keymap_running_ = false;
        if (r.error == "stopped" || r.error.rfind("can't run ", 0) == 0) return;
        save_keymap_record(r, hash);
        if (!r.error.empty()) {
            post(Kind::Error, "nvim keymaps: the check after the lazy-lock.json change could not run: " + r.error + " (maid nvim keymaps)");
            return;
        }
        auto fresh = new_collisions(before, r);
        if (fresh.empty()) return;
        std::string text = before.exists ? "a plugin update added keymaps that collide: maid nvim keymaps"
                                         : "nvim keymaps, checked for the first time: " + std::to_string(fresh.size()) + " collide: maid nvim keymaps";
        for (const auto& c : fresh) text += "\n  " + c.text;
        post(Kind::Notice, text);
    });
}

// ---------- rendering ----------

Element App::render_input(size_t width, int& rows) {
    bool insert = editor_.mode() == Editor::Mode::Insert || editor_.mode() == Editor::Mode::Replace;
    std::string pre = lua_mode_ ? (insert ? "lua❯" : "lua│") : insert ? "❯ " : "│ ";
    size_t avail = width > 3 ? width - 2 : 1;
    auto lines = editor_.text().empty() ? std::vector<StyledLine>{{}} : input_lines(editor_.text());
    auto chunks = chunk_rows(lines, avail);
    rows = static_cast<int>(chunks.size());

    const Style& base = settings_.style("input");
    const Style& visual_style = settings_.style("visual");
    static const Style cursor_block{std::nullopt, std::nullopt, false, false, false, false, true};
    auto [sel_a, sel_b] = editor_.selection();
    bool show_cursor = focus_ == Focus::Input && editor_.mode() != Editor::Mode::Command && !asking();
    bool visual = editor_.mode() == Editor::Mode::Visual || editor_.mode() == Editor::Mode::VisualLine;
    const std::string& text_ = editor_.text();

    Elements out;
    for (size_t r = 0; r < chunks.size(); ++r) {
        const auto& row = chunks[r];
        auto col_of = [&](size_t byte) { return utf8_len(text_.substr(row.begin, std::min(byte, text_.size()) - row.begin)); };
        std::vector<Overlay> overlays;
        if (visual && sel_b > row.begin && sel_a <= row.end) {
            size_t from = sel_a > row.begin ? col_of(sel_a) : 0;
            size_t to = sel_b < row.end ? col_of(sel_b) : line_width(row.spans);
            overlays.push_back({from, std::max(to, from + 1), &visual_style});
        }
        StyledLine spans = row.spans;
        size_t cursor = editor_.cursor();
        bool last_row_of_line = r + 1 == chunks.size() || chunks[r + 1].begin != row.end;
        bool cursor_here = cursor >= row.begin && (cursor < row.end || (cursor == row.end && last_row_of_line));
        if (cursor_here && show_cursor) {
            size_t c = col_of(cursor);
            if (c >= line_width(spans)) spans.push_back({" ", MdNone});
            overlays.push_back({c, c + 1, &cursor_block});
        }
        Element lead = text(r == 0 ? pre : "  ") | decorate(settings_.style(insert ? "input_prompt_insert" : "input_prompt_normal"));
        Element body = render_line(settings_, spans, base, overlays);
        out.push_back(hbox({lead, body}));
    }
    return vbox(out);
}

// nvim gets 50 ms per keystroke; when its reply is late the built-in highlighter paints this frame and the
// reply's arrival redraws. When nvim is missing or breaks, one notice says so and the built-in one stays.
std::vector<StyledLine> App::input_lines(const std::string& text) {
    if (settings_.highlight == "nvim" && !settings_.bare && !nvim_hl_failed_) {
        if (!nvim_hl_) nvim_hl_ = std::make_unique<NvimHighlighter>("nvim", [this] { screen_.PostEvent(Event::Custom); });
        auto lines = nvim_hl_->highlight(text, std::chrono::milliseconds(50));
        if (lines) return *lines;
        if (!nvim_hl_->alive()) {
            nvim_hl_failed_ = true;
            post(Kind::Notice, "highlight = nvim: " + nvim_hl_->error() + "; using the built-in highlighter");
            nvim_hl_.reset();
        }
    }
    return markdown_lines(text);
}

// The palette: commands (or arguments) matching what is typed after ':'.
std::vector<std::string> App::palette_entries() {
    std::vector<std::string> out;
    if (editor_.mode() != Editor::Mode::Command || editor_.cmd_prefix() != ':') return out;
    const std::string& line = editor_.cmdline();
    size_t space = line.find(' ');
    if (space == std::string::npos) {
        for (const auto* c : match_commands(line)) out.push_back(c->name);
        return out;
    }
    CompletionContext ctx;
    std::vector<std::string> problems;  // said by the commands that use the services, not on every completion
    for (const auto& s : load_services(root_dir() / "services", &problems)) ctx.services.push_back(s.name);
    for (const auto& p : settings_.providers) ctx.providers.push_back(p.name);
    ctx.models = installed_models();
    ctx.workspace = ws_;
    std::string cmd = line.substr(0, space), partial = line.substr(space + 1);
    auto matches = match_commands(cmd);
    if (!matches.empty() && matches.front()->name == "theme") {
        for (const auto& t : list_themes()) ctx.themes.push_back(t.name);
        if (partial.rfind("nvim:", 0) == 0 && !settings_.bare) ctx.nvim_colors = nvim_colors();
    }
    return complete_argument(matches.empty() ? cmd : matches.front()->name, partial, ctx);
}

std::vector<std::string> App::installed_models() {
    auto now = std::chrono::steady_clock::now();
    if (!models_cache_.empty() && now - models_cached_at_ < std::chrono::seconds(30)) return models_cache_;
    std::vector<std::string> out;
    for (size_t i = 0; i < settings_.providers.size(); ++i) {
        const auto& p = settings_.providers[i];
        if (p.remote()) continue;
        try {
            std::vector<std::string> names = p.kind == "openai" ? list_openai_models(p) : std::vector<std::string>{};
            // The first provider's models complete bare; the rest need their prefix.
            for (const auto& m : names) out.push_back(i == 0 ? m : p.name + "/" + m);
        } catch (const std::exception&) {
        }
    }
    models_cache_ = out;
    models_cached_at_ = now;
    return out;
}

std::vector<std::string> App::nvim_colors() {
    std::lock_guard lock(nvim_colors_mu_);
    if (!nvim_colors_asked_) {
        nvim_colors_asked_ = true;
        nvim_colors_thread_ = std::thread([this] {
            try {
                auto colors = nvim_colorschemes();
                std::lock_guard l(nvim_colors_mu_);
                nvim_colors_ = std::move(colors);
            } catch (const std::exception& e) {
                post(Kind::Error, std::string("listing nvim's colorschemes: ") + e.what());
            }
            screen_.PostEvent(Event::Custom);
        });
    }
    return nvim_colors_;
}

void App::theme_command(const std::string& arg) {
    if (arg.empty()) {
        std::string out = "themes (:theme NAME switches, :theme reload re-reads the file, :theme nvim:NAME imports a neovim colorscheme):";
        for (const auto& t : list_themes()) out += "\n" + std::string(t.name == settings_.theme ? "* " : "  ") + t.name + "  " + (t.path.empty() ? "built in" : t.path.string());
        post(Kind::Notice, out);
        return;
    }
    if (arg == "reload" && settings_.theme.rfind("nvim:", 0) == 0) {
        nvim_command("theme");  // a theme that came from the host is re-read from it
        return;
    }
    if (follow_theme_ && arg != "reload") {
        follow_theme_ = false;
        post(Kind::Notice, "no longer following nvim's colorscheme this session (:nvim theme follows it again)");
    }
    if (arg.rfind("nvim:", 0) == 0 && settings_.bare) {
        post(Kind::Error, ":theme nvim:NAME runs nvim, and this MAID is bare (--bare, MAID_BARE=1 or bare = true): it uses nothing from nvim. "
                          "Themes saved from nvim before are MAID's own files and still load: :theme nvim-NAME");
        return;
    }
    if (arg.rfind("nvim:", 0) == 0) {
        std::string scheme = arg.substr(5);
        if (scheme.empty()) {
            post(Kind::Error, ":theme nvim:NAME imports that neovim colorscheme; Tab after nvim: lists them");
            return;
        }
        if (theme_busy_) {
            post(Kind::Error, "an nvim import is still running");
            return;
        }
        if (theme_thread_.joinable()) theme_thread_.join();
        theme_busy_ = true;
        post(Kind::Notice, "importing " + scheme + " from nvim with your configuration (up to 15 s)");
        theme_thread_ = std::thread([this, scheme] {
            try {
                Theme t = import_nvim_theme(scheme);
                screen_.Post([this, t] { use_theme(t); });
            } catch (const std::exception& e) {
                post(Kind::Error, std::string(e.what()) + "; keeping " + settings_.theme);
            }
            theme_busy_ = false;
        });
        return;
    }
    try {
        use_theme(load_theme(arg == "reload" ? settings_.theme : arg));
    } catch (const std::exception& e) {
        post(Kind::Error, std::string(e.what()) + "; keeping " + settings_.theme);
    }
}

void App::start_host() {
    if (!host_refused_.empty()) view_.append(Kind::Error, "nvim: not connecting to $NVIM (" + host_refused_ + "); running without the host (:h nvim)");
    if (!host_) return;
    HostNvim::Handlers h;
    h.send = [this](const std::string& text) {
        screen_.Post([this, text] { paste_input(text); });
        screen_.PostEvent(Event::Custom);
    };
    h.command = [this](const std::string& line) {
        screen_.Post([this, line] { run_command(!line.empty() && line[0] == ':' ? line.substr(1) : line); });
        screen_.PostEvent(Event::Custom);
    };
    h.interrupt = [this] {
        screen_.Post([this] { interrupt_from_host(); });
        screen_.PostEvent(Event::Custom);
    };
    h.colorscheme = [this] { follow_host_theme(false); };
    h.error = [this](const std::string& why) { post(Kind::Error, "nvim: " + why); };
    h.closed = [this] { post(Kind::Notice, "nvim: the host is gone; :e and the theme are MAID's own again"); };
    host_->set_handlers(std::move(h));
    follow_theme_ = settings_.follow_nvim_theme;
    try {
        host_watch_colorscheme(*host_, host_->channel());
    } catch (const std::exception& e) {
        view_.append(Kind::Error, std::string("nvim: cannot watch its colorscheme: ") + e.what());
        follow_theme_ = false;
    }
    view_.append(Kind::Notice, "nvim: connected to the nvim MAID runs in (" + host_->socket() + "): :e FILE opens there, e and d at an approval show the file and the diff, :nvim says more");
    if (follow_theme_) host_->post([this] { follow_host_theme(true); });
}

// Runs on the host's handler thread (it waits for nvim); the theme is applied on the UI thread.
void App::follow_host_theme(bool announce) {
    if (!follow_theme_ || !host_up()) return;
    Theme t = host_theme(*host_);
    screen_.Post([this, t, announce] {
        if (!follow_theme_) return;
        apply_theme(settings_, t);
        if (announce) post(Kind::Notice, "theme: " + t.name + ", following nvim's colorscheme (follow_nvim_theme = false in settings keeps your own)");
        else status_msg_ = "theme: " + t.name;
    });
    screen_.PostEvent(Event::Custom);
}

void App::fire(const std::string& event, nlohmann::json data) {
    if (!host_up()) return;
    data["session"] = session_;
    host_fire(*host_, event, data);
}

void App::nvim_command(const std::string& arg) {
    if (arg == "theme") {
        if (!host_up()) {
            post(Kind::Error, "no host nvim to take the theme from (:nvim)");
            return;
        }
        follow_theme_ = true;
        host_->post([this] { follow_host_theme(true); });
        return;
    }
    if (!arg.empty()) {
        post(Kind::Error, ":nvim [theme]");
        return;
    }
    if (host_up()) {
        post(Kind::Notice, "nvim: connected to " + host_->socket() + " as channel " + std::to_string(host_->channel()) + " (client \"maid\")\n"
                           "  :e FILE and e at an approval open files there, d at a write's approval diffs it in a new tab\n"
                           "  :MaidInterrupt (<leader>mc) there stops a running turn as Ctrl-C does\n"
                           "  User autocmds MaidTurnStart, MaidToolCall, MaidApproval, MaidFileWritten, MaidTurnEnd fire there\n"
                           "  the model has the diagnostics tool; your Lua has maid.nvim\n"
                           "  theme: " + std::string(follow_theme_ ? "follows its colorscheme (" + settings_.theme + ")" : "your own (" + settings_.theme + "); :nvim theme follows nvim's"));
    } else if (settings_.bare) {
        post(Kind::Notice, "nvim: bare (--bare, MAID_BARE=1 or bare = true): MAID uses nothing from nvim. No host connection even inside nvim, the built-in "
                           "highlighter, no theme from nvim, no lazy-lock notice and no keymap check; your settings, themes, Lua and tools load as usual. :h bare");
    } else if (host_) {
        post(Kind::Notice, "nvim: the host this session connected to is gone");
    } else {
        const char* sock = std::getenv("NVIM");
        post(Kind::Notice, std::string("nvim: no host. ") + (sock && *sock ? "$NVIM was refused: " + host_refused_ : "$NVIM is not set: MAID is not running inside nvim") +
                               "\nmaid.nvim (:Maid in nvim) runs MAID in a terminal there; :h nvim");
    }
}

// maid_interrupt does what the first Ctrl-C does to a running turn, shell command or question, and nothing else: an
// idle MAID keeps its draft and says so instead of clearing it or arming the quit.
void App::interrupt_from_host() {
    if (asking() || shell_busy_ || busy_) {
        handle(Event::Special("\x03"));
        return;
    }
    post(Kind::Notice, "nvim: nothing to interrupt (MAID is idle)");
}

void App::open_file(const std::filesystem::path& path) {
    if (host_up()) {
        try {
            host_open(*host_, path);
            status_msg_ = "opened in nvim: " + path.string();
        } catch (const std::exception& e) {
            post(Kind::Error, std::string("nvim: ") + e.what());
        }
        return;
    }
    const char* editor = std::getenv("VISUAL");
    if (!editor || !*editor) editor = std::getenv("EDITOR");
    if (!editor || !*editor) editor = "nvim";
    std::string quoted = "'";
    for (char c : path.string()) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    std::string command = std::string(editor) + " " + quoted;
    int rc = 0;
    screen_.WithRestoredIO([&] { rc = std::system(command.c_str()); })();
    if (rc != 0) post(Kind::Error, std::string(editor) + " exited with status " + std::to_string(rc));
}

void App::paste_input(const std::string& text) {
    if (text.empty()) return;
    std::string cur = editor_.text();
    std::string sep = cur.empty() ? "" : cur.back() == '\n' ? "\n" : "\n\n";
    editor_.replace_text(cur + sep + text);
    set_focus(Focus::Input);
    size_t lines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    status_msg_ = "pasted " + std::to_string(lines) + (lines == 1 ? " line" : " lines") + " into the input; nothing is sent until you send it";
}

void App::use_theme(const Theme& theme) {
    apply_theme(settings_, theme);
    std::string note = "theme: " + theme.name + " (" + (theme.path.empty() ? "built in" : theme.path.string()) + ")";
    if (!settings_.style_overrides.empty()) note += "; the " + std::to_string(settings_.style_overrides.size()) + " role(s) under `style` in settings stay on top of it";
    post(Kind::Notice, note);
}

Element App::render_palette(int& rows) {
    rows = 0;
    auto entries = palette_entries();
    if (entries.empty()) return emptyElement();
    if (palette_for_ != editor_.cmdline()) {
        palette_for_ = editor_.cmdline();
        palette_sel_ = 0;
    }
    if (palette_sel_ >= entries.size()) palette_sel_ = 0;
    bool listing_commands = editor_.cmdline().find(' ') == std::string::npos;
    Elements lines;
    size_t shown = std::min<size_t>(entries.size(), 8);
    for (size_t i = 0; i < shown; ++i) {
        std::string label = entries[i], summary;
        if (listing_commands) {
            for (const auto* c : match_commands(entries[i])) {
                if (c->name == entries[i]) {
                    label = ":" + c->name + (c->args.empty() ? "" : " " + c->args);
                    summary = c->summary;
                    break;
                }
            }
        }
        Element row = hbox({text("  " + label) | size(WIDTH, GREATER_THAN, 24), text("  " + summary) | decorate(settings_.style("status_dim"))});
        if (i == palette_sel_) row = row | decorate(settings_.style("visual"));
        lines.push_back(row);
    }
    if (entries.size() > shown) lines.push_back(text("  … " + std::to_string(entries.size() - shown) + " more") | decorate(settings_.style("status_dim")));
    rows = static_cast<int>(lines.size());
    return vbox(lines);
}

// Tab in the command line: complete to the selected palette entry; Tab again cycles.
void App::complete_command() {
    auto entries = palette_entries();
    if (entries.empty()) return;
    std::string line = editor_.cmdline();
    size_t space = line.find(' ');
    std::string head = space == std::string::npos ? "" : line.substr(0, space + 1);
    std::string word = space == std::string::npos ? line : line.substr(space + 1);
    std::string pick = entries[palette_sel_ % entries.size()];
    if (word == pick) {
        palette_sel_ = (palette_sel_ + 1) % entries.size();
        pick = entries[palette_sel_];
    }
    std::string completed = head + pick;
    if (space == std::string::npos) {
        for (const auto* c : match_commands(pick)) {
            if (c->name == pick && !c->args.empty()) completed += " ";
        }
    }
    editor_.set_cmdline(completed);
    palette_for_ = completed;
    // Keep the highlight on what was just completed rather than jumping back to the first entry.
    auto after = palette_entries();
    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i] == pick) palette_sel_ = i;
    }
}

Element App::render_top_status() {
    std::string mode_s = mode_;
    Elements left = {
        text(" ⏵ " + mode_s) | decorate(settings_.style("mode_" + mode_s)),
        text(" (shift-tab to cycle)") | decorate(settings_.style("status_dim")),
    };
    Elements right;
    right.push_back(text(model_ + (think_ ? " +think" : "")));
    right.push_back(text(remote_ ? " REMOTE" : " local") | decorate(settings_.style(remote_ ? "remote" : "status_dim")));
    if (host_up()) right.push_back(text(" nvim") | decorate(settings_.style("status_dim")));
    if (settings_.bare) right.push_back(text(" bare") | decorate(settings_.style("status_dim")));
    right.push_back(text(" · ") | decorate(settings_.style("status_dim")));
    if (tripwire_state()) right.push_back(text("HARNESS TRIPPED") | decorate(settings_.style("harness_tripped")));
    else right.push_back(text("harness armed") | decorate(settings_.style("harness_armed")));
    if (!todo_.empty()) {
        size_t done = std::count_if(todo_.begin(), todo_.end(), [](const TodoItem& t) { return t.done; });
        right.push_back(text(" · todo " + std::to_string(done) + "/" + std::to_string(todo_.size()) + " done") | decorate(settings_.style("notice")));
    }
    if (queued_) right.push_back(text(" · " + std::to_string(queued_) + " queued (:w now)") | decorate(settings_.style("notice")));
    size_t others = 0, waiting = 0, finished = 0;
    for (const auto& [id, e] : index_) {
        if (id == session_ || e.value("state", "") == "parked") continue;
        ++others;
        waiting += e.value("activity", "") == "waiting";
        finished += e.value("unseen", false);
    }
    if (others > 0) {
        std::string t = " · " + std::to_string(others) + (others == 1 ? " other session" : " other sessions");
        if (waiting) t += ", " + std::to_string(waiting) + " waiting";
        if (finished) t += ", " + std::to_string(finished) + " finished";
        right.push_back(text(t + " (:switch)") | decorate(settings_.style(waiting ? "notice" : "status_dim")));
    }
    if (paused_) right.push_back(text(" · PAUSED: ctrl-q resumes") | decorate(settings_.style("notice")));
    else if (busy_) right.push_back(text(" · working… ctrl-c interrupts, ctrl-s pauses") | decorate(settings_.style("notice")));
    if (shell_busy_) right.push_back(text(" · shell running") | decorate(settings_.style("shell")));
    if (lua_mode_) right.push_back(text(" · LUA MODE (:chat returns)") | decorate(settings_.style("shell")));
    if (dumb_) right.push_back(text(" · DUMB HARNESS") | decorate(settings_.style("error")));
    if (confined_) right.push_back(text(" · ISOLATED") | decorate(settings_.style("notice")));
    if (tier_ == "open") right.push_back(text(" · PROTOCOL OPEN (unchecked)") | decorate(settings_.style("error")));
    else if (tier_ == "airtight") right.push_back(text(" · AIRTIGHT") | decorate(settings_.style("notice")));
    if (daemon_) right.push_back(text(" · daemon") | decorate(settings_.style("status_dim")));
    if (!previous_ws_.empty()) {
        std::string ws = ws_, home = std::getenv("HOME");
        if (ws.rfind(home + "/", 0) == 0) ws = "~" + ws.substr(home.size());
        right.push_back(text(" · in " + ws) | decorate(settings_.style("status_dim")));
    }
    if (lazy_lock_ && lazy_lock_->marker()) right.push_back(text(" · lock≠") | decorate(settings_.style("notice")));
    right.push_back(text(" "));
    return hbox({hbox(left), filler(), hbox(right)}) | decorate(settings_.style("status"));
}

Element App::render_bottom_status() {
    if (editor_.mode() == Editor::Mode::Command) {
        return hbox({text(" :" + editor_.cmdline()), text(" ") | inverted});
    }
    std::string vim;
    const char* style = "status_normal";
    if (focus_ == Focus::Conversation) {
        vim = view_.visual() ? " VISUAL " : " CONVERSATION ";
        if (view_.visual()) style = "status_visual";
    } else {
        switch (editor_.mode()) {
            case Editor::Mode::Insert: vim = " INSERT ", style = "status_insert"; break;
            case Editor::Mode::Replace: vim = " REPLACE ", style = "status_insert"; break;
            case Editor::Mode::Normal: vim = " NORMAL "; break;
            case Editor::Mode::Visual: vim = " VISUAL ", style = "status_visual"; break;
            case Editor::Mode::VisualLine: vim = " V-LINE ", style = "status_visual"; break;
            case Editor::Mode::Command: break;
        }
    }
    Elements parts = {text(vim) | decorate(settings_.style(style)), text(" ")};
    if (char q = editor_.recording()) parts.push_back(text("recording @" + std::string(1, q) + " ") | decorate(settings_.style("notice")));
    std::string hint = view_.status_hint();
    if (!hint.empty()) parts.push_back(text(hint + " ") | decorate(settings_.style("status_dim")));
    if (!status_msg_.empty()) parts.push_back(text(status_msg_) | decorate(settings_.style("notice")));
    parts.push_back(filler());
    if (usage_.value("calls", 0) > 0) {
        auto k = [](long n) {
            char buf[32];
            if (n >= 1000) snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
            else snprintf(buf, sizeof(buf), "%ld", n);
            return std::string(buf);
        };
        long input = usage_.value("last_input", 0L), context = usage_.value("context", 0L);
        nlohmann::json total = usage_.value("total", nlohmann::json::object());
        std::string ctx = "ctx " + k(input);
        const char* style = "status_dim";
        if (context > 0) {
            int pct = static_cast<int>(100.0 * input / context);
            ctx += "/" + k(context) + " (" + std::to_string(pct) + "%)";
            if (pct >= 85) style = "harness_tripped";
            else if (pct >= 60) style = "notice";
        }
        std::string cost;
        if (const auto c = usage_.value("cost", nlohmann::json::object()); c.contains("estimate") && c["estimate"].is_number()) {
            cost = " · ~" + format_cost(c["estimate"].get<double>(), c.value("currency", "")) + " est.";
        }
        parts.push_back(text(ctx + " · Σ↑" + k(total.value("input", 0L)) + " ↓" + k(total.value("output", 0L)) + cost + "  ") | decorate(settings_.style(style)));
    }
    parts.push_back(text(focus_ == Focus::Conversation ? "Ctrl-W j: input · v y / · :help  " : "Ctrl-W k: conversation · :help  ") |
                    decorate(settings_.style("status_dim")));
    return hbox(parts);
}

Element App::render_approval() {
    if (!approval_) return emptyElement();
    const auto& r = approval_->event;
    std::string tool = r.value("tool", ""), covers = r.value("always_covers", ""), preview = r.value("preview", "");
    std::string key = covers.empty() ? (tool == "run_shell" ? "this program" : "this file") : covers;
    Elements rows = {text(r.value("summary", "")) | bold, text("why asking: " + r.value("reason", "") + (r.value("origin", "local") == "remote" ? "  [REMOTE REQUEST]" : "")) | dim};
    if (r.contains("thread") && r["thread"].contains("session")) rows.push_back(text("from the background task " + r["thread"].value("title", r["thread"].value("session", ""))) | dim);
    if (!preview.empty()) {
        std::istringstream in(preview);
        int n = 0;
        for (std::string line; std::getline(in, line) && n < 14; ++n) {
            rows.push_back(render_line(settings_, {{line, diff_flags(line)}}, Style{}));
        }
    }
    if (approval_->typing) {
        rows.push_back(hbox({text("no, because: ") | bold, text(approval_->feedback), text(" ") | inverted, text("  (Enter sends this to the model, Esc cancels)") | dim}));
    } else {
        rows.push_back(hbox({text("[y]") | bold | decorate(settings_.style("harness_armed")), text(" yes   "), text("[n]") | bold | decorate(settings_.style("tool_err")), text(" no   "),
                             text("[N]") | bold | decorate(settings_.style("tool_err")), text(" no, and say why   "), text("[a]") | bold, text(" always: " + key + " (this session)   "),
                             text("[t]") | bold | decorate(settings_.style("error")), text(" trip the harness")}));
        if (!r.value("path", "").empty()) {
            Elements extra = {text("[e]") | bold, text(host_up() ? " open in nvim   " : " open in $EDITOR   ")};
            if (host_up() && r.contains("proposed_size")) extra.insert(extra.end(), {text("[d]") | bold, text(" diff in a new nvim tab")});
            rows.push_back(hbox(extra));
        }
    }
    return window(text(" approve? ") | bold, vbox(rows)) | decorate(settings_.style("approval"));
}

Element App::render_confirm() {
    if (!confirm_) return emptyElement();
    Elements rows;
    for (const auto& l : confirm_->lines) rows.push_back(text(l));
    return window(text(confirm_->title) | bold | decorate(settings_.style("error")), vbox(rows)) | decorate(settings_.style("approval"));
}

// The engine's question: a yes/no takes y and n in either case, the rest exactly one of their keys; Esc and Ctrl-C
// answer n. The answer goes back to the engine, which may ask the next.
bool App::handle_confirm(const Event& e) {
    if (!confirm_) return true;
    const std::string& k = e.input();
    std::string key;
    if (e == Event::Escape || k == "\x03") key = "n";
    else if (confirm_->keys == "yn" && (k == "y" || k == "Y" || k == "n" || k == "N")) key = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(k[0]))));
    else if (confirm_->keys != "yn" && k.size() == 1 && confirm_->keys.find(k) != std::string::npos) key = k;
    else return true;
    std::string id = confirm_->id;
    if (auto local = confirm_->local) {
        confirm_.reset();
        local(key);
        return true;
    }
    confirm_.reset();
    nlohmann::json reply = call("maid.session.command", {{"session", session_}, {"ask", id}, {"key", key}});
    drain();
    show(reply);
    return true;
}

Element App::render_question() {
    if (!question_) return emptyElement();
    Elements rows = {text(question_->text) | bold};
    for (size_t i = 0; i < question_->options.size(); ++i) {
        rows.push_back(hbox({text("[" + std::to_string(i + 1) + "]") | bold, text(" " + question_->options[i])}));
    }
    std::string hint = question_->options.empty() ? "  (Enter sends, Esc = no answer)" : "  (a number picks, or type an answer and Enter; Esc = no answer)";
    rows.push_back(hbox({text("answer: ") | bold, text(question_->typed), text(" ") | inverted, text(hint) | dim}));
    return window(text(" the agent asks ") | bold, vbox(rows)) | decorate(settings_.style("approval"));
}

Element App::render() {
    // FTXUI leaves ISIG on, so Ctrl-C would be a SIGINT that tears the UI down. Disable just the interrupt
    // and quit characters: Ctrl-C then arrives as a key, while Ctrl-Z still raises SIGTSTP from the line
    // discipline, which FTXUI turns into a proper suspend (its input parser drops the byte, so a key handler
    // could never do it). Checked every frame, since FTXUI re-installs the terminal after fg; it restores the
    // saved settings on exit.
    {
        termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0 && (t.c_cc[VINTR] != _POSIX_VDISABLE || t.c_cc[VQUIT] != _POSIX_VDISABLE || !(t.c_lflag & ISIG) || (t.c_iflag & IXON))) {
            t.c_lflag |= ISIG;
            t.c_cc[VINTR] = _POSIX_VDISABLE;
            t.c_cc[VQUIT] = _POSIX_VDISABLE;
            t.c_iflag &= ~IXON;  // Ctrl-S and Ctrl-Q are the pause keys, not flow control
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
    }
    auto size = Terminal::Size();
    size_t width = static_cast<size_t>(std::max(size.dimx, 20));
    int input_rows = 1;
    Element input = render_input(width, input_rows);
    int palette_rows = 0;
    Element palette = render_palette(palette_rows);
    bool is_asking = asking();
    bool focused = focus_ == Focus::Conversation;
    int approval_rows = 0;
    if (is_asking) {
        approval_rows = 5 + (approval_ && !approval_->event.value("path", "").empty() ? 1 : 0);
        if (approval_) {
            std::string preview = approval_->event.value("preview", "");
            approval_rows += std::min(14, static_cast<int>(std::count(preview.begin(), preview.end(), '\n')));
        }
        if (confirm_) approval_rows += static_cast<int>(confirm_->lines.size()) + 2;
        if (question_) approval_rows = 4 + static_cast<int>(question_->options.size());
    }
    if (pause_menu_ && paused_) approval_rows += 4;
    if (switcher_) approval_rows += static_cast<int>(std::min<size_t>(switcher_->ids.size(), 12)) + 3;
    view_height_ = std::max(1, size.dimy - input_rows - palette_rows - 3 - approval_rows - (focused ? 2 : 0));
    Element conversation = view_.render(settings_, focused ? width - 2 : width, view_height_);
    if (focused) conversation = conversation | borderLight | decorate(settings_.style("focus"));
    return vbox({conversation, render_approval(), render_question(), render_confirm(), render_switcher(), render_pause_menu(), render_top_status(), separator() | decorate(settings_.style("separator")), input, palette,
                 render_bottom_status()});
}

// ---------- keys ----------

bool App::handle(Event e) {
    if (e == Event::Custom) {
        redraw_posted_ = false;
        return true;
    }

    // A bracketed paste (maid.nvim's :MaidSend when MAID is not connected to it) goes into the input whole,
    // whatever the mode, and is never sent.
    if (e.input() == "\x1b[200~") {
        pasting_ = true;
        paste_buf_.clear();
        return true;
    }
    if (pasting_) {
        if (e.input() == "\x1b[201~") {
            pasting_ = false;
            paste_input(paste_buf_);
        } else if (e == Event::Return) {
            paste_buf_ += '\n';
        } else if (e == Event::Tab) {
            paste_buf_ += '\t';
        } else if (e.is_character()) {
            paste_buf_ += e.input();
        }
        return true;
    }

    // A fast Esc followed by a key arrives as one Alt-sequence ("\x1b:"). Vim users type that constantly,
    // so split it back into Esc plus the keys. Real escape sequences (CSI "\x1b[", SS3 "\x1bO") pass through.
    const std::string& raw = e.input();
    // Alt+Enter sends (nvim has no default Alt mappings, so nothing is lost). Terminals send it as Esc + Enter.
    // With enter_sends on, Enter itself sends a one-line input (the editor says so), and in insert mode Alt+Enter
    // or Shift+Enter (CSI u or modifyOtherKeys, as terminals encode it) is the line break instead.
    bool shift_enter = raw == "\x1b[13;2u" || raw == "\x1b[27;2;13~";
    if (raw == "\x1b\r" || raw == "\x1b\n" || shift_enter) {
        if (asking()) return true;
        bool inserting = editor_.mode() == Editor::Mode::Insert || editor_.mode() == Editor::Mode::Replace;
        if (settings_.enter_sends && inserting && focus_ == Focus::Input) editor_.newline();
        else if (!shift_enter) {
            // Normal mode after a send either way: the terminal's Esc + Enter arrives as one key or as two (the Esc
            // alone switching modes) depending on timing, and the mode afterwards must not depend on that.
            submit(editor_.text(), false);
            editor_.escape();
        }
        return true;
    }
    if (!e.is_mouse() && raw.size() >= 2 && raw[0] == '\x1b' && raw[1] != '[' && raw[1] != 'O') {
        handle(Event::Escape);
        for (size_t i = 1; i < raw.size(); i = utf8_next(raw, i)) handle(Event::Character(raw.substr(i, utf8_next(raw, i) - i)));
        return true;
    }
    if (e.is_mouse()) {
        auto& m = e.mouse();
        if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
            int dir = m.button == Mouse::WheelUp ? 1 : -1;
            view_.scroll_by(dir * 3);  // the wheel only scrolls the conversation; history is Ctrl-P / the arrows
        } else if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
            // A click on a tool call or result in the conversation window folds or unfolds it (like za).
            int top = focus_ == Focus::Conversation ? 1 : 0;  // the focus border takes a row
            if (m.y >= top && m.y < top + view_height_) view_.click(m.y - top);
        }
        return true;
    }
    if (raw != "\x03") quit_armed_ = false;
    status_msg_.clear();
    if (question_) return handle_question(e);
    if (!approval_ && confirm_) return handle_confirm(e);
    if (asking()) return handle_approval(e);
    if (switcher_) return handle_switcher(e);
    if (pause_menu_ && paused_) return handle_pause_menu(e);
    // Ctrl-S pauses a running turn (the interrupt steer), Ctrl-Q resumes a paused one: MAID keeps the terminal's
    // flow control off, so neither stops the output.
    if (raw == "\x13" && busy_ && !paused_ && !response_.empty()) return steer("interrupt"), true;
    if (raw == "\x11" && paused_) return steer("steer"), true;

    if (raw == "\x03") {  // Ctrl-C: interrupt, then clear input, then quit
        if (quit_when_idle_.exchange(false)) post(Kind::Notice, "staying after the reply (:wq cancelled)");
        if (shell_busy_) call("maid.session.shell", {{"session", session_}, {"interrupt", true}});
        else if (busy_) cancel_turn();
        else if (!editor_.empty()) editor_.clear();
        else if (quit_armed_) quit();
        else {
            quit_armed_ = true;
            post(Kind::Notice, "press Ctrl-C again to quit (or :q)");
        }
        return true;
    }
    if (e == Event::TabReverse && editor_.mode() != Editor::Mode::Command) {
        request_mode(next_mode(parse_mode(mode_).value_or(Mode::Manual)));
        return true;
    }
    // Ctrl-W j / k: move between the input and the conversation window (in insert mode Ctrl-W deletes a word).
    if (ctrl_w_pending_) {
        ctrl_w_pending_ = false;
        if (raw == "k" || e == Event::ArrowUp) set_focus(Focus::Conversation);
        else if (raw == "j" || e == Event::ArrowDown) set_focus(Focus::Input);
        else if (raw == "w" || raw == "\x17") set_focus(focus_ == Focus::Input ? Focus::Conversation : Focus::Input);
        return true;
    }
    if (raw == "\x17" && (focus_ == Focus::Conversation || editor_.mode() != Editor::Mode::Insert || editor_.empty())) {
        ctrl_w_pending_ = true;
        return true;
    }
    // Ctrl-X Ctrl-E: edit the input in nvim, the shell convention.
    if (ctrl_x_pending_) {
        ctrl_x_pending_ = false;
        if (raw == "\x05") edit_externally();
        return true;
    }
    if (raw == "\x18") {
        ctrl_x_pending_ = true;
        return true;
    }

    if (editor_.mode() == Editor::Mode::Command) {
        if (e == Event::Tab || raw == "\x0e") return complete_command(), true;  // Tab / Ctrl-N
        if (e == Event::TabReverse || raw == "\x10") {                            // Shift-Tab / Ctrl-P
            auto entries = palette_entries();
            if (!entries.empty()) palette_sel_ = (palette_sel_ + entries.size() - 1) % entries.size();
            return true;
        }
        act(editor_.handle(e));
        return true;
    }
    if (focus_ == Focus::Conversation) {
        if (raw == ":" || raw == "/") return editor_.begin_command(raw[0]), true;
        if (raw == "i" || raw == "a" || e == Event::Return) {
            set_focus(Focus::Input);
            if (raw == "i" || raw == "a") {
                editor_.escape();
                editor_.handle(Event::Character(raw));
            }
            return true;
        }
        if (e == Event::Escape && !view_.visual()) return set_focus(Focus::Input), true;
        status_msg_ = view_.handle(e, view_height_);
        return true;
    }

    // Input focus. From normal mode with nothing typed, the conversation keys work without switching windows.
    if (editor_.mode() == Editor::Mode::Normal && editor_.empty() && (raw == "v" || raw == "V")) {
        set_focus(Focus::Conversation);
        status_msg_ = view_.handle(e, view_height_);
        return true;
    }
    if (editor_.mode() != Editor::Mode::Insert) {
        if (raw == "\x04") return view_.half_page(1), true;   // Ctrl-D: down, toward newer lines, as in vim
        if (raw == "\x15") return view_.half_page(-1), true;  // Ctrl-U: up
        if (raw == "\x06" || e == Event::PageDown) return view_.page(1), true;
        if (raw == "\x02" || e == Event::PageUp) return view_.page(-1), true;
        if (raw == "\x05") return view_.scroll_by(-1), true;
        if (raw == "\x19") return view_.scroll_by(1), true;
        if (editor_.mode() == Editor::Mode::Normal && editor_.empty()) {
            if (raw == "j" || e == Event::ArrowDown) return view_.scroll_by(-1), true;
            if (raw == "k" || e == Event::ArrowUp) return view_.scroll_by(1), true;
            if (raw == "G") return view_.scroll_to_bottom(), true;
        }
    } else {
        if (e == Event::PageUp) return view_.page(-1), true;
        if (e == Event::PageDown) return view_.page(1), true;
    }
    act(editor_.handle(e));
    return true;
}

// What the editor asked for after a key: a command, a conversation search (`/`, `*`, `#`), or a send.

// What the editor asked for after a key: a command, a conversation search (`/`, `*`, `#`), or a send.
void App::act(const Editor::Result& r) {
    if (r.action == Editor::Action::Command) run_command(r.text);
    else if (r.action == Editor::Action::Search || r.action == Editor::Action::SearchBack) {
        view_.search(r.text);
        set_focus(Focus::Conversation);
        status_msg_ = view_.search_next(r.action == Editor::Action::Search ? 1 : -1);
    } else if (r.action == Editor::Action::Send) {
        submit(r.text, false);
    }
}

bool App::handle_approval(const Event& e) {
    const std::string& k = e.input();
    if (approval_ && approval_->typing) {
        if (e == Event::Escape) approval_->typing = false, approval_->feedback.clear();
        else if (e == Event::Return) answer(Approval::No, approval_->feedback);
        else if (e == Event::Backspace) {
            if (!approval_->feedback.empty()) approval_->feedback.erase(utf8_prev(approval_->feedback, approval_->feedback.size()));
        } else if (e.is_character()) approval_->feedback += k;
        return true;
    }
    if (k == "y" || k == "Y") answer(Approval::Yes);
    else if (k == "n" || e == Event::Escape) answer(Approval::No);
    else if (k == "N") {
        if (approval_) approval_->typing = true;
    }
    else if (k == "a" || k == "A") answer(Approval::Always);
    else if (k == "t" || k == "T") answer(Approval::Trip);
    else if (k == "e" || k == "d") {
        std::filesystem::path path = approval_ ? approval_->event.value("path", "") : "";
        if (path.empty()) status_msg_ = "this approval is not about a file";
        else if (k == "e") open_file(path);
        else if (!host_up()) status_msg_ = "d shows the diff in nvim: run MAID inside nvim (maid.nvim)";
        else {
            nlohmann::json proposed = result(call("maid.approval.proposed", {{"session", session_}, {"approval", approval_->id}}));
            if (!proposed.value("text", nlohmann::json()).is_string()) status_msg_ = "no proposed content to diff for this call";
            else {
                try {
                    host_diff(*host_, path, proposed["text"].get<std::string>());
                    status_msg_ = "the diff is in a new nvim tab; answer here";
                } catch (const std::exception& ex) {
                    post(Kind::Error, std::string("nvim: ") + ex.what());
                }
            }
        }
    }
    else if (k == "\x03") answer(Approval::No), cancel_turn();
    return true;
}

void App::answer(Approval a, std::string feedback) {
    if (!approval_) return;
    static const char* choices[] = {"yes", "no", "always", "trip"};
    std::string id = approval_->id;
    approval_.reset();
    call("maid.approval.answer", {{"session", session_}, {"approval", id}, {"choice", choices[static_cast<int>(a)]}, {"feedback", std::move(feedback)}});
    drain();
}

bool App::handle_question(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) answer_question("");
    else if (e == Event::Return) answer_question(question_->typed);
    else if (k == "\x03") answer_question(""), cancel_turn();
    else if (e == Event::Backspace) {
        if (!question_->typed.empty()) question_->typed.erase(utf8_prev(question_->typed, question_->typed.size()));
    } else if (e.is_character()) {
        size_t n = question_->typed.empty() && k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) ? static_cast<size_t>(k[0] - '0') : 0;
        if (n >= 1 && n <= question_->options.size()) answer_question(question_->options[n - 1]);
        else question_->typed += k;
    }
    return true;
}

void App::answer_question(std::string text) {
    if (!question_) return;
    view_.append(Kind::User, text.empty() ? "(no answer)" : text);
    std::string id = question_->id;
    question_.reset();
    call("maid.question.reply", {{"session", session_}, {"question", id}, {"text", std::move(text)}});
    drain();
}

// Ctrl-C: the running response is cancelled, and with it the turn.
void App::cancel_turn() {
    call("cancelResponse", {{"response_id", response_}});
    drain();
}

// Ctrl-S (interrupt), Ctrl-Q (steer, to resume) and the pause menu: a steering action on the running response or the
// paused turn. What is in the input goes with steer, drop and further as their note.
void App::steer(const std::string& action) {
    nlohmann::json params = {{"session", session_}, {"response_id", response_}, {"action", action}};
    std::string note = editor_.text();
    while (!note.empty() && std::isspace(static_cast<unsigned char>(note.back()))) note.pop_back();
    bool with_note = !note.empty() && (action == "steer" || action == "drop" || action == "further");
    if (with_note) params["note"] = note;
    nlohmann::json reply = call("maid.steer", params);
    drain();
    if (reply.contains("error")) {
        post(Kind::Error, reply["error"].value("message", "the engine refused the steer"));
        return;
    }
    if (with_note) editor_.clear();
    pause_menu_ = false;
}

Element App::render_pause_menu() {
    if (!pause_menu_ || !paused_) return emptyElement();
    return window(text(" paused ") | bold, vbox({text("[Ctrl-Q] resume  [s] steer  [d] drop  [f] further  [k] keep  [h] halt"),
                                                text("what is in the input goes with s, d and f as their note; Esc leaves this to type a message (Enter resumes with it)") | dim})) |
           decorate(settings_.style("approval"));
}

bool App::handle_pause_menu(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) pause_menu_ = false;
    else if (k == "\x11" || k == "s") steer("steer");
    else if (k == "d") steer("drop");
    else if (k == "f") steer("further");
    else if (k == "k") steer("keep");
    else if (k == "h") steer("halt");
    else if (k == "\x03") cancel_turn();
    return true;
}

void App::set_focus(Focus f) {
    focus_ = f;
    view_.set_focused(f == Focus::Conversation);
}

// ---------- actions ----------

void App::submit(std::string text, bool now) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty()) {
        if (now && queued_) {
            call("response.create", {{"conversation", session_}, {"maid", {{"now", true}}}});
            drain();
        }
        return;
    }
    editor_.remember(text);
    {
        std::ofstream f(state_dir() / "prompt-history.jsonl", std::ios::app);
        f << nlohmann::json{{"text", text}}.dump() << "\n";
    }
    editor_.clear();
    view_.scroll_to_bottom();
    if (text[0] == '/' && text.size() > 1 && text[1] != '/' && text[1] != ' ') {
        run_command(text.substr(1));
        return;
    }
    if (text[0] == '!') {
        run_shell(text.substr(1));
        return;
    }
    if (lua_mode_) {
        run_lua(text, false);
        return;
    }
    if (busy_ && !now && !response_.empty()) {
        // A message for the running response (or the paused turn, which it resumes): OpenAI's response.steer.
        nlohmann::json reply = call("response.steer", {{"previous_response_id", response_}, {"input", text}});
        if (!reply.contains("error")) {
            drain();
            if (!paused_) post(Kind::Notice, "queued; it reaches the model at its next step (:w now to force it)");
            return;
        }
        // It ended meanwhile: this one starts the next turn.
    }
    if (busy_) {
        nlohmann::json params = {{"conversation", session_}, {"input", text}};
        if (now) params["maid"] = {{"now", true}};
        nlohmann::json reply = call("response.create", params);
        if (!result(reply).contains("maid") || !result(reply)["maid"].value("queued", false)) {
            // The turn ended meanwhile: this one starts the next.
            busy_ = true;
            turn_seq_ = result(reply).contains("maid") ? result(reply)["maid"].value("sequence_number", -1L) : -1;
            response_ = result(reply).value("id", "");
            drain();
            return;
        }
        drain();
        post(Kind::Notice, now ? "delivering now" : "queued; it reaches the model at its next step (:w now to force it)");
        return;
    }
    start_turn(text);
}

void App::start_turn(const std::string& text_in) {
    std::string text = take_dropped_image(text_in);
    nlohmann::json reply = call("response.create", {{"conversation", session_}, {"input", text}});
    if (reply.contains("error")) {
        drain();
        post(Kind::Error, reply["error"].value("message", "the engine refused the message"));
        return;
    }
    busy_ = true;
    turn_seq_ = result(reply)["maid"].value("sequence_number", -1L);
    response_ = result(reply).value("id", "");
    drain();
}

// :image FILE through the engine, quietly: whether the picture is attached to the next message.
bool App::attach(const std::filesystem::path& file) {
    nlohmann::json reply = call("maid.session.command", {{"session", session_}, {"line", "image " + file.string()}});
    drain();
    return !reply.contains("error") && result(reply).value("ok", false);
}

// A file dragged onto the terminal arrives as its path in the input, quoted or backslash-escaped the way
// terminals write a drop. Only that shape is taken as an attachment: the whole message being one image
// path, or a message that begins with a quoted/escaped/file:// one. A plain path inside a sentence stays
// text, so a pasted path is something the agent can be asked to read rather than a picture MAID sends.
std::string App::take_dropped_image(const std::string& text) {
    // The explicit form first: ![alt](path) or [text](path) whose target is an image file, anywhere in the
    // message. Micaiah's suggestion: no guessing, and it works for a pasted path too.
    {
        std::string out;
        size_t pos = 0;
        bool any = false;
        while (true) {
            size_t lb = text.find('[', pos);
            if (lb == std::string::npos) break;
            size_t rb = text.find("](", lb);
            size_t rp = rb == std::string::npos ? std::string::npos : text.find(')', rb + 2);
            if (rp == std::string::npos) break;
            std::string alt = text.substr(lb + 1, rb - lb - 1), target = text.substr(rb + 2, rp - rb - 2);
            while (!target.empty() && (target.front() == '"' || target.front() == '\'' || target.front() == '<')) target.erase(0, 1);
            while (!target.empty() && (target.back() == '"' || target.back() == '\'' || target.back() == '>')) target.pop_back();
            if (target.rfind("file://", 0) == 0) target = target.substr(7);
            if (target.rfind("~/", 0) == 0) target = std::string(std::getenv("HOME")) + target.substr(1);
            std::filesystem::path p = std::filesystem::path(target).is_absolute() ? std::filesystem::path(target) : std::filesystem::path(ws_) / target;
            std::error_code ec;
            size_t start = lb > 0 && text[lb - 1] == '!' ? lb - 1 : lb;
            if (is_image_path(p) && std::filesystem::is_regular_file(p, ec) && attach(p)) {
                out += text.substr(pos, start - pos) + "[image: " + (alt.empty() ? p.filename().string() : alt) + "]";
                any = true;
                pos = rp + 1;
                continue;
            }
            out += text.substr(pos, rp + 1 - pos);
            pos = rp + 1;
        }
        if (any) return out + text.substr(pos);
    }
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t start = t.find_first_not_of(" \t\n");
    if (start == std::string::npos) return text;
    t = t.substr(start);
    std::string path, rest;
    bool dropped_shape = false;
    if (t.rfind("file://", 0) == 0) {
        dropped_shape = true;
        size_t sp = t.find_first_of(" \n");
        path = t.substr(7, sp == std::string::npos ? std::string::npos : sp - 7);
        rest = sp == std::string::npos ? "" : t.substr(sp);
    } else if (t.front() == '\'' || t.front() == '"') {
        size_t end = t.find(t.front(), 1);
        if (end == std::string::npos) return text;
        dropped_shape = true;
        path = t.substr(1, end - 1);
        rest = t.substr(end + 1);
    } else {
        // Backslash-escaped spaces mark a drop; a path with no spaces counts only when it is the whole message.
        size_t i = 0;
        while (i < t.size() && !(t[i] == ' ' || t[i] == '\n')) {
            if (t[i] == '\\' && i + 1 < t.size()) {
                path += t[i + 1];
                dropped_shape = true;
                i += 2;
            } else {
                path += t[i++];
            }
        }
        rest = t.substr(i);
        if (!dropped_shape && !rest.empty()) return text;  // a bare path followed by words: text
    }
    if (path.rfind("~/", 0) == 0) path = std::string(std::getenv("HOME")) + path.substr(1);
    std::filesystem::path p(path);
    std::error_code ec;
    if (!is_image_path(p) || !std::filesystem::is_regular_file(p, ec)) return text;
    if (!attach(p)) return text;
    std::string r = rest;
    size_t rs = r.find_first_not_of(" \t\n");
    r = rs == std::string::npos ? "" : r.substr(rs);
    return "[image: " + p.filename().string() + "]" + (r.empty() ? "" : " " + r);
}

// `!cmd` in the user's own shell, through the engine (maid.session.shell): it streams under the command's line
// and what it printed reaches the model as context.
void App::run_shell(const std::string& command) {
    if (shell_busy_) {
        post(Kind::Error, "a shell command is still running; Ctrl-C stops it");
        return;
    }
    view_.append(Kind::Shell, command);
    view_.append(Kind::ToolOk, "");
    if (shell_thread_.joinable()) shell_thread_.join();
    shell_busy_ = true;
    shell_thread_ = std::thread([this, command] {
        nlohmann::json reply = call("maid.session.shell", {{"session", session_}, {"command", command}}, false);
        screen_.Post([this, reply] {
            drain();
            if (reply.contains("error")) post(Kind::Error, reply["error"].value("message", "the command did not run"));
            else {
                if (int rc = result(reply).value("exit_code", 0); rc != 0) view_.append_to_last(Kind::ToolOk, "\n[exit code " + std::to_string(rc) + "]");
                post(Kind::Notice, result(reply).value("in_turn", false) ? "result queued; it reaches the model at the turn's next step" : "result added; the agent sees it with your next message");
            }
            shell_busy_ = false;
        });
        screen_.PostEvent(Event::Custom);
    });
}

// Opens the input in $VISUAL / $EDITOR (default nvim) as a markdown file and loads it back on exit.
void App::edit_externally() {
    char path[] = "/tmp/maid-input-XXXXXX.md";
    int fd = mkstemps(path, 3);
    if (fd < 0) {
        post(Kind::Error, "can't create a temporary file for the editor");
        return;
    }
    const std::string& current = editor_.text();
    [[maybe_unused]] ssize_t w = write(fd, current.data(), current.size());
    close(fd);
    const char* editor = std::getenv("VISUAL");
    if (!editor || !*editor) editor = std::getenv("EDITOR");
    if (!editor || !*editor) editor = "nvim";
    std::string command = std::string(editor) + " " + path;
    int rc = 0;
    screen_.WithRestoredIO([&] { rc = std::system(command.c_str()); })();
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    unlink(path);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (rc != 0) {
        post(Kind::Error, std::string(editor) + " exited with status " + std::to_string(rc) + "; input unchanged");
        return;
    }
    editor_.replace_text(text);
    editor_.escape();
}

// :lua and :luafile run in the session's Lua state in the engine; what the chunk prints reaches the model too.
void App::run_lua(const std::string& code, bool from_file) {
    nlohmann::json reply = call("maid.session.command", {{"session", session_}, {"line", (from_file ? "luafile " : "lua ") + code}});
    drain();
    view_.append(Kind::Shell, (from_file ? "luafile " : "lua> ") + code);
    if (reply.contains("error")) {
        post(Kind::Error, reply["error"].value("message", ""));
        return;
    }
    for (const auto& l : result(reply).value("lines", nlohmann::json::array())) view_.append(l.value("level", "info") == "info" ? Kind::ToolOk : Kind::ToolErr, l.value("text", ""));
    screen_.PostEvent(Event::Custom);
}

void App::run_command(const std::string& line) {
    if (!line.empty() && line[0] == '!') {
        run_shell(line.substr(1));
        return;
    }
    std::istringstream in(line);
    std::string cmd, arg;
    in >> cmd;
    std::getline(in >> std::ws, arg);
    bool told = false;
    auto services = [&] {
        std::vector<std::string> problems;
        auto defs = load_services(root_dir() / "services", &problems);
        for (const auto& p : problems) if (!told) post(Kind::Error, p);
        told = true;
        return defs;
    };
    // The commands that act on the session are the engine's (maid.session.command), under the names the TUI has
    // always taken; the rest are the view's, the editor's and the machine's.
    static const std::set<std::string> engine_owned = {"mode", "harness", "model", "models", "think", "undo", "export", "rename", "title", "budget", "compact",
                                                       "clear", "trip", "status", "usage", "todo", "tools", "init", "cd", "ban", "sampling", "sampler", "image", "img",
                                                       "forbid", "allow", "rule", "rules", "ctx", "context-size", "ctx2", "prefill", "prefix", "system",
                                                       "instructions", "session", "steer", "steering", "tier"};
    try {
        if (cmd == "wq") {
            // Send, then leave once the reply is in. With nothing to send it is just :q.
            if (!editor_.text().empty()) {
                quit_when_idle_ = true;
                submit(editor_.text(), false);
                post(Kind::Notice, "sending; quitting when the reply is in (Ctrl-C to stay)");
            } else {
                quit();
            }
        } else if (cmd == "q" || cmd == "q!" || cmd == "quit" || cmd == "exit") {
            if (cmd == "q!") quit_warned_ = true;
            std::istringstream in(arg);
            std::string as;
            for (std::string w; in >> w;) {
                if (w == "--bg" || w == "--park" || w == "--stop") as = w.substr(2);
            }
            quit(as);
        } else if (cmd == "new" || cmd == "switch" || cmd == "fork" || cmd == "bg" || cmd == "park" || cmd == "stop") {
            session_verb(cmd, arg);
        } else if (cmd == "w" || cmd == "write" || cmd == "send") {
            submit(editor_.text(), arg == "now" || arg == "!");
        } else if (cmd == "ww") {
            submit(editor_.text(), true);
        } else if (cmd == "e" || cmd == "edit") {
            if (arg.empty()) edit_externally();
            else open_file(resolve_path(ws_, arg));
        } else if (cmd == "nvim" || cmd == "host") {
            nvim_command(arg);
        } else if (cmd == "h" || cmd == "help") {
            post(Kind::Notice, help_text(arg));
        } else if (cmd == "set") {
            std::istringstream a(arg);
            std::string key, value;
            a >> key >> value;
            bool on = value != "off" && value != "false" && value != "0";
            if (key == "markdown" || key == "md") view_.set_markdown(on), post(Kind::Notice, on ? "markdown rendering on" : "markdown rendering off (raw text)");
            else if (key == "mouse") settings_.mouse = on, screen_.TrackMouse(on), post(Kind::Notice, on ? "mouse on (Shift+drag selects text in the terminal)" : "mouse off");
            else if (key == "timestamps" || key == "time") view_.set_timestamps(on), post(Kind::Notice, on ? "timestamps on" : "timestamps off");
            else if (key == "tooldetails" || key == "details") {
                view_.set_collapse_default(!on);
                view_.set_all_collapsed(!on);
                post(Kind::Notice, on ? "tool output shown in full (za folds one, zM all)" : "tool output folded to a preview (za unfolds one, zR all)");
            } else if (key == "highlight" || key == "hl") {
                if (value != "nvim" && value != "builtin") post(Kind::Error, ":set highlight nvim|builtin");
                else if (value == "nvim" && settings_.bare) post(Kind::Error, "highlight nvim runs nvim, and this MAID is bare (--bare, MAID_BARE=1 or bare = true); the built-in highlighter stays");
                else {
                    settings_.highlight = value;
                    nvim_hl_.reset();
                    nvim_hl_failed_ = false;
                    post(Kind::Notice, value == "nvim" ? "input highlighted by nvim (started on the next keystroke)" : "built-in input highlighter");
                }
            } else if (key == "enter_sends" || key == "entersends") {
                settings_.enter_sends = on;
                editor_.set_enter_sends(on);
                post(Kind::Notice, on ? "Enter sends a one-line input; Shift+Enter or Alt+Enter inserts a line break" : "Enter inserts a line break; Alt+Enter or :w sends");
            } else post(Kind::Error, ":set markdown|mouse|tooldetails|timestamps|enter_sends on|off, :set highlight nvim|builtin");
        } else if (cmd == "theme" || cmd == "themes" || cmd == "colorscheme" || cmd == "colo") {
            theme_command(arg);
        } else if (cmd == "lua" || cmd == "luafile") {
            if (cmd == "lua" && arg.empty()) {
                lua_mode_ = !lua_mode_;
                post(Kind::Notice, lua_mode_ ? "Lua mode: what you send runs in LuaJIT (globals persist). :chat or :lua returns to the model." : "back to the model");
            } else {
                run_lua(arg, cmd == "luafile");
            }
        } else if (cmd == "copy") {
            std::string last = view_.last_assistant();
            if (last.empty()) post(Kind::Error, "nothing to copy yet");
            else {
                register_ = last;
                post(Kind::Notice, "copied the last reply (" + copy_to_clipboard(last) + ")");
            }
        } else if (cmd == "stash" || cmd == "pop") {
            std::filesystem::path stash = state_dir() / "prompt-stash.jsonl";
            if (cmd == "stash") {
                if (editor_.text().empty()) post(Kind::Error, "nothing to stash");
                else {
                    std::ofstream f(stash, std::ios::app);
                    f << nlohmann::json{{"text", editor_.text()}}.dump() << "\n";
                    editor_.clear();
                    post(Kind::Notice, "stashed; :pop brings it back");
                }
            } else {
                std::vector<std::string> lines;
                std::ifstream f(stash);
                for (std::string l; std::getline(f, l);) lines.push_back(l);
                if (lines.empty()) post(Kind::Error, "the stash is empty");
                else {
                    auto j = nlohmann::json::parse(lines.back(), nullptr, false);
                    lines.pop_back();
                    std::ofstream w(stash, std::ios::trunc);
                    for (const auto& l : lines) w << l << "\n";
                    editor_.replace_text(j.value("text", ""));
                    editor_.escape();
                    post(Kind::Notice, "popped (" + std::to_string(lines.size()) + " left)");
                }
            }
        } else if (cmd == "chat") {
            lua_mode_ = false;
            post(Kind::Notice, "back to the model");
        } else if (cmd == "unlock") {
            if (!tripwire_state()) post(Kind::Notice, "harness is not tripped");
            else if (session_tripped()) {
                post(unlock_session() ? Kind::Notice : Kind::Error, unlock_session() ? "session lock removed; carry on" : "session lock removed, but the machine lock is set: `maid unlock` (sudo)");
            } else {
                screen_.WithRestoredIO([] {
                    [[maybe_unused]] int rc = run_helper("echo 'Unlocking the MAID harness.'; sudo -k && sudo /usr/local/sbin/maid-lock reset");
                })();
                post(Kind::Notice, tripwire_state() ? "still tripped" : "harness unlocked; carry on");
            }
        } else if (cmd == "up" || cmd == "down") {
            bool found = false;
            for (const auto& s : services()) {
                if (s.name != arg) continue;
                found = true;
                if (cmd == "up") {
                    require_armed("start services");
                    if (std::string freed = free_gpu_for(s, services()); !freed.empty()) post(Kind::Notice, freed);
                    post(Kind::Notice, "starting " + s.name + "…");
                    try {
                        bool ready = start_service(s);
                        post(Kind::Notice, ready ? s.name + " is ready" : s.name + " is still starting");
                        if (is_fim_server(s.name) && !ready) post(Kind::Notice, "once it is up, :gpu load " + s.name + " loads its coder");
                        try {
                            if (is_fim_server(s.name) && ready) post(Kind::Notice, load_fim(s, services()));
                        } catch (const std::exception& e) {
                            post(Kind::Error, e.what());
                        }
                    } catch (const std::exception& e) {
                        std::string why = explain_exit(s, services());
                        post(Kind::Error, std::string(e.what()) + (why.empty() ? "" : "\n" + why));
                    }
                } else {
                    stop_service(s);
                    post(Kind::Notice, s.name + " stopped");
                    try {
                        if (std::string back = restore_gpu_after(s, services()); !back.empty()) post(Kind::Notice, back);
                    } catch (const std::exception& e) {
                        post(Kind::Error, e.what());
                    }
                }
            }
            if (!found) post(Kind::Error, "unknown service: " + arg + (arg.empty() ? " (:up NAME)" : " (see :status)"));
        } else if (cmd == "trust" || cmd == "untrust") {
            std::istringstream words(arg);
            std::vector<std::string> args;
            for (std::string w; words >> w;) args.push_back(w);
            if (cmd == "trust" && args == std::vector<std::string>{"imports", "--approve"}) return command("trust imports --approve");
            try {
                post(Kind::Notice, trust_command(cmd, args, ws_));
            } catch (const std::exception& e) {
                post(Kind::Error, e.what());
            }
        } else if (cmd == "settings") {
            std::string out = "settings files in effect (nearest last, wins):";
            for (const auto& p : settings_.sources) out += "\n  " + p.string();
            if (settings_.sources.empty()) out += "\n  none (defaults). `maid settings init` writes the global settings.lua; `:init` scaffolds a project's.";
            out += "\nsessions home: " + session_home_dir(settings_).lexically_relative(sessions_dir()).string() + "  (sessions_home = " + settings_.sessions_home + ")";
            post(Kind::Notice, out);
        } else if (cmd == "lazylock" || cmd == "lazy-lock" || cmd == "lazy_lock") {
            std::string out;
            int rc = lazy_lock_command(arg, lazy_lock_path(settings_.lazy_lock), out);
            if (lazy_lock_) lazy_lock_->check();
            if (!out.empty() && out.back() == '\n') out.pop_back();
            post(rc == 2 ? Kind::Error : Kind::Notice, out);
        } else if (cmd == "gpu" || cmd == "vram") {
            try {
                if (arg.rfind("free", 0) == 0) {
                    require_armed("free GPU memory");
                    std::string what = arg.size() > 5 ? arg.substr(5) : "all";
                    post(Kind::Notice, gpu_free(services(), what));
                } else if (arg.rfind("load", 0) == 0) {
                    require_armed("load a model");
                    post(Kind::Notice, gpu_load(services(), arg.size() > 5 ? arg.substr(5) : "llamacpp-fim"));
                } else {
                    GpuReport report = gpu_report(services());
                    std::string fit = gpu_budget(report, settings_);
                    post(Kind::Notice, report.text() + (fit.empty() ? "" : fit + "\n") + ":gpu free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui] releases memory without stopping anything; :gpu load llamacpp-fim brings the coder back");
                }
            } catch (const std::exception& e) {
                post(Kind::Error, e.what());
            }
        } else if (cmd == "path" || cmd == "paths") {
            std::istringstream a(arg);
            std::string name, what;
            a >> name >> what;
            auto places = known_places(settings_, ws_, services(), std::filesystem::path(transcript_));
            if (name.empty()) {
                std::string out = "places (:path NAME shows one, :path NAME copy puts it on the clipboard, :open NAME opens it):";
                for (const auto& p : places) out += "\n  " + p.name + "  " + p.path.string();
                post(Kind::Notice, out);
            } else {
                try {
                    const auto& p = find_place(places, name);
                    if (what == "copy" || what == "yank" || what == "y") {
                        register_ = p.path.string();
                        post(Kind::Notice, p.path.string() + "\ncopied (" + copy_to_clipboard(p.path.string()) + ")");
                    } else {
                        post(Kind::Notice, p.name + ": " + p.path.string() + "  (" + p.description + ")");
                    }
                } catch (const std::exception& e) {
                    post(Kind::Error, e.what());
                }
            }
        } else if (cmd == "open") {
            try {
                std::istringstream a(arg);
                std::string name, flag, browser;
                a >> name >> flag >> browser;
                bool folder = flag == "folder" || flag == "--folder";
                if (!folder && flag != "--browser") browser = flag;  // `:open comfyui firefox`
                auto [cmdline, what] = open_command(name, settings_, ws_, services(), std::filesystem::path(transcript_), browser, folder);
                post(run_helper(cmdline) == 0 ? Kind::Notice : Kind::Error, "opened " + what);
            } catch (const std::exception& e) {
                post(Kind::Error, e.what());
            }
        } else if (cmd == "artifacts") {
            std::string out = "where MAID and its services keep things:";
            for (const auto& a : list_artifacts(services())) {
                auto u = measure(a);
                out += "\n  " + a.owner + "/" + a.name + "  " + a.path.string() + "  " + human_bytes(u.bytes) + " in " + std::to_string(u.files) + " files";
            }
            out += "\nclean with: maid artifacts clean OWNER/NAME [--older-than DAYS]";
            post(Kind::Notice, out);
        } else if (cmd == "reg" || cmd == "register") {
            std::string out = register_.empty() ? "register is empty" : "register:\n" + register_;
            for (const auto& [name, r] : editor_.registers()) out += "\n\"" + std::string(1, name) + (r.linewise ? " (lines):\n" : ":\n") + r.text;
            post(Kind::Notice, out);
        } else if (engine_owned.count(cmd)) {
            nlohmann::json reply = call("maid.session.command", {{"session", session_}, {"line", cmd + (arg.empty() ? "" : " " + arg)}});
            drain();
            show(reply);
            bool ok = !reply.contains("error") && result(reply).value("ok", false);
            if (ok && cmd == "clear") view_.clear();
            if (ok && cmd == "undo" && host_up()) host_checktime(*host_);
            // :gpu's budget reads the context sizes from the TUI's settings.
            if (ok && (cmd == "ctx" || cmd == "context-size" || cmd == "ctx2") && std::atoi(arg.c_str()) >= 1024) (cmd == "ctx2" ? settings_.context_2 : settings_.context) = std::atoi(arg.c_str());
        } else if (!cmd.empty()) {
            // A unique prefix runs the command, like vim's :abbreviations.
            auto matches = match_commands(cmd);
            if (matches.size() == 1 && matches.front()->name != cmd) {
                run_command(matches.front()->name + (arg.empty() ? "" : " " + arg));
            } else if (matches.size() > 1) {
                std::string list;
                for (const auto* m : matches) list += (list.empty() ? "" : ", ") + (":" + m->name);
                post(Kind::Error, ":" + cmd + " is ambiguous: " + list);
            } else {
                post(Kind::Error, "unknown command :" + cmd + " (try :help)");
            }
        }
    } catch (const std::exception& ex) {
        post(Kind::Error, ex.what());
    }
}

// ---------- sessions ----------

// The view follows the session this client's focus moved to: its last exchanges from attach, what it is writing,
// its waiting approval or question and its settings, in its workspace; the session left sends no more here.
void App::show_session(const std::string& id) {
    if (session_ != id) call("maid.session.unsubscribe", {{"session", session_}});  // refused when the leave ended it: nothing to undo
    session_ = id;
    nlohmann::json snap = result(call("maid.session.attach", {{"session", id}, {"exchanges", 10}}));
    approval_.reset();
    question_.reset();
    todo_.clear();
    approvals_seen_.clear();
    paused_ = pause_menu_ = false;
    tool_calls_ = 0;
    live_call_.clear();
    view_.clear();
    const nlohmann::json& e = snap.value("entry", nlohmann::json::object());
    if (snap.value("more_before", false)) view_.append(Kind::Notice, "(earlier turns: maid sessions read " + id + ")");
    for (const auto& item : snap.value("items", nlohmann::json::array())) {
        std::string type = item.value("type", "");
        const nlohmann::json& m = item.value("maid", nlohmann::json::object());
        if (type == "message") {
            std::string t;
            for (const auto& part : item.value("content", nlohmann::json::array())) t += part.value("text", "");
            view_.append(item.value("role", "") == "user" ? Kind::User : Kind::Assistant, t);
        } else if (type == "function_call_output" || type == "shell_call_output") {
            std::string out = type == "function_call_output" ? item.value("output", "") : item["output"].empty() ? "" : item["output"][0].value("stdout", "");
            if (m.value("collapsed", false)) out = m.value("head", "") + " … (" + std::to_string(m.value("size", size_t(0))) + " bytes)";
            view_.append(Kind::Tool, m.value("summary", ""));
            view_.append(m.value("ok", true) ? Kind::ToolOk : Kind::ToolErr, out);
        } else {
            view_.append(Kind::Notice, m.value("collapsed", false) ? m.value("head", "") + " …" : item.value("text", ""));
        }
    }
    if (snap["inflight"].is_object()) {
        for (const auto& open : snap["inflight"].value("items", nlohmann::json::array())) {
            std::string type = open["item"].value("type", ""), t = open.value("text", "");
            if (type == "message") view_.append(Kind::Assistant, t);
            else if (type == "reasoning") view_.append(Kind::Thinking, t);
            else if (type == "function_call_output" || type == "shell_call_output") {
                view_.append(Kind::Tool, open.contains("call") && open["call"].contains("maid") ? open["call"]["maid"].value("summary", "") : "");
                live_call_ = open["item"].value("id", "");
                live_next_[0] = live_next_[1] = open.value("size", t.size());
                view_.live_output(t);
            }
        }
    }
    for (const auto& a : snap.value("pending", nlohmann::json::array())) {
        if (!approval_) approval_ = PendingApproval{a.value("id", ""), a};
    }
    for (const auto& q : snap.value("questions", nlohmann::json::array())) {
        if (!question_) question_ = PendingQuestion{q.value("id", ""), q.value("text", ""), q.value("options", std::vector<std::string>{}), ""};
    }
    for (const auto& t : snap.value("todo", nlohmann::json::array())) todo_.push_back({t.value("text", ""), t.value("done", false)});
    usage_ = snap.value("usage", nlohmann::json::object());
    index_[id] = e;
    queued_ = 0;
    follow_entry(e);
    busy_ = e.value("activity", "idle") != "idle";
    response_ = e["response"].is_string() ? e["response"].get<std::string>() : "";
    turn_seq_ = snap.value("sequence_number", -1L);
    std::string ws = e.value("workspace", ws_);
    if (ws != ws_) follow_workspace(ws);
    view_.append(Kind::Notice, "session " + session_line(e));
    screen_.PostEvent(Event::Custom);
}

// One line about a session: what it is doing, its title (or id), its model and where it works.
std::string App::session_line(const nlohmann::json& e) const {
    std::string state = e.value("state", ""), activity = e.value("activity", "idle");
    // `waiting` on a parent also covers its tasks: one of them waits for an approval.
    const nlohmann::json waiting = e.contains("waiting") ? e["waiting"] : nlohmann::json();
    std::string doing = state == "parked" ? (e.value("unseen", false) ? "finished, parked" : "parked") : activity == "waiting" || waiting.is_object() ? "waiting" : activity != "idle" ? "working" : e.value("unseen", false) ? "finished" : "idle";
    if (doing == "waiting" && waiting.is_object()) doing += ": " + waiting.value("summary", waiting.value("kind", "")) + (waiting.contains("session") ? " (in a task)" : "");
    std::string ws = e.value("workspace", ""), home = std::getenv("HOME") ? std::getenv("HOME") : "";
    if (!home.empty() && ws.rfind(home + "/", 0) == 0) ws = "~" + ws.substr(home.size());
    std::string title = e.value("title", "");
    return std::string(e.value("kind", "") == "sub" ? "↳ " : "") + (title.empty() ? e.value("id", "") : title) + "  ·  " + doing + "  ·  " + e.value("model", "") + "  ·  " + ws;
}

// An id, a unique id prefix or a title (any letter case) among the sessions the engine holds; else what was typed,
// for maid.session.resume to find among the transcripts.
std::string App::resolve_session(const std::string& given) const {
    if (index_.count(given)) return given;
    auto lower = [](std::string t) {
        for (auto& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return t;
    };
    std::vector<std::string> hits;
    for (const auto& [id, e] : index_) {
        if (id.rfind(given, 0) == 0 || lower(e.value("title", "")) == lower(given)) hits.push_back(id);
    }
    return hits.size() == 1 ? hits.front() : given;
}

// :new, :switch, :fork, :bg, :park and :stop (`:h sessions`). --bg, --park or --stop names what happens to the
// session left, this once.
void App::session_verb(const std::string& cmd, const std::string& arg) {
    std::istringstream in(arg);
    std::string as, rest;
    for (std::string w; in >> w;) {
        if (w == "--bg" || w == "--park" || w == "--stop") as = w.substr(2);
        else rest += (rest.empty() ? "" : " ") + w;
    }
    if (cmd == "new") return go(Go::New, "", as, rest);
    if (cmd == "fork") return go(Go::Fork, "", as, "");
    if (cmd == "switch" && !rest.empty()) {
        std::string id = resolve_session(rest);
        if (id == session_) return post(Kind::Notice, "already in " + session_line(index_[id]));
        return go(Go::To, id, as, "");
    }
    if (cmd == "switch") return open_switcher(" switch to ", as);
    if (cmd == "bg") return open_switcher(" background this session, and go to ", "bg");
    std::string id = rest.empty() ? session_ : resolve_session(rest);
    if (id == session_) return open_switcher(" " + cmd + " this session, and go to ", cmd);
    end_other(id, cmd);
}

// Moves this client's focus: to a new session, a fork of this one, or another session (`target`), loaded or parked. `as` says what happens to the one left: "" for
// the leave.switch setting's choice, which may be to ask (the engine answers maid_leave_ask); parking or stopping one mid-turn interrupts it, so that is asked first.
void App::go(Go how, const std::string& target, std::string as, const std::string& dir, bool sure) {
    if (as.empty()) as = "default";
    if (!sure && busy_ && (as == "park" || as == "stop")) {
        confirm_ = PendingConfirm{"leave", " " + as + " a working session? ", {"Its turn is interrupted (y), or it stays (n)."}, "yn",
                                  [this, how, target, as, dir](const std::string& k) {
                                      if (k == "y") go(how, target, as, dir, true);
                                  }};
        return;
    }
    nlohmann::json leave = {{"as", as}};
    nlohmann::json reply;
    if (how == Go::New) reply = call("createConversation", {{"maid", {{"workspace", ws_}, {"leave", leave}}}});
    else if (how == Go::Fork) reply = call("maid.session.fork", {{"session", session_}, {"leave", leave}});
    else if (index_.count(target) && index_[target].value("state", "") != "parked") reply = call("maid.session.focus", {{"session", target}, {"leave", leave}});
    else reply = call("maid.session.resume", {{"session", target}, {"leave", leave}});
    if (reply.contains("error")) {
        drain();
        const nlohmann::json& data = reply["error"]["data"];
        if (data.is_object() && data.value("code", "") == "maid_leave_ask") {
            confirm_ = PendingConfirm{"leave", " leave this session? ", {"[b] background: it keeps working", "[p] park: it stops for now and resumes where it was",
                                                                       "[s] stop: it ends; its transcript stays (maid -r)", "Esc stays here"},
                                      "bps", [this, how, target, dir](const std::string& k) {
                                          if (k != "n") go(how, target, k == "b" ? "bg" : k == "p" ? "park" : "stop", dir);
                                      }};
            return;
        }
        show(reply);
        return;
    }
    drain();
    show_session(result(reply).value("id", ""));
    if (!dir.empty()) command("cd " + dir);
}

// :park ID or :stop ID: another session ends, asked first when it is mid-turn; this one stays in focus.
void App::end_other(const std::string& id, const std::string& verb, bool sure) {
    bool working = index_.count(id) && index_[id].value("state", "") != "parked" && index_[id].value("activity", "idle") != "idle";
    if (working && !sure) {
        confirm_ = PendingConfirm{verb, " " + verb + " a working session? ", {session_line(index_[id]), "Its turn is interrupted (y), or it goes on (n)."}, "yn",
                                  [this, id, verb](const std::string& k) {
                                      if (k == "y") end_other(id, verb, true);
                                  }};
        return;
    }
    nlohmann::json reply = call(verb == "park" ? "maid.session.park" : "maid.session.stop", {{"session", id}, {"interrupt", true}});
    drain();
    if (reply.contains("error")) return show(reply);
    post(Kind::Notice, (verb == "park" ? "parked " : "stopped ") + session_line(result(reply)) + (verb == "park" ? "  (:switch resumes it)" : "  (maid -r resumes it)"));
}

void App::open_switcher(const std::string& title, const std::string& leave) {
    Switcher sw{title, leave, {""}, 0};
    std::vector<const nlohmann::json*> loaded, parked;
    std::map<std::string, std::vector<const nlohmann::json*>> tasks;  // under their parent, this session's first
    for (const auto& [id, e] : index_) {
        if (id == session_) continue;
        std::string parent = e.value("kind", "") == "sub" && e["parent"].is_string() ? e["parent"].get<std::string>() : "";
        if (!parent.empty() && (parent == session_ || index_.count(parent))) tasks[parent].push_back(&e);
        else (e.value("state", "") == "parked" ? parked : loaded).push_back(&e);
    }
    auto newest = [](const nlohmann::json* a, const nlohmann::json* b) { return a->value("last_activity", "") > b->value("last_activity", ""); };
    std::sort(loaded.begin(), loaded.end(), newest);
    std::sort(parked.begin(), parked.end(), newest);
    auto add = [&](const std::string& id) {
        if (!id.empty() && id != session_) sw.ids.push_back(id);
        auto under = tasks[id];
        std::sort(under.begin(), under.end(), newest);
        for (const auto* t : under) sw.ids.push_back(t->value("id", ""));
    };
    add(session_);
    for (const auto* e : loaded) add(e->value("id", ""));
    for (const auto* e : parked) add(e->value("id", ""));
    if (sw.ids.size() > 1) sw.sel = 1;
    switcher_ = std::move(sw);
}

Element App::render_switcher() {
    if (!switcher_) return emptyElement();
    Elements rows;
    size_t first = switcher_->sel >= 12 ? switcher_->sel - 11 : 0;
    for (size_t i = first; i < switcher_->ids.size() && i < first + 12; ++i) {
        const std::string& id = switcher_->ids[i];
        std::string line = id.empty() ? "+ a new session in " + ws_ : session_line(index_.count(id) ? index_.at(id) : nlohmann::json{{"id", id}});
        Element row = text((i == switcher_->sel ? "▸ " : "  ") + line);
        if (i == switcher_->sel) row = row | decorate(settings_.style("visual"));
        rows.push_back(row);
    }
    rows.push_back(text("j/k move · Enter goes there · Esc stays") | decorate(settings_.style("status_dim")));
    return window(text(switcher_->title) | bold, vbox(rows)) | decorate(settings_.style("approval"));
}

bool App::handle_switcher(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape || k == "\x03" || k == "q") {
        switcher_.reset();
    } else if (k == "j" || e == Event::ArrowDown) {
        if (switcher_->sel + 1 < switcher_->ids.size()) ++switcher_->sel;
    } else if (k == "k" || e == Event::ArrowUp) {
        if (switcher_->sel > 0) --switcher_->sel;
    } else if (e == Event::Return) {
        Switcher sw = std::move(*switcher_);
        switcher_.reset();
        const std::string& id = sw.ids[sw.sel];
        go(id.empty() ? Go::New : Go::To, id, sw.leave, "");
    }
    return true;
}

// :q, with --bg, --park or --stop for this session (`as`; else leave.quit decides, which may be to ask), parking or
// stopping it mid-turn asked first.
void App::quit(const std::string& as, bool sure) {
    if (!sure && busy_ && (as == "park" || as == "stop")) {
        confirm_ = PendingConfirm{"leave", " " + as + " a working session? ", {"Its turn is interrupted (y), or it stays (n)."}, "yn", [this, as](const std::string& k) {
            if (k == "y") quit(as, true);
        }};
        return;
    }
    if (as.empty() && (busy_ ? settings_.leave.quitting.working : settings_.leave.quitting.idle) == "ask") {
        confirm_ = PendingConfirm{"leave", " quit: this session? ", {"[b] background: it keeps working", "[p] park: it stops for now and resumes where it was",
                                                                   "[s] stop: it ends; its transcript stays (maid -r)", "Esc stays here"},
                                  "bps", [this](const std::string& k) {
                                      if (k != "n") quit(k == "b" ? "bg" : k == "p" ? "park" : "stop");
                                  }};
        return;
    }
    // Background sessions in this process's own engine end with it, as leave.no_daemon says, mid-turn too. The
    // daemon's keep working.
    size_t working = 0;
    for (const auto& [id, e] : index_) working += !daemon_ && id != session_ && e.value("state", "") != "parked" && e.value("activity", "idle") != "idle";
    if (working > 0 && !quit_warned_) {
        quit_warned_ = true;
        post(Kind::Notice, std::to_string(working) + (working == 1 ? " other session is" : " other sessions are") + " still working: quitting " +
                               (settings_.leave.no_daemon == "stop" ? "stops" : "parks") + " them and interrupts their turns (leave.no_daemon). :q again quits.");
        return;
    }
    quit_as_ = as;
    // A draft that was never sent is stashed, so a reflexive :q loses nothing.
    if (!editor_.text().empty()) {
        std::ofstream f(state_dir() / "prompt-stash.jsonl", std::ios::app);
        f << nlohmann::json{{"text", editor_.text()}}.dump() << "\n";
        exit_note_ = "your unsent input was stashed; :pop in the next session brings it back";
    }
    shutdown();
    screen_.Exit();
}

void App::shutdown() {
    if (stopped_) return;
    stopped_ = true;
    if (host_) host_->set_handlers({});
    nvim_hl_.reset();
    // The session in focus becomes what :q's flag or leave.quit says (maid.session.leave): in the daemon a working
    // one can keep on; this process's own engine interrupts what still runs, as leave.no_daemon says, and parks the rest.
    call("maid.session.leave", quit_as_.empty() ? nlohmann::json::object() : nlohmann::json{{"as", quit_as_}});
    if (daemon_) daemon_->close();
    else engine_->shutdown();
    if (shell_thread_.joinable()) shell_thread_.join();
    {
        std::lock_guard lock(pump_mu_);
        pump_stop_ = true;
    }
    pump_cv_.notify_one();
    if (pump_.joinable()) pump_.join();
    take_events();  // the session's last events, parked, for the recording
    recorder_.reset();
    if (theme_thread_.joinable()) theme_thread_.join();
    if (nvim_colors_thread_.joinable()) nvim_colors_thread_.join();
    keymap_cancel_ = true;
    if (keymap_thread_.joinable()) keymap_thread_.join();
}

}  // namespace

// The settings files for `workspace` with the command line's flags over them.
Settings tui_settings(const TuiOptions& options, const std::filesystem::path& workspace) {
    Settings settings = load_settings(workspace);
    if (options.bare) settings.bare = true;
    if (options.model) settings.model = *options.model;
    apply_preset(settings, settings.model);
    settings.model = resolve_model_alias(settings.model);
    if (options.mode) settings.mode = *options.mode;
    if (options.record) settings.record = *options.record;
    if (options.system) settings.system_prompt = *options.system;
    if (options.prefill) settings.prefill = *options.prefill;
    if (options.ctx) settings.context = *options.ctx;
    if (options.ctx2) settings.context_2 = *options.ctx2;
    settings.rules.insert(settings.rules.end(), options.rules.begin(), options.rules.end());
    if (options.load_instructions) settings.load_instructions = *options.load_instructions;
    settings.bans.strings.insert(settings.bans.strings.end(), options.bans.begin(), options.bans.end());
    settings.bans.patterns.insert(settings.bans.patterns.end(), options.ban_patterns.begin(), options.ban_patterns.end());
    for (const auto& [k, v] : options.sampling.items()) settings.sampling[k] = v;
    if (options.harness) settings.harness = *options.harness;
    if (options.accept_dumb_auto) settings.dumb_auto_ok = true;
    return settings;
}

namespace {

// `maid --ui nvim`: nvim with this MAID's maid.nvim as the whole interface and this binary's `maid --rpc`, with the
// same agent flags, as its job. The user's config loads first; the plugin goes on 'runtimepath' after it (lazy.nvim
// resets the path at setup). Returns why only when nvim could not be started.
std::string exec_nvim_ui(const TuiOptions& options) {
    std::error_code ec;
    nlohmann::json o = {{"plugin", (root_dir() / "maid.nvim").string()}, {"cmd", std::filesystem::read_symlink("/proc/self/exe", ec).string()}, {"args", options.engine_args}};
    if (options.resume) o["session"] = options.resume->stem().string();
    setenv("MAID_UI", o.dump().c_str(), 1);
    execlp("nvim", "nvim", "-c",
           "lua local o = vim.json.decode(vim.env.MAID_UI); vim.env.MAID_UI = nil; vim.opt.rtp:prepend(o.plugin); vim.cmd('runtime plugin/maid.lua'); require('maid.ui').main(o)",
           static_cast<char*>(nullptr));
    unsetenv("MAID_UI");
    return std::string("cannot run nvim: ") + std::strerror(errno);
}

}  // namespace

int run_tui(const TuiOptions& options) {
    // The host nvim first, so the settings files' Lua can use maid.nvim (docs/nvim.md); never when bare. `bare = true`
    // in a settings file is known only once they are read, so then the host is dropped right after.
    const char* bare_env = std::getenv("MAID_BARE");
    bool bare = options.bare || (bare_env && std::string(bare_env) == "1");
    std::string host_refused;
    std::shared_ptr<HostNvim> host = bare ? nullptr : HostNvim::from_env(host_refused);
    set_lua_nvim_host(host);
    // Trust is settled before any project file is read: asked on the terminal, before the screen is drawn.
    std::filesystem::path ws = std::filesystem::current_path();
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) ask_trust(ws, std::cin, std::cout);
    std::vector<std::string> trust_lines = trust_notices(ws);
    for (const auto& n : settle_trust(ws)) trust_lines.push_back(n);
    Settings settings = tui_settings(options, ws);
    trust_lines.insert(trust_lines.end(), settings.warnings.begin(), settings.warnings.end());
    if (settings.bare) {
        set_lua_nvim_host(nullptr);
        host.reset();
        host_refused.clear();
    }
    if (settings.harness != "smart" && settings.harness != "dumb") {
        std::cerr << "maid: --harness must be smart or dumb\n";
        return 2;
    }
    if (settings.tripwire == "isolated" && !settings.allow_isolated) {
        std::cerr << "maid: tripwire = \"isolated\" (opting out of the machine lock) is not allowed: set allow_isolated = true in settings to permit it\n";
        return 2;
    }
    if (!parse_mode(settings.mode)) {
        fprintf(stderr, "maid: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    // --mode auto is asked for here and now; auto from the settings starts only where the workspace is trusted.
    if (!options.mode && parse_mode(settings.mode) == Mode::Auto) {
        if (std::string why = auto_held(ws); !why.empty()) {
            settings.mode = "manual";
            trust_lines.push_back(why);
        }
    }
    // nvim as the interface (--ui nvim, ui = "nvim"), started here with trust settled; the engine holds a due audit.
    // Never inside nvim (maid.nvim's :Maid is the interface there) and never bare; what it cannot take runs here.
    std::string ui = options.ui.value_or(settings.ui);
    if (ui == "nvim") {
        std::string why = std::getenv("NVIM") ? "inside nvim, where :Maid opens it"
                          : bare || settings.bare ? "with bare, which takes nothing from nvim"
                          : !options.context.empty() || !options.images.empty() || !options.initial_prompt.empty() || options.fork_at ? "with --context, --image, -i or --fork-at"
                                                                                                                                       : exec_nvim_ui(options);
        if (options.ui) {
            std::cerr << "maid: --ui nvim: " << why << "\n";
            return 2;
        }
        trust_lines.push_back("ui = \"nvim\": not " + why + "; MAID's own interface instead");
    }
    // An audit that is due holds here, before the screen is drawn (docs/audit-trail.md).
    audit_gate(settings);
    set_color_depth(settings.colors);
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(settings.mouse);
    std::string first = options.initial_prompt;
    if (first == "-") first.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    // The daemon holds the session when one runs and nothing on the command line needs this process's own engine.
    bool attach = false;
    if (int fd = settings.daemon == "attach" ? daemon_connect() : -1; fd >= 0) {
        close(fd);
        std::string why = not_for_daemon(options, true);
        attach = why.empty();
        if (!attach) trust_lines.push_back("the daemon runs, but " + why + " does not travel to it: this session runs in this process");
    }
    App app(screen, settings, options, attach, host, host_refused, [&options](const std::filesystem::path& ws) { return tui_settings(options, ws); });
    // --ctx: the local server is restarted to match before the first message; a restart that fails is said.
    for (const char* service : {options.ctx ? "llamacpp" : "", options.ctx2 ? "llamacpp-2" : ""}) {
        if (!*service) continue;
        try {
            if (std::string r = restart_llamacpp_if_changed(service); !r.empty()) app.startup_notice(r);
        } catch (const std::exception& e) {
            app.startup_notice(std::string(service) + " was not restarted for the new context size: " + e.what());
        }
    }
    app.welcome();
    for (const auto& n : trust_lines) app.startup_notice(n);
    // nvim as the interface, offered once when nvim is installed (docs/nvim.md).
    if (ui == "tui" && !options.ui && !bare && !settings.bare && !std::getenv("NVIM") && on_path("nvim") && !std::filesystem::exists(state_dir() / "ui-offered")) {
        app.startup_notice("nvim is installed: maid --ui nvim runs MAID with nvim as its interface, your config and mappings included; ui = \"nvim\" in settings makes it the default (maid help ui). Said once.");
        std::ofstream(state_dir() / "ui-offered");
    }
    app.attach_context();
    for (const auto& im : options.images) app.attach_image(im);
    if (!first.empty()) app.send(first);
    auto component = CatchEvent(Renderer([&] { return app.render(); }), [&](Event e) { return app.handle(e); });
    screen.Loop(component);
    set_lua_nvim_host(nullptr);
    // The way back, printed after the screen is restored: a temporary transcript lives in the runtime
    // directory and is never listed, so this is the only place its path is easy to find.
    std::cout << "transcript" << (settings.record ? "" : " (temporary; gone at logout)") << ": " << app.transcript_path() << "\n"
              << "resume it with: maid -r " << app.transcript_path() << "\n";
    if (!app.exit_note().empty()) std::cout << app.exit_note() << "\n";
    return 0;
}

}  // namespace maid
