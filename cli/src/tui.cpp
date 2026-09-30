#include "tui.hpp"

#include "commands.hpp"
#include "editor.hpp"
#include "maic/agent.hpp"
#include "maic/artifacts.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
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
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace maic {

namespace {

using namespace ftxui;

constexpr size_t kToolPreviewLines = 8;

std::string preview(const std::string& text) {
    std::istringstream in(text);
    std::string out, line;
    size_t n = 0, total = 0;
    while (std::getline(in, line)) {
        if (n < kToolPreviewLines) out += (n ? "\n" : "") + line, ++n;
        ++total;
    }
    if (total > n) out += "\n… +" + std::to_string(total - n) + " lines";
    return out;
}

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

struct PendingApproval {
    ApprovalRequest request;
    std::promise<Approval> answer;
};

enum class Focus { Input, Conversation };

class App : public AgentEvents {
public:
    App(ScreenInteractive& screen, Settings settings, const std::optional<std::filesystem::path>& resume, bool append)
        : screen_(screen), settings_(std::move(settings)),
          log_(!resume ? SessionLog("tui") : append ? SessionLog::reopen(*resume) : SessionLog::fork(*resume, count_records(*resume), "tui")),
          agent_(std::filesystem::current_path(), settings_.model), editor_(&register_), view_(&register_) {
        agent_.providers = settings_.providers;
        agent_.think = settings_.think;
        if (auto m = parse_mode(settings_.mode)) agent_.mode = *m;
        view_.set_markdown(settings_.markdown);
        if (resume) {
            LoadedSession old = load_session(*resume);
            for (const auto& t : old.transcript) {
                if (t.type == "user") view_.append(Kind::User, t.text);
                else if (t.type == "assistant") view_.append(Kind::Assistant, t.text);
                else if (t.type == "tool_call") view_.append(Kind::Tool, t.text);
                else if (t.type == "tool_result") view_.append(t.ok ? Kind::ToolOk : Kind::ToolErr, preview(t.text));
                else view_.append(Kind::Notice, t.text);
            }
            agent_.set_log(&log_);
            agent_.restore(std::move(old.messages));
            view_.append(Kind::Notice, "resumed session " + resume->stem().string() + " (" + std::to_string(old.transcript.size()) + " entries)" +
                                           (append ? ", continuing in the same file" : ", continuing in a new file that points at it"));
        } else {
            agent_.set_log(&log_);
        }
    }

    ~App() override { shutdown(); }

    void welcome();
    Element render();
    bool handle(Event e);

    // AgentEvents, from the worker thread.
    void on_text(std::string_view delta, bool thinking) override {
        view_.append_to_last(thinking ? Kind::Thinking : Kind::Assistant, delta);
        screen_.PostEvent(Event::Custom);
    }
    void on_tool_call(const std::string& summary) override { post(Kind::Tool, summary); }
    void on_tool_result(const std::string& text, bool ok) override { post(ok ? Kind::ToolOk : Kind::ToolErr, preview(text)); }
    void on_notice(const std::string& text) override { post(Kind::Notice, text); }
    Approval ask(const ApprovalRequest& request) override {
        std::future<Approval> answer;
        {
            std::lock_guard lock(mu_);
            approval_.emplace(PendingApproval{request, {}});
            answer = approval_->answer.get_future();
        }
        screen_.PostEvent(Event::Custom);
        return answer.get();
    }

private:
    void post(Kind k, std::string text) {
        view_.append(k, std::move(text));
        screen_.PostEvent(Event::Custom);
    }
    bool asking() {
        std::lock_guard lock(mu_);
        return approval_.has_value();
    }

    Element render_input(size_t width, int& rows);
    Element render_palette(int& rows);
    std::vector<std::string> palette_entries();  // what the palette lists for the current command line
    void complete_command();
    Element render_top_status();
    Element render_bottom_status();
    Element render_approval();

    bool handle_approval(const Event& e);
    void answer(Approval a);
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
    SessionLog log_;
    Agent agent_;
    std::string register_;
    Editor editor_;
    View view_;
    Focus focus_ = Focus::Input;

    std::mutex mu_;  // guards approval_
    std::optional<PendingApproval> approval_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;

    std::atomic<bool> shell_busy_{false};
    std::atomic<bool> shell_cancel_{false};
    std::thread shell_thread_;

    std::string status_msg_;
    bool ctrl_w_pending_ = false;
    bool ctrl_x_pending_ = false;
    size_t palette_sel_ = 0;
    std::string palette_for_;  // the command line the selection belongs to
    bool quit_armed_ = false;
    bool ctrl_c_is_key_ = false;
    int view_height_ = 10;
};

void App::welcome() {
    std::string remote = agent_.remote() ? "  ·  REMOTE" : "  ·  local";
    view_.append(Kind::Notice, "MAIC  ·  workspace " + agent_.harness().workspace().string() + "  ·  model " + agent_.model + remote);
    std::string files;
    for (const auto& f : agent_.instructions()) files += (files.empty() ? "" : ", ") + f.path.string();
    view_.append(Kind::Notice, "session transcript: " + log_.path().string() + (files.empty() ? "" : "\ninstructions: " + files));
    view_.append(Kind::Notice, "Press i to type, Alt+Enter (or :w) to send, Enter for a new line. Esc = normal mode: j/k scroll, u/Ctrl-R undo/redo, :e opens nvim, Ctrl-W k = conversation window, :help for everything.");
    if (agent_.remote()) view_.append(Kind::Error, "This model runs off this machine: prompts, files the agent reads and command output are sent to it.");
    try {
        for (const auto& s : load_services(root_dir() / "services")) {
            if (s.name == "ollama" && !agent_.remote() && service_status(s).state == ServiceState::Stopped) {
                view_.append(Kind::Error, "Ollama isn't running. Start it with :up ollama");
            }
        }
    } catch (const std::exception& e) {
        view_.append(Kind::Error, e.what());
    }
}

// ---------- rendering ----------

Element App::render_input(size_t width, int& rows) {
    bool insert = editor_.mode() == Editor::Mode::Insert;
    std::string pre = insert ? "❯ " : "│ ";
    size_t avail = width > 3 ? width - 2 : 1;
    auto lines = editor_.text().empty() ? std::vector<StyledLine>{{}} : markdown_lines(editor_.text());
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
    std::string cmd = line.substr(0, space), partial = line.substr(space + 1);
    auto matches = match_commands(cmd);
    return complete_argument(matches.empty() ? cmd : matches.front()->name, partial, ctx);
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
    right.push_back(text(" · ") | decorate(settings_.style("status_dim")));
    if (tripwire_state()) right.push_back(text("HARNESS TRIPPED") | decorate(settings_.style("harness_tripped")));
    else right.push_back(text("harness armed") | decorate(settings_.style("harness_armed")));
    if (size_t q = agent_.queued()) right.push_back(text(" · " + std::to_string(q) + " queued (:w now)") | decorate(settings_.style("notice")));
    if (busy_) right.push_back(text(" · working… ctrl-c interrupts") | decorate(settings_.style("notice")));
    if (shell_busy_) right.push_back(text(" · shell running") | decorate(settings_.style("shell")));
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
            case Editor::Mode::Normal: vim = " NORMAL "; break;
            case Editor::Mode::Visual: vim = " VISUAL ", style = "status_visual"; break;
            case Editor::Mode::VisualLine: vim = " V-LINE ", style = "status_visual"; break;
            case Editor::Mode::Command: break;
        }
    }
    Elements parts = {text(vim) | decorate(settings_.style(style)), text(" ")};
    std::string hint = view_.status_hint();
    if (!hint.empty()) parts.push_back(text(hint + " ") | decorate(settings_.style("status_dim")));
    if (!status_msg_.empty()) parts.push_back(text(status_msg_) | decorate(settings_.style("notice")));
    parts.push_back(filler());
    parts.push_back(text(focus_ == Focus::Conversation ? "Ctrl-W j: back to input · v/V select · y yank · / search  " : "Ctrl-W k: conversation · :help  ") |
                    decorate(settings_.style("status_dim")));
    return hbox(parts);
}

Element App::render_approval() {
    std::lock_guard lock(mu_);
    if (!approval_) return emptyElement();
    const auto& r = approval_->request;
    std::string key = r.tool == "run_shell" ? "this program" : "this file";
    return window(text(" approve? ") | bold,
                  vbox({
                      text(r.summary) | bold,
                      text("why asking: " + r.reason + (r.origin == Origin::Remote ? "  [REMOTE REQUEST]" : "")) | dim,
                      hbox({text("[y]") | bold | color(Color::Green), text(" yes   "), text("[n]") | bold | color(Color::Red), text(" no   "),
                            text("[a]") | bold, text(" always allow " + key + " this session   "), text("[t]") | bold | color(Color::RedLight),
                            text(" trip the harness")}),
                  })) |
           decorate(settings_.style("approval"));
}

Element App::render() {
    // FTXUI leaves ISIG on, so Ctrl-C would be a SIGINT that tears the UI down. Make it a key instead.
    // This runs on the first frame, after FTXUI has set up the terminal; it restores the saved settings on exit.
    if (!ctrl_c_is_key_) {
        termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_lflag &= ~static_cast<tcflag_t>(ISIG);
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        ctrl_c_is_key_ = true;
    }
    auto size = Terminal::Size();
    size_t width = static_cast<size_t>(std::max(size.dimx, 20));
    int input_rows = 1;
    Element input = render_input(width, input_rows);
    int palette_rows = 0;
    Element palette = render_palette(palette_rows);
    bool is_asking = asking();
    bool focused = focus_ == Focus::Conversation;
    view_height_ = std::max(1, size.dimy - input_rows - palette_rows - 3 - (is_asking ? 5 : 0) - (focused ? 2 : 0));
    Element conversation = view_.render(settings_, focused ? width - 2 : width, view_height_);
    if (focused) conversation = conversation | borderLight | decorate(settings_.style("focus"));
    return vbox({conversation, render_approval(), render_top_status(), separator() | decorate(settings_.style("separator")), input, palette,
                 render_bottom_status()});
}

// ---------- keys ----------

bool App::handle(Event e) {
    if (e == Event::Custom) return true;

    // A fast Esc followed by a key arrives as one Alt-sequence ("\x1b:"). Vim users type that constantly,
    // so split it back into Esc plus the keys. Real escape sequences (CSI "\x1b[", SS3 "\x1bO") pass through.
    const std::string& raw = e.input();
    // Alt+Enter sends (nvim has no default Alt mappings, so nothing is lost). Terminals send it as Esc + Enter.
    if (raw == "\x1b\r" || raw == "\x1b\n") {
        if (!asking()) submit(editor_.text(), false);
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
            if (focus_ == Focus::Input && editor_.mode() == Editor::Mode::Insert) editor_.history_step(-dir);
            else view_.scroll_by(dir * 3);
        }
        return true;
    }
    if (raw != "\x03") quit_armed_ = false;
    status_msg_.clear();
    if (asking()) return handle_approval(e);

    if (raw == "\x03") {  // Ctrl-C: interrupt, then clear input, then quit
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
        agent_.mode = next_mode(agent_.mode.load());
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
        auto r = editor_.handle(e);
        if (r.action == Editor::Action::Command) run_command(r.text);
        if (r.action == Editor::Action::Search) {
            view_.search(r.text);
            set_focus(Focus::Conversation);
            status_msg_ = view_.search_next(1);
        }
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
    auto r = editor_.handle(e);
    if (r.action == Editor::Action::Command) run_command(r.text);
    else if (r.action == Editor::Action::Search) {
        view_.search(r.text);
        set_focus(Focus::Conversation);
        status_msg_ = view_.search_next(1);
    }
    return true;
}

bool App::handle_approval(const Event& e) {
    const std::string& k = e.input();
    if (k == "y" || k == "Y") answer(Approval::Yes);
    else if (k == "n" || k == "N" || e == Event::Escape) answer(Approval::No);
    else if (k == "a" || k == "A") answer(Approval::Always);
    else if (k == "t" || k == "T") answer(Approval::Trip);
    else if (k == "\x03") answer(Approval::No), cancel_ = true;
    return true;
}

void App::answer(Approval a) {
    std::lock_guard lock(mu_);
    if (approval_) {
        approval_->answer.set_value(a);
        approval_.reset();
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

void App::start_turn(const std::string& text) {
    view_.append(Kind::User, text);
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    cancel_ = false;
    worker_ = std::thread([this, text] {
        std::string next = text;
        for (;;) {
            try {
                agent_.submit(next, Origin::Local, *this, cancel_);
            } catch (const std::exception& ex) {
                view_.append(Kind::Error, ex.what());
            }
            // Messages queued after the turn's last model call start a new turn on their own.
            if (cancel_.load() || agent_.queued() == 0) break;
            next.clear();
            for (const auto& p : agent_.take_queued()) next += (next.empty() ? "" : "\n\n") + p;
        }
        busy_ = false;
        screen_.PostEvent(Event::Custom);
    });
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

void App::set_model(const std::string& model) {
    auto [provider, name] = resolve_model(agent_.providers, model);
    agent_.model = model;
    std::string note = "model: " + model + " (" + provider.name + ", " + provider.kind + ")";
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
        if (cmd == "q" || cmd == "q!" || cmd == "quit" || cmd == "wq" || cmd == "exit") {
            quit();
        } else if (cmd == "w" || cmd == "write" || cmd == "send") {
            submit(editor_.text(), arg == "now" || arg == "!");
        } else if (cmd == "e" || cmd == "edit" || cmd == "nvim") {
            edit_externally();
        } else if (cmd == "h" || cmd == "help") {
            post(Kind::Notice, help_text(arg));
        } else if (cmd == "mode") {
            if (auto m = parse_mode(arg)) agent_.mode = *m;
            else post(Kind::Error, "modes: manual, auto-read, edit, auto, plan");
        } else if (cmd == "model") {
            if (arg.empty()) {
                std::string list = "model: " + agent_.model + "\nproviders:";
                for (const auto& p : agent_.providers) list += "\n  " + p.name + "/<model>  (" + p.kind + ", " + p.base_url + (p.remote() ? ", REMOTE)" : ")");
                post(Kind::Notice, list);
            } else if (idle()) {
                set_model(arg);
            }
        } else if (cmd == "models") {
            auto [provider, name] = resolve_model(agent_.providers, agent_.model);
            if (provider.kind != "ollama") post(Kind::Notice, "listing is only available for Ollama providers");
            else {
                std::string out = "models on " + provider.name + ":";
                for (const auto& m : list_ollama_models(provider)) out += "\n  " + m;
                post(Kind::Notice, out);
            }
        } else if (cmd == "think") {
            if (idle()) agent_.think = arg != "off", post(Kind::Notice, agent_.think ? "thinking on (slower, better on hard problems)" : "thinking off");
        } else if (cmd == "set") {
            std::istringstream a(arg);
            std::string key, value;
            a >> key >> value;
            bool on = value != "off" && value != "false" && value != "0";
            if (key == "markdown" || key == "md") view_.set_markdown(on), post(Kind::Notice, on ? "markdown rendering on" : "markdown rendering off (raw text)");
            else if (key == "mouse") settings_.mouse = on, screen_.TrackMouse(on), post(Kind::Notice, on ? "mouse on (Shift+drag selects text in the terminal)" : "mouse off");
            else post(Kind::Error, ":set markdown|mouse on|off");
        } else if (cmd == "clear") {
            if (idle()) {
                agent_.clear();
                view_.clear();
            }
        } else if (cmd == "trip") {
            trip_tripwire("manual trip: " + (arg.empty() ? std::string("from the agent session") : arg));
            post(Kind::Error, "HARNESS TRIPPED. Nothing will run until :unlock");
        } else if (cmd == "unlock") {
            if (!tripwire_state()) post(Kind::Notice, "harness is not tripped");
            else {
                screen_.WithRestoredIO([] {
                    [[maybe_unused]] int rc = std::system("echo 'Unlocking the MAIC harness.'; sudo -k && sudo /usr/local/sbin/maic-lock reset");
                })();
                post(Kind::Notice, tripwire_state() ? "still tripped" : "harness unlocked; carry on");
            }
        } else if (cmd == "status") {
            auto [provider, name] = resolve_model(agent_.providers, agent_.model);
            std::string out = format_status(status_report(services()));
            out += "model: " + name + " via " + provider.name + " at " + provider.base_url + (provider.remote() ? "  [REMOTE: data leaves this machine]" : "  [local]") + "\n";
            out += "session: " + log_.path().string() + "\n";
            out += "mode: " + std::string(mode_name(agent_.mode.load())) + (busy_ ? "  (working)" : "  (idle)");
            if (size_t q = agent_.queued()) out += "  " + std::to_string(q) + " queued  -> :w now";
            post(Kind::Notice, out);
        } else if (cmd == "up" || cmd == "down") {
            bool found = false;
            for (const auto& s : services()) {
                if (s.name != arg) continue;
                found = true;
                if (cmd == "up") {
                    require_armed("start services");
                    post(Kind::Notice, "starting " + s.name + "…");
                    post(Kind::Notice, start_service(s) ? s.name + " is ready" : s.name + " is still starting");
                } else {
                    stop_service(s);
                    post(Kind::Notice, s.name + " stopped");
                }
            }
            if (!found) post(Kind::Error, "unknown service: " + arg + (arg.empty() ? " (:up NAME)" : " (see :status)"));
        } else if (cmd == "instructions") {
            agent_.reload_instructions();
            std::string out = "instruction files in effect (re-read every turn):";
            for (const auto& f : agent_.instructions()) out += "\n  " + f.path.string() + "  (" + std::to_string(f.text.size()) + " bytes)";
            if (agent_.instructions().empty()) out += "\n  none. Create " + global_instructions_path().string() + " or a MAIC.md / AGENTS.md in the workspace.";
            post(Kind::Notice, out);
        } else if (cmd == "session") {
            post(Kind::Notice, "this session: " + log_.path().string() + "\nall sessions: " + sessions_dir().string() + "\n`maic artifacts` lists and cleans them");
        } else if (cmd == "artifacts") {
            std::string out = "where MAIC and its services keep things:";
            for (const auto& a : list_artifacts(services())) {
                auto u = measure(a);
                out += "\n  " + a.owner + "/" + a.name + "  " + a.path.string() + "  " + human_bytes(u.bytes) + " in " + std::to_string(u.files) + " files";
            }
            out += "\nclean with: maic artifacts clean OWNER/NAME [--older-than DAYS]";
            post(Kind::Notice, out);
        } else if (cmd == "reg" || cmd == "register") {
            post(Kind::Notice, register_.empty() ? "register is empty" : "register:\n" + register_);
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
    shutdown();
    screen_.Exit();
}

void App::shutdown() {
    cancel_ = true;
    shell_cancel_ = true;
    answer(Approval::No);
    if (worker_.joinable()) worker_.join();
    if (shell_thread_.joinable()) shell_thread_.join();
}

}  // namespace

int run_tui(const TuiOptions& options) {
    Settings settings = load_settings();
    if (options.model) settings.model = *options.model;
    if (options.mode) settings.mode = *options.mode;
    if (!parse_mode(settings.mode)) {
        fprintf(stderr, "maic: unknown mode '%s' (manual, auto-read, edit, auto, plan)\n", settings.mode.c_str());
        return 2;
    }
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(settings.mouse);
    App app(screen, settings, options.resume, options.append);
    app.welcome();
    auto component = CatchEvent(Renderer([&] { return app.render(); }), [&](Event e) { return app.handle(e); });
    screen.Loop(component);
    return 0;
}

}  // namespace maic
