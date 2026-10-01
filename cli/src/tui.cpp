#include "tui.hpp"

#include "commands.hpp"
#include "editor.hpp"
#include "highlight.hpp"
#include "nvim_host.hpp"
#include "maic/agent.hpp"
#include "maic/artifacts.hpp"
#include "maic/clipboard.hpp"
#include "maic/image.hpp"
#include "maic/places.hpp"
#include "maic/vendor.hpp"
#include "maic/lua.hpp"
#include "maic/nvim_host.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/theme.hpp"
#include "maic/tools.hpp"
#include "maic/tripwire.hpp"
#include "style.hpp"
#include "view.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <regex.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace maic {

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

// A command the user runs themselves (`!cmd`): their shell, their environment, no sandbox. Output streams to
// `on_output`; cancel kills the whole process group.
int run_user_shell(const std::string& command, const std::filesystem::path& cwd, const std::atomic<bool>& cancel,
                   const std::function<void(std::string_view)>& on_output) {
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

// Scaffolds a project: a MAIC.md placeholder and .maic/settings.lua. Returns what was created.
std::string init_project(const std::filesystem::path& ws) {
    std::string made;
    std::filesystem::create_directories(ws / ".maic");
    if (!std::filesystem::exists(ws / ".maic" / "settings.lua") && !std::filesystem::exists(ws / ".maic" / "settings.json")) {
        std::ofstream(ws / ".maic" / "settings.lua") << "-- Project settings for MAIC, committed with the code. Personal overrides go in settings.local.lua\n"
                                                          "-- (add it to .gitignore). Keys: docs/settings.md\n"
                                                          "return {\n}\n";
        made += "created .maic/settings.lua\n";
    }
    if (!std::filesystem::exists(ws / "MAIC.md")) {
        std::ofstream(ws / "MAIC.md") << "# " << ws.filename().string() << "\n\nStanding instructions for agents working in this project.\n";
        made += "created MAIC.md (transcripts for this project now go under sessions/projects/)\n";
    }
    return made;
}

std::filesystem::path session_home_dir(const Settings& settings) {
    return resolve_sessions_home(settings, std::filesystem::current_path());
}

struct PendingApproval {
    ApprovalRequest request;
    std::promise<ApprovalAnswer> answer;
    bool typing = false;   // after N: a sentence for the model is being typed
    std::string feedback;
};

// The question tool: shown like an approval; a number picks an option, typed text is a free answer.
// A yes/no the UI needs from the user before it does something (entering auto under a dumb harness).
struct PendingConfirm {
    std::string title;
    std::vector<std::string> lines;
    std::function<void(bool)> then;
};

struct PendingQuestion {
    std::string text;
    std::vector<std::string> options;
    std::promise<std::string> answer;
    std::string typed;
};

enum class Focus { Input, Conversation };

class App : public AgentEvents {
public:
    App(ScreenInteractive& screen, Settings settings, const std::optional<std::filesystem::path>& resume, bool append, std::optional<size_t> fork_at,
        std::shared_ptr<HostNvim> host, std::string host_refused)
        : host_(std::move(host)), host_refused_(std::move(host_refused)), screen_(screen), settings_(std::move(settings)),
          log_(!resume ? std::make_unique<SessionLog>("tui", settings_.record ? session_home_dir(settings_) : runtime_sessions_dir())
               : append && settings_.record ? std::make_unique<SessionLog>(SessionLog::Reopen{}, *resume)
                                            : std::make_unique<SessionLog>(SessionLog::Fork{}, *resume, fork_at.value_or(count_records(*resume)), "tui",
                                                                           settings_.record ? session_home_dir(settings_) : runtime_sessions_dir())),
          agent_(std::filesystem::current_path(), settings_.model), editor_(&register_), view_(&register_) {
        agent_.providers = settings_.providers;
        set_context(agent_.providers, settings_.context);
        set_context(agent_.providers, settings_.context_2, "llamacpp-2");
        agent_.think = settings_.think;
        agent_.review_with_model = settings_.harness != "dumb";
        agent_.reviewer_model = settings_.reviewer_model;
        agent_.small_model = settings_.small_model;
        agent_.reviewer_budget_tokens = settings_.reviewer_budget_tokens;
        agent_.presets = settings_.presets;
        dumb_auto_ok_ = settings_.dumb_auto_ok;
        if (auto m = parse_mode(settings_.mode)) {
            // Auto under a dumb harness is confirmed first; until then the session starts one step safer.
            if (*m == Mode::Auto && !agent_.review_with_model && !dumb_auto_ok_) {
                agent_.mode = Mode::Edit;
                request_mode(Mode::Auto);
            } else {
                agent_.mode = *m;
            }
        }
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
        agent_.compaction.at = settings_.compact_at;
        agent_.compaction.keep_results = settings_.compact_keep_results;
        agent_.budget_tokens = settings_.budget_tokens;
        agent_.set_instruction_names(settings_.instruction_files);
        agent_.load_instruction_files = settings_.load_instructions;
        agent_.system_prefix = resolve_system_prompt(settings_.system_prompt);
        agent_.prefill = resolve_system_prompt(settings_.prefill);
        agent_.rules = settings_.rules;
        agent_.set_permission(settings_.permission);
        agent_.agents = settings_.agents;
        agent_.set_forbid(settings_.forbid);
        set_tripwire_scope(settings_.tripwire, log_->path().string() + ".tripped");
        if (settings_.tripwire == "isolated") agent_.set_confined(true);
        agent_.reload_instructions();
        agent_.bans = settings_.bans;
        agent_.set_nvim_host(host_);
        apply_sampling();
        view_.set_timestamps(settings_.timestamps);
        if (resume) {
            LoadedSession old = load_session(*resume, fork_at.value_or(~size_t(0)));
            for (const auto& t : old.transcript) {
                if (t.type == "user") view_.append(Kind::User, t.text);
                else if (t.type == "assistant") view_.append(Kind::Assistant, t.text);
                else if (t.type == "tool_call") view_.append(Kind::Tool, t.text);
                else if (t.type == "tool_result") view_.append(t.ok ? Kind::ToolOk : Kind::ToolErr, t.text);
                else view_.append(Kind::Notice, t.text);
            }
            agent_.set_log(log_.get());
            agent_.restore(std::move(old.messages));
            view_.append(Kind::Notice, "resumed session " + resume->stem().string() + " (" + std::to_string(old.transcript.size()) + " entries" +
                                           (fork_at ? ", forked at record " + std::to_string(*fork_at) : "") + ")" +
                                           (append ? ", continuing in the same file" : ", continuing in a new file that points at it"));
        } else {
            agent_.set_log(log_.get());
        }
    }

    ~App() override { shutdown(); }

    void welcome();
    void startup_notice(const std::string& t) { view_.append(Kind::Notice, t); }
    void attach_image(const std::filesystem::path& f) {
        try {
            agent_.attach_image(f.string().rfind("~/", 0) == 0 ? std::filesystem::path(std::getenv("HOME")) / f.string().substr(2) : f);
            post(Kind::Notice, "image attached to the next message: " + f.filename().string() + "  (:image lists, :image clear drops)");
        } catch (const std::exception& e) {
            post(Kind::Error, e.what());
        }
    }
    // A file dragged onto the terminal arrives as its path in the input, quoted or backslash-escaped the way
    // terminals write a drop. Only that shape is taken as an attachment: the whole message being one image
    // path, or a message that begins with a quoted/escaped/file:// one. A plain path inside a sentence stays
    // text, so a pasted path is something the agent can be asked to read rather than a picture MAIC sends.
    std::string take_dropped_image(const std::string& text) {
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
                std::filesystem::path p = std::filesystem::path(target).is_absolute() ? std::filesystem::path(target) : agent_.harness().workspace() / target;
                std::error_code ec;
                size_t start = lb > 0 && text[lb - 1] == '!' ? lb - 1 : lb;
                if (is_image_path(p) && std::filesystem::is_regular_file(p, ec)) {
                    try {
                        agent_.attach_image(p);
                        out += text.substr(pos, start - pos) + "[image: " + (alt.empty() ? p.filename().string() : alt) + "]";
                        any = true;
                        pos = rp + 1;
                        continue;
                    } catch (const std::exception&) {
                    }
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
        try {
            agent_.attach_image(p);
        } catch (const std::exception&) {
            return text;
        }
        std::string r = rest;
        size_t rs = r.find_first_not_of(" \t\n");
        r = rs == std::string::npos ? "" : r.substr(rs);
        return "[image: " + p.filename().string() + "]" + (r.empty() ? "" : " " + r);
    }
    // The current provider's `sampling` settings table, merged into every request.
    void apply_sampling() {
        auto [provider, name] = resolve_model(agent_.providers, agent_.model);
        nlohmann::json s = settings_.sampling.is_object() ? settings_.sampling : nlohmann::json::object();
        nlohmann::json per_provider = provider.options.value("sampling", nlohmann::json::object());
        for (const auto& [k, v] : per_provider.items()) s[k] = v;
        for (const auto& [k, v] : live_sampling_.items()) {
            if (v.is_null()) s.erase(k);
            else s[k] = v;
        }
        agent_.sampling = s;
        agent_.operator_note_in_turn = provider.options.value("operator_note", provider.kind != "anthropic");
    }
    nlohmann::json live_sampling_ = nlohmann::json::object();  // :sampling changes, over the settings
    std::string transcript_path() const { return log_->path().string(); }
    std::string exit_note() const { return exit_note_; }
    void send(const std::string& text) { submit(text, false); }
    void attach_context(const std::vector<std::filesystem::path>& files) {
        for (const auto& f : files) {
            try {
                view_.append(Kind::Notice, agent_.add_context_file(f));
            } catch (const std::exception& e) {
                view_.append(Kind::Error, e.what());
            }
        }
    }
    Element render();
    bool handle(Event e);

    // AgentEvents, from the worker thread.
    void on_text(std::string_view delta, bool thinking) override {
        view_.append_to_last(thinking ? Kind::Thinking : Kind::Assistant, delta);
        screen_.PostEvent(Event::Custom);
    }
    void on_tool_call(const std::string& summary) override {
        ++tool_calls_;
        post(Kind::Tool, summary);
    }
    void on_tool_started(const std::string& tool, const std::string& path, const std::string& summary) override {
        if (!host_up()) return;
        std::string resolved = path;
        try {
            if (!path.empty()) resolved = agent_.harness().resolve(path).string();
        } catch (const std::exception&) {
        }
        fire("MaicToolCall", {{"tool", tool}, {"path", resolved}, {"summary", summary}});
    }
    void on_file_written(const std::filesystem::path& path, const std::string& tool) override {
        if (!host_up()) return;
        host_checktime(*host_);
        fire("MaicFileWritten", {{"tool", tool}, {"path", path.string()}});
    }
    void on_tool_result(const std::string& text, bool ok) override { post(ok ? Kind::ToolOk : Kind::ToolErr, text); }
    void on_notice(const std::string& text) override { post(Kind::Notice, text); }
    ApprovalAnswer ask(const ApprovalRequest& request) override {
        std::future<ApprovalAnswer> answer;
        {
            std::lock_guard lock(mu_);
            approval_.emplace(PendingApproval{request, {}});
            answer = approval_->answer.get_future();
        }
        screen_.PostEvent(Event::Custom);
        nlohmann::json data = {{"tool", request.tool}, {"path", request.path.string()}, {"summary", request.summary}, {"reason", request.reason}, {"verdict", "pending"}};
        fire("MaicApproval", data);
        ApprovalAnswer a = answer.get();
        const char* verdicts[] = {"yes", "no", "always", "trip"};
        data["verdict"] = verdicts[static_cast<int>(a.choice)];
        fire("MaicApproval", data);
        return a;
    }
    std::string question(const std::string& text, const std::vector<std::string>& options) override {
        std::future<std::string> answer;
        {
            std::lock_guard lock(mu_);
            question_.emplace(PendingQuestion{text, options, {}, ""});
            answer = question_->answer.get_future();
        }
        screen_.PostEvent(Event::Custom);
        return answer.get();
    }
    void on_todo(const std::vector<TodoItem>& items) override {
        {
            std::lock_guard lock(mu_);
            todo_ = items;
        }
        screen_.PostEvent(Event::Custom);
    }

private:
    void post(Kind k, std::string text) {
        view_.append(k, std::move(text));
        screen_.PostEvent(Event::Custom);
    }
    bool asking() {
        std::lock_guard lock(mu_);
        return approval_.has_value() || question_.has_value() || confirm_.has_value();
    }
    // Changes the mode; auto under a dumb harness is confirmed once per session (or dumb_auto_ok in settings).
    void request_mode(Mode m) {
        if (m == Mode::Auto && !agent_.review_with_model && !dumb_auto_ok_) {
            confirm_dumb_auto([this](bool yes) {
                if (yes) {
                    dumb_auto_ok_ = true;
                    agent_.mode = Mode::Auto;
                    post(Kind::Notice, "auto mode under a dumb harness: the rule list alone decides what runs. `:harness smart` brings the reviewer back.");
                } else {
                    post(Kind::Notice, "staying in " + std::string(mode_name(agent_.mode.load())));
                }
            });
            return;
        }
        agent_.mode = m;
    }
    void confirm_dumb_auto(std::function<void(bool)> then) {
        std::lock_guard lock(mu_);
        confirm_ = PendingConfirm{
            " dumb harness + auto mode ",
            {"No model reads the conversation before the agent acts. In auto mode only the rule list stands between",
             "the agent and your shell: the trip patterns, the read-only classifier, the workspace fence and the",
             "sandbox. A wrong but well-formed command runs. Nothing asks you first.",
             "",
             "Continue into auto mode?  [y] yes, for this session   [n] stay in " + std::string(mode_name(agent_.mode.load())) +
                 "   (dumb_auto_ok = true in settings skips this)"},
            std::move(then)};
        screen_.PostEvent(Event::Custom);
    }
    std::string todo_text() {
        std::lock_guard lock(mu_);
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
    // The host nvim (maic.nvim, docs/nvim.md): connected before the settings were read, or refused with a reason.
    std::shared_ptr<HostNvim> host_;
    std::string host_refused_;
    std::atomic<bool> follow_theme_{false};  // the theme follows the host's colorscheme
    bool host_up() const { return host_ && host_->connected(); }
    void start_host();
    void follow_host_theme(bool announce);  // on the host's handler thread: read its colorscheme, apply it here
    void fire(const std::string& event, nlohmann::json data);  // a User autocmd in the host, with the session id
    void nvim_command(const std::string& arg);  // :nvim, :nvim theme
    void open_file(const std::filesystem::path& path);  // in the host, or in $EDITOR in MAIC's place
    void paste_input(const std::string& text);  // appended to the input, never sent
    bool pasting_ = false;  // inside a bracketed paste (maic.nvim's fallback when MAIC is not connected)
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
    void submit(std::string text, bool now);
    void start_turn(const std::string& text);
    void run_command(const std::string& line);
    void run_shell(const std::string& command);
    void set_model(const std::string& model);
    void set_focus(Focus f);
    void edit_externally();
    void quit();
    void shutdown();

    ScreenInteractive& screen_;
    Settings settings_;
    std::unique_ptr<SessionLog> log_;
    std::string log_path() const { return log_->path().string() + (settings_.record ? "" : "  (temporary: --no-record)"); }
    Agent agent_;
    std::string register_;
    Editor editor_;
    View view_;
    Focus focus_ = Focus::Input;

    std::mutex mu_;  // guards approval_, question_ and todo_
    std::optional<PendingApproval> approval_;
    std::optional<PendingQuestion> question_;
    std::optional<PendingConfirm> confirm_;
    bool dumb_auto_ok_ = false;
    Element render_confirm();
    bool handle_confirm(const Event& e);
    std::vector<TodoItem> todo_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;

    std::atomic<bool> shell_busy_{false};
    std::atomic<bool> shell_cancel_{false};
    std::thread shell_thread_;

    std::string status_msg_;
    std::atomic<int> tool_calls_{0};  // this turn
    std::atomic<bool> quit_when_idle_{false};  // :wq
    std::string exit_note_;
    bool titled_ = false;
    void maybe_title(const std::string& first_prompt);
    void set_title(const std::string& title);
    std::unique_ptr<Lua> lua_;  // created on first :lua; keeps globals between calls
    bool lua_mode_ = false;     // :lua with no argument: sends go to Lua until :chat (or :lua again)
    void run_lua(const std::string& code, bool from_file);
    bool ctrl_w_pending_ = false;
    bool ctrl_x_pending_ = false;
    size_t palette_sel_ = 0;
    std::string palette_for_;  // the command line the selection belongs to
    bool quit_armed_ = false;
    int view_height_ = 10;
};

void App::welcome() {
    std::string remote = agent_.remote() ? "  ·  REMOTE" : "  ·  local";
    view_.append(Kind::Notice, "MAIC  ·  workspace " + agent_.harness().workspace().string() + "  ·  model " + agent_.model + remote);
    std::string files;
    for (const auto& f : agent_.instructions()) files += (files.empty() ? "" : ", ") + f.path.string();
    if (!settings_.theme_error.empty()) view_.append(Kind::Error, settings_.theme_error + "; the default theme is in use (:theme reload after fixing it)");
    if (session_tripped()) view_.append(Kind::Error, "this session is tripped (its own lock, from an earlier run): :unlock removes it");
    view_.append(Kind::Notice, "session transcript: " + log_path() + (files.empty() ? "" : "\ninstructions: " + files));
    if (!agent_.tools().empty() || !agent_.script_tools().empty()) {
        std::string names;
        for (const auto& t : agent_.tools()) names += (names.empty() ? "" : ", ") + t.name;
        for (const auto& t : agent_.script_tools()) names += (names.empty() ? "" : ", ") + t.name;
        view_.append(Kind::Notice, "tools: " + names + "  (:tools lists them)");
    }
    for (const auto& n : agent_.tool_notices()) view_.append(Kind::Error, n);
    start_host();
    view_.append(Kind::Notice, std::string("Press i to type, ") + (settings_.enter_sends ? "Enter to send a one-line input (Shift+Enter or Alt+Enter for a new line, :w sends any)" : "Alt+Enter (or :w) to send, Enter for a new line") +
                                   ". Esc = normal mode: j/k scroll, u/Ctrl-R undo/redo, :e opens nvim, Ctrl-W k = conversation window, :help for everything.");
    if (agent_.remote()) view_.append(Kind::Error, "This model runs off this machine: prompts, files the agent reads and command output are sent to it.");
    // The service behind the current model, if any: say so when it is down. Never a service the model does not use.
    try {
        auto [provider, name] = resolve_model(agent_.providers, agent_.model);
        if (!provider.remote()) {
            std::string hint = unreachable_hint(provider, load_services(root_dir() / "services"));
            if (hint.find("is not running") != std::string::npos) view_.append(Kind::Error, hint.substr(0, hint.find(':')) + ": :up " + hint.substr(0, hint.find(' ')));
        }
    } catch (const std::exception& e) {
        view_.append(Kind::Error, e.what());
    }
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
    if (settings_.highlight == "nvim" && !nvim_hl_failed_) {
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
    try {
        for (const auto& s : load_services(root_dir() / "services")) ctx.services.push_back(s.name);
    } catch (const std::exception&) {
    }
    for (const auto& p : agent_.providers) ctx.providers.push_back(p.name);
    ctx.models = installed_models();
    std::string cmd = line.substr(0, space), partial = line.substr(space + 1);
    auto matches = match_commands(cmd);
    if (!matches.empty() && matches.front()->name == "theme") {
        for (const auto& t : list_themes()) ctx.themes.push_back(t.name);
        if (partial.rfind("nvim:", 0) == 0) ctx.nvim_colors = nvim_colors();
    }
    return complete_argument(matches.empty() ? cmd : matches.front()->name, partial, ctx);
}

std::vector<std::string> App::installed_models() {
    auto now = std::chrono::steady_clock::now();
    if (!models_cache_.empty() && now - models_cached_at_ < std::chrono::seconds(30)) return models_cache_;
    std::vector<std::string> out;
    for (size_t i = 0; i < agent_.providers.size(); ++i) {
        const auto& p = agent_.providers[i];
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
    h.colorscheme = [this] { follow_host_theme(false); };
    h.error = [this](const std::string& why) { post(Kind::Error, "nvim: " + why); };
    h.closed = [this] { post(Kind::Notice, "nvim: the host is gone; :e and the theme are MAIC's own again"); };
    host_->set_handlers(std::move(h));
    follow_theme_ = settings_.follow_nvim_theme;
    try {
        host_watch_colorscheme(*host_, host_->channel());
    } catch (const std::exception& e) {
        view_.append(Kind::Error, std::string("nvim: cannot watch its colorscheme: ") + e.what());
        follow_theme_ = false;
    }
    view_.append(Kind::Notice, "nvim: connected to the nvim MAIC runs in (" + host_->socket() + "): :e FILE opens there, e and d at an approval show the file and the diff, :nvim says more");
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
    data["session"] = log_->path().stem().string();
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
        post(Kind::Notice, "nvim: connected to " + host_->socket() + " as channel " + std::to_string(host_->channel()) + " (client \"maic\")\n"
                           "  :e FILE and e at an approval open files there, d at a write's approval diffs it in a new tab\n"
                           "  User autocmds MaicTurnStart, MaicToolCall, MaicApproval, MaicFileWritten, MaicTurnEnd fire there\n"
                           "  the model has the diagnostics tool; your Lua has maic.nvim\n"
                           "  theme: " + std::string(follow_theme_ ? "follows its colorscheme (" + settings_.theme + ")" : "your own (" + settings_.theme + "); :nvim theme follows nvim's"));
    } else if (host_) {
        post(Kind::Notice, "nvim: the host this session connected to is gone");
    } else {
        const char* sock = std::getenv("NVIM");
        post(Kind::Notice, std::string("nvim: no host. ") + (sock && *sock ? "$NVIM was refused: " + host_refused_ : "$NVIM is not set: MAIC is not running inside nvim") +
                               "\nmaic.nvim (:Maic in nvim) runs MAIC in a terminal there; :h nvim");
    }
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
    Mode m = agent_.mode.load();
    std::string mode_s(mode_name(m));
    Elements left = {
        text(" ⏵ " + mode_s) | decorate(settings_.style("mode_" + mode_s)),
        text(" (shift-tab to cycle)") | decorate(settings_.style("status_dim")),
    };
    Elements right;
    right.push_back(text(agent_.model + (agent_.think ? " +think" : "")));
    right.push_back(text(agent_.remote() ? " REMOTE" : " local") | decorate(settings_.style(agent_.remote() ? "remote" : "status_dim")));
    if (host_up()) right.push_back(text(" nvim") | decorate(settings_.style("status_dim")));
    right.push_back(text(" · ") | decorate(settings_.style("status_dim")));
    if (tripwire_state()) right.push_back(text("HARNESS TRIPPED") | decorate(settings_.style("harness_tripped")));
    else right.push_back(text("harness armed") | decorate(settings_.style("harness_armed")));
    {
        std::lock_guard lock(mu_);
        if (!todo_.empty()) {
            size_t done = std::count_if(todo_.begin(), todo_.end(), [](const TodoItem& t) { return t.done; });
            right.push_back(text(" · todo " + std::to_string(done) + "/" + std::to_string(todo_.size()) + " done") | decorate(settings_.style("notice")));
        }
    }
    if (size_t q = agent_.queued()) right.push_back(text(" · " + std::to_string(q) + " queued (:w now)") | decorate(settings_.style("notice")));
    if (busy_) right.push_back(text(" · working… ctrl-c interrupts") | decorate(settings_.style("notice")));
    if (shell_busy_) right.push_back(text(" · shell running") | decorate(settings_.style("shell")));
    if (lua_mode_) right.push_back(text(" · LUA MODE (:chat returns)") | decorate(settings_.style("shell")));
    if (!agent_.review_with_model) right.push_back(text(" · DUMB HARNESS") | decorate(settings_.style("error")));
    if (agent_.harness().confined()) right.push_back(text(" · ISOLATED") | decorate(settings_.style("notice")));
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
    auto u = agent_.usage();
    if (u.calls) {
        auto k = [](long n) {
            char buf[32];
            if (n >= 1000) snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
            else snprintf(buf, sizeof(buf), "%ld", n);
            return std::string(buf);
        };
        std::string ctx = "ctx " + k(u.last.input);
        const char* style = "status_dim";
        if (u.last.context > 0) {
            int pct = static_cast<int>(100.0 * u.last.input / u.last.context);
            ctx += "/" + k(u.last.context) + " (" + std::to_string(pct) + "%)";
            if (pct >= 85) style = "harness_tripped";
            else if (pct >= 60) style = "notice";
        }
        parts.push_back(text(ctx + " · Σ↑" + k(u.total_input) + " ↓" + k(u.total_output) + "  ") | decorate(settings_.style(style)));
    }
    parts.push_back(text(focus_ == Focus::Conversation ? "Ctrl-W j: input · v y / · :help  " : "Ctrl-W k: conversation · :help  ") |
                    decorate(settings_.style("status_dim")));
    return hbox(parts);
}

Element App::render_approval() {
    std::lock_guard lock(mu_);
    if (!approval_) return emptyElement();
    const auto& r = approval_->request;
    std::string key = r.always_covers.empty() ? (r.tool == "run_shell" ? "this program" : "this file") : r.always_covers;
    Elements rows = {text(r.summary) | bold, text("why asking: " + r.reason + (r.origin == Origin::Remote ? "  [REMOTE REQUEST]" : "")) | dim};
    if (!r.preview.empty()) {
        std::istringstream in(r.preview);
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
        if (!r.path.empty()) {
            Elements extra = {text("[e]") | bold, text(host_up() ? " open in nvim   " : " open in $EDITOR   ")};
            if (host_up() && r.proposed) extra.insert(extra.end(), {text("[d]") | bold, text(" diff in a new nvim tab")});
            rows.push_back(hbox(extra));
        }
    }
    return window(text(" approve? ") | bold, vbox(rows)) | decorate(settings_.style("approval"));
}

Element App::render_confirm() {
    std::lock_guard lock(mu_);
    if (!confirm_) return emptyElement();
    Elements rows;
    for (const auto& l : confirm_->lines) rows.push_back(text(l));
    return window(text(confirm_->title) | bold | decorate(settings_.style("error")), vbox(rows)) | decorate(settings_.style("approval"));
}

bool App::handle_confirm(const Event& e) {
    const std::string& k = e.input();
    std::function<void(bool)> then;
    bool yes = false;
    {
        std::lock_guard lock(mu_);
        if (!confirm_) return true;
        if (k == "y" || k == "Y") yes = true;
        else if (k == "n" || k == "N" || e == Event::Escape || k == "\x03") yes = false;
        else return true;
        then = std::move(confirm_->then);
        confirm_.reset();
    }
    if (then) then(yes);
    return true;
}

Element App::render_question() {
    std::lock_guard lock(mu_);
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
        if (tcgetattr(STDIN_FILENO, &t) == 0 && (t.c_cc[VINTR] != _POSIX_VDISABLE || t.c_cc[VQUIT] != _POSIX_VDISABLE || !(t.c_lflag & ISIG))) {
            t.c_lflag |= ISIG;
            t.c_cc[VINTR] = _POSIX_VDISABLE;
            t.c_cc[VQUIT] = _POSIX_VDISABLE;
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
        std::lock_guard lock(mu_);
        approval_rows = 5 + (approval_ && !approval_->request.path.empty() ? 1 : 0);
        if (approval_) approval_rows += std::min(14, static_cast<int>(std::count(approval_->request.preview.begin(), approval_->request.preview.end(), '\n')));
        if (confirm_) approval_rows += static_cast<int>(confirm_->lines.size()) + 2;
        if (question_) approval_rows = 4 + static_cast<int>(question_->options.size());
    }
    view_height_ = std::max(1, size.dimy - input_rows - palette_rows - 3 - approval_rows - (focused ? 2 : 0));
    Element conversation = view_.render(settings_, focused ? width - 2 : width, view_height_);
    if (focused) conversation = conversation | borderLight | decorate(settings_.style("focus"));
    return vbox({conversation, render_approval(), render_question(), render_confirm(), render_top_status(), separator() | decorate(settings_.style("separator")), input, palette,
                 render_bottom_status()});
}

// ---------- keys ----------

bool App::handle(Event e) {
    if (e == Event::Custom) return true;

    // A bracketed paste (maic.nvim's :MaicSend when MAIC is not connected to it) goes into the input whole,
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
        else if (!shift_enter) submit(editor_.text(), false);
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
    if (asking()) {
        bool q = false, c = false;
        {
            std::lock_guard lock(mu_);
            q = question_.has_value();
            c = !q && !approval_ && confirm_.has_value();
        }
        if (q) return handle_question(e);
        if (c) return handle_confirm(e);
    }
    if (asking()) return handle_approval(e);

    if (raw == "\x03") {  // Ctrl-C: interrupt, then clear input, then quit
        if (quit_when_idle_.exchange(false)) post(Kind::Notice, "staying after the reply (:wq cancelled)");
        if (shell_busy_) shell_cancel_ = true;
        else if (busy_) cancel_ = true;
        else if (!editor_.empty()) editor_.clear();
        else if (quit_armed_) quit();
        else {
            quit_armed_ = true;
            post(Kind::Notice, "press Ctrl-C again to quit (or :q)");
        }
        return true;
    }
    if (e == Event::TabReverse && editor_.mode() != Editor::Mode::Command) {
        request_mode(next_mode(agent_.mode.load()));
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
        if (raw == "\x04") return view_.half_page(-1), true;
        if (raw == "\x15") return view_.half_page(1), true;
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
    {
        std::lock_guard lock(mu_);
        if (approval_ && approval_->typing) {
            if (e == Event::Escape) approval_->typing = false, approval_->feedback.clear();
            else if (e == Event::Return) {
                approval_->answer.set_value({Approval::No, approval_->feedback});
                approval_.reset();
            } else if (e == Event::Backspace) {
                if (!approval_->feedback.empty()) approval_->feedback.erase(utf8_prev(approval_->feedback, approval_->feedback.size()));
            } else if (e.is_character()) approval_->feedback += k;
            return true;
        }
    }
    if (k == "y" || k == "Y") answer(Approval::Yes);
    else if (k == "n" || e == Event::Escape) answer(Approval::No);
    else if (k == "N") {
        std::lock_guard lock(mu_);
        if (approval_) approval_->typing = true;
    }
    else if (k == "a" || k == "A") answer(Approval::Always);
    else if (k == "t" || k == "T") answer(Approval::Trip);
    else if (k == "e" || k == "d") {
        std::filesystem::path path;
        std::optional<std::string> proposed;
        {
            std::lock_guard lock(mu_);
            if (approval_) path = approval_->request.path, proposed = approval_->request.proposed;
        }
        if (path.empty()) status_msg_ = "this approval is not about a file";
        else if (k == "e") open_file(path);
        else if (!host_up()) status_msg_ = "d shows the diff in nvim: run MAIC inside nvim (maic.nvim)";
        else if (!proposed) status_msg_ = "no proposed content to diff for this call";
        else {
            try {
                host_diff(*host_, path, *proposed);
                status_msg_ = "the diff is in a new nvim tab; answer here";
            } catch (const std::exception& ex) {
                post(Kind::Error, std::string("nvim: ") + ex.what());
            }
        }
    }
    else if (k == "\x03") answer(Approval::No), cancel_ = true;
    return true;
}

void App::answer(Approval a, std::string feedback) {
    std::lock_guard lock(mu_);
    if (approval_) {
        approval_->answer.set_value({a, std::move(feedback)});
        approval_.reset();
    }
}

// Called with mu_ held.
bool App::handle_question(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) answer_question("");
    else if (e == Event::Return) answer_question(question_->typed);
    else if (k == "\x03") answer_question(""), cancel_ = true;
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
    if (question_) {
        view_.append(Kind::User, text.empty() ? "(no answer)" : text);
        question_->answer.set_value(std::move(text));
        question_.reset();
    }
}

void App::set_focus(Focus f) {
    focus_ = f;
    view_.set_focused(f == Focus::Conversation);
}

// ---------- actions ----------

void App::submit(std::string text, bool now) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty()) {
        if (now && agent_.queued()) agent_.deliver_now();
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
    if (busy_) {
        view_.append(Kind::User, text);
        agent_.post_message(text);
        if (now) {
            agent_.deliver_now();
            post(Kind::Notice, "delivering now");
        } else {
            post(Kind::Notice, "queued; it reaches the model at its next step (:w now to force it)");
        }
        return;
    }
    start_turn(text);
}

void App::start_turn(const std::string& text_in) {
    std::string text = take_dropped_image(text_in);
    auto pics = agent_.pending_images();
    view_.append(Kind::User, text + (pics.empty() ? "" : "\n(with " + std::to_string(pics.size()) + " image" + (pics.size() == 1 ? "" : "s") + ")"));
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    cancel_ = false;
    worker_ = std::thread([this, text] {
        std::string next = text;
        for (;;) {
            auto t0 = std::chrono::steady_clock::now();
            tool_calls_ = 0;
            fire("MaicTurnStart", {{"model", agent_.model}});
            try {
                agent_.submit(next, Origin::Local, *this, cancel_);
            } catch (const std::exception& ex) {
                view_.append(Kind::Error, failure_text(agent_, ex));
            }
            // The footer: model, how long the turn took, how many tools ran.
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char dur[32];
            if (secs < 1) snprintf(dur, sizeof(dur), "%.0fms", secs * 1000);
            else if (secs < 60) snprintf(dur, sizeof(dur), "%.1fs", secs);
            else if (secs < 3600) snprintf(dur, sizeof(dur), "%dm %ds", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
            else snprintf(dur, sizeof(dur), "%dh %dm", static_cast<int>(secs) / 3600, (static_cast<int>(secs) % 3600) / 60);
            int calls = tool_calls_.load();
            view_.append(Kind::Notice, "▣ " + agent_.model + " · " + dur + (calls ? " · " + std::to_string(calls) + (calls == 1 ? " tool call" : " tool calls") : "") +
                                           (cancel_.load() ? " · interrupted" : ""));
            fire("MaicTurnEnd", {{"model", agent_.model}, {"tool_calls", calls}, {"seconds", secs}, {"interrupted", cancel_.load()}});
            maybe_title(next);
            // Messages queued after the turn's last model call start a new turn on their own.
            if (cancel_.load() || agent_.queued() == 0) break;
            next.clear();
            for (const auto& p : agent_.take_queued()) next += (next.empty() ? "" : "\n\n") + p;
        }
        busy_ = false;
        if (quit_when_idle_.load() && !cancel_.load()) {
            quit_when_idle_ = false;
            screen_.Post([this] { quit(); });
        }
        screen_.PostEvent(Event::Custom);
    });
}

void App::set_title(const std::string& title) {
    if (log_) log_->write("title", {{"text", title}});
    titled_ = true;
}

// After the first turn, ask a small model for a title when settings name one. A remote title model is never
// used for a local session, so nothing leaves the machine that would not have anyway.
void App::maybe_title(const std::string& first_prompt) {
    if (titled_ || settings_.small_model.empty() || cancel_.load()) return;
    titled_ = true;
    try {
        auto [provider, name] = resolve_model(agent_.providers, settings_.small_model);
        if (provider.remote() && !agent_.remote()) return;
        std::string t = generate_title(provider, name, first_prompt);
        if (t.empty()) return;
        if (log_) log_->write("title", {{"text", t}});
        post(Kind::Notice, "titled: " + t + "  (:rename changes it)");
    } catch (const std::exception&) {
    }
}

void App::run_shell(const std::string& command) {
    if (shell_busy_) {
        post(Kind::Error, "a shell command is still running; Ctrl-C stops it");
        return;
    }
    view_.append(Kind::Shell, command);
    view_.append(Kind::ToolOk, "");
    if (shell_thread_.joinable()) shell_thread_.join();
    shell_busy_ = true;
    shell_cancel_ = false;
    shell_thread_ = std::thread([this, command] {
        std::string output;
        int rc = run_user_shell(command, agent_.harness().workspace(), shell_cancel_, [&](std::string_view chunk) {
            output.append(chunk);
            view_.append_to_last(Kind::ToolOk, chunk);
            screen_.PostEvent(Event::Custom);
        });
        std::string tail = rc == 0 ? "" : "\n[exit code " + std::to_string(rc) + "]";
        if (!tail.empty()) view_.append_to_last(Kind::ToolOk, tail);
        if (output.size() > 32 * 1024) output = output.substr(0, 32 * 1024) + "\n[truncated]";
        std::string context = "[The user ran this in their shell: `" + command + "`]\n" + output + tail;
        if (busy_) agent_.post_message(context);
        else agent_.add_context(context);
        shell_busy_ = false;
        screen_.PostEvent(Event::Custom);
    });
}

// Opens the input in $VISUAL / $EDITOR (default nvim) as a markdown file and loads it back on exit.
void App::edit_externally() {
    char path[] = "/tmp/maic-input-XXXXXX.md";
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

void App::run_lua(const std::string& code, bool from_file) {
    if (!lua_) lua_ = std::make_unique<Lua>(agent_.harness().workspace(), [this](const std::string& t) { post(Kind::Notice, t); });
    Lua::Result r;
    if (from_file) r = lua_->run_file(agent_.harness().resolve(code));
    else if (!code.empty() && code[0] == '=') r = lua_->run("return " + code.substr(1));
    else {
        // An expression shows its value; a statement runs as is.
        r = lua_->compiles("return " + code) ? lua_->run("return " + code) : lua_->run(code);
    }
    view_.append(Kind::Shell, (from_file ? "luafile " : "lua> ") + code);
    std::string out = r.output;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    view_.append(r.ok ? Kind::ToolOk : Kind::ToolErr, out.empty() ? "(no output)" : out);
    if (!r.output.empty()) {
        std::string context = "[The user ran Lua in MAIC: `" + code + "`]\n" + (r.output.size() > 32 * 1024 ? r.output.substr(0, 32 * 1024) + "\n[truncated]" : r.output);
        if (busy_) agent_.post_message(context);
        else agent_.add_context(context);
    }
}

void App::set_model(const std::string& model_in) {
    std::string preset = apply_preset(settings_, model_in);
    std::string model = resolve_model_alias(preset.empty() ? model_in : settings_.model);
    auto [provider, name] = resolve_model(agent_.providers, model);
    agent_.model = model;
    if (!preset.empty()) {
        agent_.providers = settings_.providers;
        agent_.think = settings_.think;
        set_context(agent_.providers, settings_.context);
        set_context(agent_.providers, settings_.context_2, "llamacpp-2");
        if (is_llama_server(provider.name)) {
            try {
                std::string r = restart_llamacpp_if_changed(provider.name);
                if (!r.empty()) post(Kind::Notice, r);
            } catch (const std::exception& e) {
                post(Kind::Error, e.what());
            }
        }
    }
    apply_sampling();
    ModelPick reviewer = agent_.reviewer().pick;
    std::string note = "model: " + model + " (" + provider.name + ", " + provider.kind + ")" +
                       (preset.empty() ? "" : "  preset " + preset + ": context " + std::to_string(settings_.providers.empty() ? 0 : provider.options.value("context_window", 0)) +
                                                  ", reviewer " + (reviewer.model.empty() ? "off" : reviewer.preset.empty() ? reviewer.model : reviewer.preset) + ", thinking " + (agent_.think ? "on" : "off"));
    if (provider.remote()) {
        post(Kind::Error, note + "\nREMOTE: prompts, files the agent reads and command output will be sent to " + provider.base_url);
    } else {
        post(Kind::Notice, note);
    }
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
    auto services = [&] { return load_services(root_dir() / "services"); };
    auto idle = [&] {
        if (busy_) post(Kind::Error, ":" + cmd + " has to wait until the agent is idle");
        return !busy_;
    };
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
            quit();
        } else if (cmd == "w" || cmd == "write" || cmd == "send") {
            submit(editor_.text(), arg == "now" || arg == "!");
        } else if (cmd == "ww") {
            submit(editor_.text(), true);
        } else if (cmd == "e" || cmd == "edit") {
            if (arg.empty()) edit_externally();
            else open_file(agent_.harness().resolve(arg));
        } else if (cmd == "nvim" || cmd == "host") {
            nvim_command(arg);
        } else if (cmd == "h" || cmd == "help") {
            post(Kind::Notice, help_text(arg));
        } else if (cmd == "mode") {
            if (auto m = parse_mode(arg)) request_mode(*m);
            else post(Kind::Error, "modes: manual, auto-read, edit, auto, plan");
        } else if (cmd == "harness") {
            if (arg.empty()) {
                Agent::ReviewerInfo r = agent_.reviewer();
                std::string spent = ", " + std::to_string(r.tokens) + " tokens so far" + (settings_.reviewer_budget_tokens > 0 ? " of " + std::to_string(settings_.reviewer_budget_tokens) : "");
                post(Kind::Notice, !agent_.review_with_model ? "harness: dumb. The rule list alone decides; nothing reads the conversation. `:harness smart` brings the reviewer back."
                                   : r.pick.model.empty()
                                       ? "harness: smart, but the reviewer is off for this session (" + r.pick.reason + spent + "): every action it would review is asked."
                                       : "harness: smart. A model (" + r.pick.model + ", " + r.pick.reason + spent +
                                             ") reads the conversation and reviews every command or write the rules would allow without asking. `:harness dumb` turns that off.");
            } else if (arg == "smart") {
                agent_.review_with_model = true;
                post(Kind::Notice, "harness: smart (reviewer on)");
            } else if (arg == "dumb") {
                agent_.review_with_model = false;
                if (agent_.mode.load() == Mode::Auto && !dumb_auto_ok_) {
                    agent_.mode = Mode::Edit;
                    post(Kind::Notice, "harness: dumb. Dropped to edit mode until you confirm auto.");
                    request_mode(Mode::Auto);
                } else {
                    post(Kind::Notice, "harness: dumb (rules only)");
                }
            } else {
                post(Kind::Error, ":harness [smart|dumb]");
            }
        } else if (cmd == "model") {
            if (arg.empty()) {
                std::string list = "model: " + agent_.model + "\npresets (:model NAME):" + preset_lines(settings_);
                list += "\nproviders:";
                for (const auto& p : agent_.providers) list += "\n  " + p.name + "/<model>  (" + p.kind + ", " + p.base_url + (p.remote() ? ", REMOTE)" : ")");
                post(Kind::Notice, list);
            } else if (idle()) {
                set_model(arg);
            }
        } else if (cmd == "models") {
            auto [provider, name] = resolve_model(agent_.providers, agent_.model);
            std::string out = "models on " + provider.name + " (" + provider.base_url + "):";
            try {
                std::vector<std::string> names = list_openai_models(provider);
                for (const auto& m : names) out += "\n  " + provider.name + "/" + m + (provider.name + "/" + m == agent_.model ? "   (in use)" : "");
                if (is_llama_server(provider.name)) {
                    out += "\nfiles under " + llamacpp_models_root().string() + " (a subdirectory holds a GGUF plus its mmproj); one model is resident per server; :model " + provider.name + "/NAME switches";
                }
            } catch (const std::exception& e) {
                out += "\n  " + std::string(e.what());
            }
            post(Kind::Notice, out);
        } else if (cmd == "think") {
            if (idle()) agent_.think = arg != "off", post(Kind::Notice, agent_.think ? "thinking on (slower, better on hard problems)" : "thinking off");
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
        } else if (cmd == "undo") {
            if (idle()) {
                post(Kind::Notice, agent_.undo(arg.empty() ? 1 : static_cast<size_t>(std::max(1, std::atoi(arg.c_str())))));
                if (host_up()) host_checktime(*host_);
            }
        } else if (cmd == "copy") {
            std::string last = view_.last_assistant();
            if (last.empty()) post(Kind::Error, "nothing to copy yet");
            else {
                register_ = last;
                post(Kind::Notice, "copied the last reply (" + copy_to_clipboard(last) + ")");
            }
        } else if (cmd == "export") {
            auto info = find_session(log_->path().stem().string());
            std::filesystem::path out = arg.empty() ? agent_.harness().workspace() / (log_->path().stem().string() + ".md") : agent_.harness().resolve(arg);
            if (!info) post(Kind::Error, "this session is not listed (temporary transcripts cannot be exported by id yet)");
            else {
                std::ofstream f(out);
                f << export_markdown(*info, load_session(log_->path()));
                post(Kind::Notice, "exported to " + out.string());
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
        } else if (cmd == "rename" || cmd == "title") {
            if (arg.empty()) post(Kind::Error, ":rename TITLE");
            else {
                set_title(arg);
                post(Kind::Notice, "titled: " + arg);
            }
        } else if (cmd == "budget") {
            if (arg == "off" || arg == "0") agent_.budget_tokens = 0, post(Kind::Notice, "no token budget");
            else if (!arg.empty()) agent_.budget_tokens = std::atol(arg.c_str()), post(Kind::Notice, "token budget: " + std::to_string(agent_.budget_tokens));
            else {
                auto u = agent_.usage();
                post(Kind::Notice, "used " + std::to_string(u.total_input + u.total_output) + " tokens this session" +
                                       (agent_.budget_tokens ? " of " + std::to_string(agent_.budget_tokens) : " (no budget; :budget N sets one)"));
            }
        } else if (cmd == "chat") {
            lua_mode_ = false;
            post(Kind::Notice, "back to the model");
        } else if (cmd == "compact") {
            if (idle()) {
                std::atomic<bool> no{false};
                if (arg == "all") post(Kind::Notice, agent_.compact(Agent::Compaction::All, no));
                else if (arg == "head") post(Kind::Notice, agent_.compact(Agent::Compaction::Head, no));
                else if (arg == "prune") post(Kind::Notice, agent_.compact(Agent::Compaction::Prune, no));
                else post(Kind::Notice, agent_.compact_auto(no));
            }
        } else if (cmd == "clear") {
            if (idle()) {
                agent_.clear();
                view_.clear();
            }
        } else if (cmd == "trip") {
            trip_tripwire("manual trip: " + (arg.empty() ? std::string("from the agent session") : arg));
            post(Kind::Error, settings_.tripwire == "session" ? "SESSION TRIPPED. Nothing runs in this session until :unlock (no sudo: the lock is this session's own)"
                                                                : "HARNESS TRIPPED. Nothing will run until :unlock");
        } else if (cmd == "unlock") {
            if (!tripwire_state()) post(Kind::Notice, "harness is not tripped");
            else if (session_tripped()) {
                post(unlock_session() ? Kind::Notice : Kind::Error, unlock_session() ? "session lock removed; carry on" : "session lock removed, but the machine lock is set: `maic unlock` (sudo)");
            } else {
                screen_.WithRestoredIO([] {
                    [[maybe_unused]] int rc = std::system("echo 'Unlocking the MAIC harness.'; sudo -k && sudo /usr/local/sbin/maic-lock reset");
                })();
                post(Kind::Notice, tripwire_state() ? "still tripped" : "harness unlocked; carry on");
            }
        } else if (cmd == "status") {
            auto [provider, name] = resolve_model(agent_.providers, agent_.model);
            std::string out = format_status(status_report(services()));
            out += "model: " + name + " via " + provider.name + " at " + provider.base_url + (provider.remote() ? "  [REMOTE: data leaves this machine]" : "  [local]") + "\n";
            out += "session: " + log_path() + "\n";
            out += "mode: " + std::string(mode_name(agent_.mode.load())) + (busy_ ? "  (working)" : "  (idle)");
            if (size_t q = agent_.queued()) out += "  " + std::to_string(q) + " queued  -> :w now";
            if (!agent_.tools().empty() || !agent_.script_tools().empty()) {
                out += "\ntools:";
                for (const auto& t : agent_.tools()) out += " " + t.name;
                for (const auto& t : agent_.script_tools()) out += " " + t.name;
            }
            if (std::string todo = todo_text(); !todo.empty()) out += "\n" + todo;
            post(Kind::Notice, out);
        } else if (cmd == "todo") {
            std::string todo = todo_text();
            post(Kind::Notice, todo.empty() ? "no plan yet: the agent keeps one with the todo tool during multi-step work" : todo);
        } else if (cmd == "tools") {
            std::string out = "built-in tools (all through the harness):";
            for (const auto& t : tool_schemas()) {
                std::string desc = t["function"].value("description", "");
                if (auto nl = desc.find('\n'); nl != std::string::npos) desc = desc.substr(0, nl);
                if (desc.size() > 90) desc = desc.substr(0, 87) + "...";
                out += "\n  " + t["function"].value("name", "") + "  " + desc;
            }
            out += "\nhelpers: maic-workflow-edit, maic-storyboard, maic-danbooru-tags, maic-panel-check (run_shell; allow-listed)";
            if (agent_.tools().empty() && agent_.script_tools().empty()) out += "\nno user-defined tools. Put a <name>.lua or a <name>/tool.json in .maic/tools/ or " + global_tools_dir().string() + " (see :h tools)";
            for (const auto& t : agent_.tools()) out += "\n  " + t.name + "  (lua)  " + t.file.string() + "\n    " + t.description;
            auto globs = [](const std::vector<std::string>& g) {
                std::string s;
                for (const auto& x : g) s += (s.empty() ? "" : ", ") + x;
                return s.empty() ? "nothing" : s;
            };
            for (const auto& t : agent_.script_tools()) {
                out += "\n  " + t.name + "  (" + script_tool_language(t) + ")  " + (t.dir / "tool.json").string() + "\n    " + t.description + "\n    reads " + globs(t.reads) + "; writes " + globs(t.writes);
            }
            for (const auto& n : agent_.tool_notices()) out += "\n  " + n;
            post(Kind::Notice, out);
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
        } else if (cmd == "settings") {
            std::string out = "settings files in effect (nearest last, wins):";
            for (const auto& p : settings_.sources) out += "\n  " + p.string();
            if (settings_.sources.empty()) out += "\n  none (defaults). `maic settings init` writes the global settings.lua; `:init` scaffolds a project's.";
            out += "\nsessions home: " + session_home_dir(settings_).lexically_relative(sessions_dir()).string() + "  (sessions_home = " + settings_.sessions_home + ")";
            post(Kind::Notice, out);
        } else if (cmd == "init") {
            std::filesystem::path ws = agent_.harness().workspace();
            std::string made = init_project(ws);
            post(Kind::Notice, made.empty() ? "already initialised: MAIC.md and .maic/settings.lua exist" : made);
            if (!std::filesystem::exists(ws / "MAIC.md") || std::filesystem::file_size(ws / "MAIC.md") < 200) {
                submit("Look over this project (list the top level, read the README and build files) and write a MAIC.md at the workspace root: "
                       "what the project is, how it is built and tested, the conventions to follow, and anything an agent should know before editing. "
                       "Keep it under 60 lines. Use write_file for MAIC.md only.", false);
            }
        } else if (cmd == "ban") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto& b = agent_.bans;
            if (sub.empty() || sub == "list") {
                std::string out = "banned strings (" + std::to_string(b.strings.size()) + "):";
                for (size_t i = 0; i < b.strings.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". \"" + b.strings[i] + "\"";
                out += "\nbanned patterns (" + std::to_string(b.patterns.size()) + ", POSIX extended regex, window " + std::to_string(b.window) + "):";
                for (size_t i = 0; i < b.patterns.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". /" + b.patterns[i] + "/";
                out += "\nbanned tokens (" + std::to_string(b.tokens.size()) + "):";
                for (size_t i = 0; i < b.tokens.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". " + b.tokens[i].dump();
                out += "\nretries " + std::to_string(b.retries) + ", then replaced by \"" + b.replacement + "\"" + (b.ignore_case ? ", case-insensitive" : "") +
                       "\n:ban add TEXT|@FILE · :ban pattern REGEX|@FILE · :ban token ID|TEXT|@FILE · :ban remove N · :ban patterns remove N · :ban tokens remove N · :ban clear · :ban retries N · :ban case on|off · :ban window N";
                post(Kind::Notice, out);
            } else if (sub == "pattern" && !rest.empty()) {
                std::vector<std::string> entries;
                try {
                    entries = expand_ban_entry(rest);
                } catch (const std::exception& e) {
                    post(Kind::Error, e.what());
                    return;
                }
                int added = 0;
                for (const auto& pat : entries) {
                    regex_t re;
                    int rc = regcomp(&re, pat.c_str(), REG_EXTENDED | (b.ignore_case ? REG_ICASE : 0));
                    if (rc != 0) {
                        char err[200];
                        regerror(rc, &re, err, sizeof(err));
                        post(Kind::Error, "not a valid POSIX extended regex: /" + pat + "/: " + err);
                        continue;
                    }
                    regfree(&re);
                    b.patterns.push_back(pat);
                    ++added;
                }
                if (added) post(Kind::Notice, added == 1 && entries.size() == 1 ? "banned /" + entries[0] + "/ (from the next model call)" : "banned " + std::to_string(added) + " patterns from " + rest.substr(1));
            } else if (sub == "patterns" && rest.rfind("remove ", 0) == 0) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str() + 7));
                if (n >= 1 && n <= b.patterns.size()) b.patterns.erase(b.patterns.begin() + static_cast<long>(n - 1)), post(Kind::Notice, "removed");
                else post(Kind::Error, "no banned pattern " + rest.substr(7));
            } else if (sub == "window" && !rest.empty()) {
                b.window = std::max(8, std::atoi(rest.c_str()));
                post(Kind::Notice, "regex hold-back window: " + std::to_string(b.window) + " characters");
            } else if ((sub == "add" || sub == "token") && !rest.empty()) {
                std::vector<std::string> entries;
                try {
                    entries = expand_ban_entry(rest);
                } catch (const std::exception& e) {
                    post(Kind::Error, e.what());
                    return;
                }
                for (const auto& e : entries) {
                    if (sub == "add") {
                        b.strings.push_back(e);
                        continue;
                    }
                    bool numeric = std::all_of(e.begin(), e.end(), [](unsigned char c) { return std::isdigit(c); });
                    b.tokens.push_back(numeric ? nlohmann::json(std::stoll(e)) : nlohmann::json(e));
                }
                if (entries.size() == 1 && rest[0] != '@') {
                    post(Kind::Notice, sub == "add" ? "banned \"" + rest + "\" (from the next model call)"
                                                    : "banned token " + rest + (b.tokens.back().is_number() ? " (logit_bias on OpenAI-compatible providers only)" : ""));
                } else {
                    post(Kind::Notice, "banned " + std::to_string(entries.size()) + (sub == "add" ? " phrases" : " tokens") + " from " + rest.substr(1));
                }
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= b.strings.size()) b.strings.erase(b.strings.begin() + static_cast<long>(n - 1)), post(Kind::Notice, "removed");
                else post(Kind::Error, "no banned string " + rest);
            } else if (sub == "tokens" && rest.rfind("remove ", 0) == 0) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str() + 7));
                if (n >= 1 && n <= b.tokens.size()) b.tokens.erase(b.tokens.begin() + static_cast<long>(n - 1)), post(Kind::Notice, "removed");
                else post(Kind::Error, "no banned token " + rest.substr(7));
            } else if (sub == "clear") {
                b.strings.clear();
                b.patterns.clear();
                b.tokens.clear();
                post(Kind::Notice, "bans cleared");
            } else if (sub == "retries" && !rest.empty()) {
                b.retries = std::max(0, std::atoi(rest.c_str()));
                post(Kind::Notice, "ban retries: " + std::to_string(b.retries));
            } else if (sub == "case") {
                b.ignore_case = rest == "off" || rest == "ignore";
                post(Kind::Notice, b.ignore_case ? "bans ignore case" : "bans match case");
            } else {
                post(Kind::Error, ":ban [list] · add TEXT · pattern REGEX · token ID|TEXT · remove N · patterns remove N · tokens remove N · clear · retries N · case on|off · window N");
            }
        } else if (cmd == "sampling" || cmd == "sampler") {
            std::istringstream a(arg);
            std::string key, v1, v2;
            a >> key >> v1 >> v2;
            auto [provider, mname] = resolve_model(agent_.providers, agent_.model);
            auto number = [](const std::string& s) {
                nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
                return j.is_number() ? j : nlohmann::json(s);
            };
            if (key.empty()) {
                std::string out = "sampling for " + agent_.model + " (" + provider.kind + "):";
                if (agent_.sampling.empty()) out += " defaults";
                for (const auto& [k, v] : agent_.sampling.items()) out += "\n  " + k + " = " + v.dump();
                out += "\n:sampling KEY VALUE · :sampling xtc P [T] · :sampling unset KEY · :sampling reset";
                if (provider.kind == "anthropic") out += "\nAnthropic's current models reject sampling parameters; nothing is sent there.";
                post(Kind::Notice, out);
            } else if (key == "xtc") {
                if (v1.empty()) post(Kind::Error, ":sampling xtc PROBABILITY [THRESHOLD]  (0.5 0.1 is a common start)");
                else {
                    live_sampling_["xtc_probability"] = number(v1);
                    live_sampling_["xtc_threshold"] = v2.empty() ? nlohmann::json(0.1) : number(v2);
                    apply_sampling();
                    post(Kind::Notice, "XTC: probability " + live_sampling_["xtc_probability"].dump() + ", threshold " + live_sampling_["xtc_threshold"].dump() +
                                           (provider.kind == "openai" ? " (sent as xtc_probability / xtc_threshold; llama.cpp server, koboldcpp and the like honour it)"
                                                                      : " (this provider has no XTC; the keys are kept for when you switch to a llama.cpp-style server)"));
                }
            } else if (key == "unset" && !v1.empty()) {
                live_sampling_.erase(v1);
                live_sampling_[v1] = nullptr;  // masks a settings value
                apply_sampling();
                agent_.sampling.erase(v1);
                post(Kind::Notice, "unset " + v1);
            } else if (key == "reset") {
                live_sampling_ = nlohmann::json::object();
                apply_sampling();
                post(Kind::Notice, "sampling back to the settings");
            } else if (!v1.empty()) {
                live_sampling_[key] = number(v1);
                apply_sampling();
                post(Kind::Notice, key + " = " + live_sampling_[key].dump() + (provider.kind == "anthropic" ? " (not sent to Anthropic)" : ""));
            } else {
                post(Kind::Error, ":sampling [KEY VALUE | xtc P [T] | unset KEY | reset]");
            }
        } else if (cmd == "image" || cmd == "img") {
            if (arg.empty()) {
                auto pics = agent_.pending_images();
                std::string out = pics.empty() ? "no image attached. :image FILE attaches one to the next message; a file dropped onto the terminal is attached on send" : "attached to the next message:";
                for (const auto& p : pics) out += "\n  " + p;
                post(Kind::Notice, out);
            } else if (arg == "clear") {
                agent_.clear_pending_images();
                post(Kind::Notice, "attachments dropped");
            } else {
                attach_image(arg);
            }
        } else if (cmd == "forbid") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto fb = agent_.harness().forbid();
            if (arg.empty() || arg == "list") {
                std::string out = "forbidden terms (" + std::to_string(fb.size()) + "; any tool call containing one is halted, in every mode, under every harness):";
                for (size_t i = 0; i < fb.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". " + fb[i];
                out += "\n:forbid TERM adds one · :forbid remove N   (forbid = { ... } in settings keeps them)";
                post(Kind::Notice, out);
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= fb.size()) fb.erase(fb.begin() + static_cast<long>(n - 1)), agent_.set_forbid(fb), post(Kind::Notice, "removed");
                else post(Kind::Error, "no term " + rest);
            } else {
                fb.push_back(arg);
                agent_.set_forbid(fb);
                post(Kind::Notice, "forbidden: " + arg);
            }
        } else if (cmd == "allow") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto al = agent_.harness().allow();
            if (arg.empty() || arg == "list") {
                std::string out = "allowed command patterns (" + std::to_string(al.size()) + "; no asking, no review, trip patterns still win):";
                for (size_t i = 0; i < al.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". " + al[i];
                out += "\n:allow PATTERN adds one (glob over the whole command, e.g. `pytest *`) · :allow remove N";
                post(Kind::Notice, out);
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= al.size()) al.erase(al.begin() + static_cast<long>(n - 1)), agent_.set_allow(al), post(Kind::Notice, "removed");
                else post(Kind::Error, "no pattern " + rest);
            } else {
                al.push_back(arg);
                agent_.set_allow(al);
                post(Kind::Notice, "allowed: " + arg + " (this session; put it under `allow` in settings to keep it)");
            }
        } else if (cmd == "rule" || cmd == "rules") {
            std::istringstream a(arg);
            std::string sub;
            a >> sub;
            std::string rest;
            std::getline(a >> std::ws, rest);
            auto rs = agent_.rules;
            if (arg.empty() || arg == "list") {
                std::string out = "standing rules (" + std::to_string(rs.size()) + "):";
                for (size_t i = 0; i < rs.size(); ++i) out += "\n  " + std::to_string(i + 1) + ". " + rs[i];
                out += "\n:rule TEXT adds one · :rule remove N · :rule clear   (a rule is a request the model is reminded of every turn; :prefix gives the literal first words)";
                post(Kind::Notice, out);
            } else if (!idle()) {
                post(Kind::Error, "wait for the turn to finish");
            } else if (sub == "remove" && !rest.empty()) {
                size_t n = static_cast<size_t>(std::atoi(rest.c_str()));
                if (n >= 1 && n <= rs.size()) {
                    rs.erase(rs.begin() + static_cast<long>(n - 1));
                    agent_.set_rules(rs);
                    post(Kind::Notice, "rule removed");
                } else post(Kind::Error, "no rule " + rest);
            } else if (sub == "clear") {
                agent_.set_rules({});
                post(Kind::Notice, "rules cleared");
            } else {
                rs.push_back(arg);
                agent_.set_rules(rs);
                post(Kind::Notice, "rule " + std::to_string(rs.size()) + " added; it is carried with the operator instructions from the next turn");
            }
        } else if (cmd == "ctx" || cmd == "context-size" || cmd == "ctx2") {
            bool side = cmd == "ctx2";
            int& current = side ? settings_.context_2 : settings_.context;
            std::string service = side ? "llamacpp-2" : "llamacpp";
            if (arg.empty()) {
                post(Kind::Notice, (side ? "side server context window: " : "context window: ") + std::to_string(current) + " tokens (:" + cmd + " N sets it and restarts " + service + "; --" + cmd + " N on the command line; " +
                                       (side ? "context_2" : "context") + " in settings)");
            } else if (!idle()) {
                post(Kind::Error, "wait for the turn to finish");
            } else {
                int n = std::atoi(arg.c_str());
                if (n < 1024) {
                    post(Kind::Error, ":" + cmd + " N takes tokens (8192, 16384, 32768, ...)");
                } else {
                    current = n;
                    set_context(agent_.providers, n, service);
                    std::string r;
                    try {
                        require_armed("restart services");
                        r = restart_llamacpp_if_changed(service);
                    } catch (const std::exception& e) {
                        r = e.what();
                    }
                    post(Kind::Notice, (side ? "side server context window: " : "context window: ") + std::to_string(n) + " tokens" + (r.empty() ? " (" + service + " was not running with another size)" : "; " + r));
                }
            }
        } else if (cmd == "prefill" || cmd == "prefix") {
            if (arg == "off" || arg == "none") agent_.prefill.clear(), post(Kind::Notice, "no prefill");
            else if (!arg.empty()) {
                agent_.prefill = resolve_system_prompt(arg);
                post(Kind::Notice, "every reply now begins with these literal words: \"" + agent_.prefill + "\"  (a rule such as \"always start with X\" belongs in :system; here you give X itself)");
            } else {
                post(Kind::Notice, agent_.prefill.empty() ? "no prefill (:prefill TEXT makes every reply start with TEXT; :prefill off clears)" : "replies start with: " + agent_.prefill);
            }
        } else if (cmd == "system") {
            if (arg == "off" && idle()) {
                agent_.set_system_prefix("");
                post(Kind::Notice, "operator instructions withdrawn");
            } else if (!arg.empty() && idle()) {
                agent_.set_system_prefix(resolve_system_prompt(arg));
                post(Kind::Notice, "operator instructions set: they now lead the system prompt and close each of your messages as the model sees them");
            } else if (arg.empty()) {
                post(Kind::Notice, agent_.system_prefix.empty() ? "no operator instructions (:system TEXT or :system @file sets them; --system on the command line)"
                                                                 : "operator instructions (placed first in the system prompt):\n" + agent_.system_prefix);
            }
        } else if (cmd == "instructions") {
            agent_.reload_instructions();
            if (!agent_.load_instruction_files) {
                post(Kind::Notice, "instruction files are disabled for this session (--no-instructions or load_instructions = false); :instructions on enables them");
                if (arg == "on") agent_.load_instruction_files = true, agent_.reload_instructions(), post(Kind::Notice, "instruction files enabled");
                return;
            }
            if (arg == "off") {
                agent_.load_instruction_files = false;
                agent_.reload_instructions();
                post(Kind::Notice, "instruction files disabled for the next turns");
                return;
            }
            std::string out = "instruction files in effect (re-read every turn):";
            for (const auto& f : agent_.instructions()) out += "\n  " + f.path.string() + "  (" + std::to_string(f.text.size()) + " bytes)";
            if (agent_.instructions().empty()) out += "\n  none. Create " + global_instructions_path().string() + " or a MAIC.md / AGENTS.md in the workspace.";
            post(Kind::Notice, out);
        } else if (cmd == "session") {
            post(Kind::Notice, "this session: " + log_path() + (settings_.record ? "\nhome: " + log_->path().parent_path().lexically_relative(sessions_dir()).string() +
                                   "  (maic sessions rehome " + log_->path().stem().string() + " project|general|NAME moves it)" : "\nnot kept: it lives in the runtime directory and is gone at logout") + "\n"
                                   "all sessions: " + sessions_dir().string() + "\n`maic sessions` lists them, `maic artifacts` cleans");
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
            auto places = known_places(settings_, agent_.harness().workspace(), services(), log_->path());
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
                auto [cmdline, what] = open_command(name, settings_, agent_.harness().workspace(), services(), log_->path(), browser, folder);
                post(std::system(cmdline.c_str()) == 0 ? Kind::Notice : Kind::Error, "opened " + what);
            } catch (const std::exception& e) {
                post(Kind::Error, e.what());
            }
        } else if (cmd == "artifacts") {
            std::string out = "where MAIC and its services keep things:";
            for (const auto& a : list_artifacts(services())) {
                auto u = measure(a);
                out += "\n  " + a.owner + "/" + a.name + "  " + a.path.string() + "  " + human_bytes(u.bytes) + " in " + std::to_string(u.files) + " files";
            }
            out += "\nclean with: maic artifacts clean OWNER/NAME [--older-than DAYS]";
            post(Kind::Notice, out);
        } else if (cmd == "reg" || cmd == "register") {
            std::string out = register_.empty() ? "register is empty" : "register:\n" + register_;
            for (const auto& [name, r] : editor_.registers()) out += "\n\"" + std::string(1, name) + (r.linewise ? " (lines):\n" : ":\n") + r.text;
            post(Kind::Notice, out);
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

void App::quit() {
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
    if (host_) host_->set_handlers({});
    nvim_hl_.reset();
    cancel_ = true;
    shell_cancel_ = true;
    answer(Approval::No);
    {
        std::lock_guard lock(mu_);
        answer_question("");
    }
    if (worker_.joinable()) worker_.join();
    if (shell_thread_.joinable()) shell_thread_.join();
    if (theme_thread_.joinable()) theme_thread_.join();
    if (nvim_colors_thread_.joinable()) nvim_colors_thread_.join();
}

}  // namespace

int run_tui(const TuiOptions& options) {
    // The host nvim first, so the settings files' Lua can use maic.nvim (docs/nvim.md).
    std::string host_refused;
    std::shared_ptr<HostNvim> host = HostNvim::from_env(host_refused);
    set_lua_nvim_host(host);
    Settings settings = load_settings();
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
    if (settings.harness != "smart" && settings.harness != "dumb") {
        std::cerr << "maic: --harness must be smart or dumb\n";
        return 2;
    }
    if (settings.tripwire == "isolated" && !settings.allow_isolated) {
        std::cerr << "maic: tripwire = \"isolated\" (opting out of the machine lock) is not allowed: set allow_isolated = true in settings to permit it\n";
        return 2;
    }
    if (!parse_mode(settings.mode)) {
        fprintf(stderr, "maic: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    set_color_depth(settings.colors);
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(settings.mouse);
    std::string first = options.initial_prompt;
    if (first == "-") first.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    App app(screen, settings, options.resume, options.append, options.fork_at, host, host_refused);
    if (options.ctx) {
        // --ctx: the local server is restarted to match before the first message.
        std::string r = restart_llamacpp_if_changed();
        if (!r.empty()) app.startup_notice(r);
    }
    if (options.ctx2) {
        std::string r = restart_llamacpp_if_changed("llamacpp-2");
        if (!r.empty()) app.startup_notice(r);
    }
    app.welcome();
    app.attach_context(options.context);
    for (const auto& im : options.images) app.attach_image(im);
    if (!first.empty()) app.send(first);
    auto component = CatchEvent(Renderer([&] { return app.render(); }), [&](Event e) { return app.handle(e); });
    screen.Loop(component);
    set_lua_nvim_host(nullptr);
    // The way back, printed after the screen is restored: a temporary transcript lives in the runtime
    // directory and is never listed, so this is the only place its path is easy to find.
    std::cout << "transcript" << (settings.record ? "" : " (temporary; gone at logout)") << ": " << app.transcript_path() << "\n"
              << "resume it with: maic -r " << app.transcript_path() << "\n";
    if (!app.exit_note().empty()) std::cout << app.exit_note() << "\n";
    return 0;
}

}  // namespace maic
