#include "tui.hpp"

#include "maic/agent.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/tripwire.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <termios.h>
#include <unistd.h>
#include <cctype>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace maic {

namespace {

using namespace ftxui;

// ---------- UTF-8 helpers (cursor positions are byte offsets on code point boundaries) ----------

size_t next_cp(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t prev_cp(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t cp_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Byte offset of the n-th code point.
size_t cp_offset(const std::string& s, size_t n) {
    size_t i = 0;
    while (n-- && i < s.size()) i = next_cp(s, i);
    return i;
}

// Soft wrap to `width` columns: break at the last space when it's not too far back, keep all other spacing.
std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> out;
    width = std::max<size_t>(width, 10);
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        for (size_t t; (t = line.find('\t')) != std::string::npos;) line.replace(t, 1, "    ");  // the terminal renderer drops tabs
        while (cp_len(line) > width) {
            size_t cut = cp_offset(line, width);
            size_t space = line.rfind(' ', cut);
            bool soft = space != std::string::npos && cp_len(line.substr(0, space)) > width / 2;
            size_t at = soft ? space : cut;
            out.push_back(line.substr(0, at));
            line.erase(0, soft ? at + 1 : at);
        }
        out.push_back(line);
    }
    if (out.empty()) out.push_back("");
    return out;
}

// vim word classes: 0 space, 1 word, 2 punctuation
int char_class(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n') return 0;
    if (c >= 0x80 || std::isalnum(c) || c == '_') return 1;
    return 2;
}

// ---------- transcript ----------

enum class Kind { User, Assistant, Thinking, Tool, ToolOk, ToolErr, Notice, Error };

struct Entry {
    Kind kind;
    std::string text;
};

struct Line {
    std::string text;
    Kind kind;
};

const char* prefix(Kind k) {
    switch (k) {
        case Kind::User: return "❯ ";
        case Kind::Assistant: return "● ";
        case Kind::Thinking: return "✻ ";
        case Kind::Tool: return "▸ ";
        case Kind::ToolOk:
        case Kind::ToolErr: return "  ⎿ ";
        case Kind::Notice: return "※ ";
        case Kind::Error: return "✗ ";
    }
    return "";
}

Decorator style(Kind k) {
    switch (k) {
        case Kind::User: return bold;
        case Kind::Assistant: return nothing;
        case Kind::Thinking: return dim;
        case Kind::Tool: return color(Color::Cyan);
        case Kind::ToolOk: return color(Color::GrayDark);
        case Kind::ToolErr: return color(Color::Red);
        case Kind::Notice: return color(Color::Yellow);
        case Kind::Error: return color(Color::RedLight);
    }
    return nothing;
}

constexpr size_t kToolPreviewLines = 8;

std::string preview(const std::string& text) {
    std::istringstream in(text);
    std::string out, line;
    size_t n = 0, total = 0;
    while (std::getline(in, line)) {
        if (n < kToolPreviewLines) {
            out += (n ? "\n" : "") + line;
            ++n;
        }
        ++total;
    }
    if (total > n) out += "\n… +" + std::to_string(total - n) + " lines";
    return out;
}

// ---------- the app ----------

enum class VimMode { Insert, Normal, Command };

struct PendingApproval {
    ApprovalRequest request;
    std::promise<Approval> answer;
};

class App : public AgentEvents {
public:
    App(ScreenInteractive& screen, std::string model)
        : screen_(screen), agent_(std::filesystem::current_path(), std::move(model)) {}

    ~App() override { shutdown(); }

    void welcome() {
        add(Kind::Notice, "MAIC  ·  workspace " + agent_.harness().workspace().string() + "  ·  model " + agent_.model);
        add(Kind::Notice, "Type and press Enter. Esc = normal mode (j/k scroll, :help for commands). Shift-Tab cycles modes.");
        try {
            for (const auto& s : load_services(root_dir() / "services")) {
                if (s.name == "ollama" && service_status(s).state == ServiceState::Stopped) {
                    add(Kind::Error, "Ollama isn't running. Start it with :up ollama");
                }
            }
        } catch (const std::exception& e) {
            add(Kind::Error, e.what());
        }
    }

    Element render();
    bool handle(Event e);

    // AgentEvents, called from the worker thread.
    void on_text(std::string_view delta, bool thinking) override {
        {
            std::lock_guard lock(mu_);
            Kind k = thinking ? Kind::Thinking : Kind::Assistant;
            if (entries_.empty() || entries_.back().kind != k) entries_.push_back({k, ""});
            entries_.back().text += delta;
        }
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
    void add(Kind k, std::string text) {
        std::lock_guard lock(mu_);
        entries_.push_back({k, std::move(text)});
    }
    void post(Kind k, std::string text) {
        add(k, std::move(text));
        screen_.PostEvent(Event::Custom);
    }

    std::vector<Line> layout(size_t width);
    Element render_input(size_t width, int& rows, bool asking);
    Element render_status();
    Element render_approval();

    bool handle_approval(const Event& e);
    bool handle_insert(const Event& e);
    bool handle_normal(const Event& e);
    bool handle_command(const Event& e);

    void submit();
    void run_command(const std::string& line);
    void answer(Approval a);
    void quit();
    void shutdown();
    void scroll_by(int lines);
    void history_step(int dir);
    void save_undo() { undo_ = {input_, cursor_}; }

    // input motions
    void word_forward();
    void word_backward();
    void word_end();
    void delete_range(size_t from, size_t to) {
        if (from > to) std::swap(from, to);
        input_.erase(from, to - from);
        cursor_ = std::min(from, input_.size());
    }
    void clamp_normal() {
        if (vim_ == VimMode::Normal && cursor_ >= input_.size() && !input_.empty()) cursor_ = prev_cp(input_, input_.size());
    }

    ScreenInteractive& screen_;
    Agent agent_;

    std::mutex mu_;  // guards entries_ and approval_
    std::vector<Entry> entries_;
    std::optional<PendingApproval> approval_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;

    std::string input_;
    size_t cursor_ = 0;
    std::pair<std::string, size_t> undo_;
    VimMode vim_ = VimMode::Insert;
    std::string pending_;  // operator waiting for a motion: g, d, c
    int count_ = 0;
    std::string cmdline_;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    std::string history_draft_;

    int scroll_ = 0;  // lines up from the bottom; 0 follows new output
    size_t last_total_ = 0;
    int view_height_ = 10;
    bool quit_armed_ = false;
    bool ctrl_c_is_key_ = false;
};

std::vector<Line> App::layout(size_t width) {
    std::vector<Line> lines;
    std::lock_guard lock(mu_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        bool attached = e.kind == Kind::ToolOk || e.kind == Kind::ToolErr;
        if (i > 0 && !attached) lines.push_back({"", Kind::Assistant});
        std::string pre = prefix(e.kind);
        std::string indent(cp_len(pre), ' ');
        bool first = true;
        for (auto& w : wrap(e.text, width - cp_len(pre))) {
            lines.push_back({(first ? pre : indent) + w, e.kind});
            first = false;
        }
    }
    return lines;
}

Element App::render_input(size_t width, int& rows, bool asking) {
    std::string pre = vim_ == VimMode::Normal ? "│ " : "❯ ";
    size_t avail = width - 2;
    std::vector<std::string> row_text{""};
    size_t cur_row = 0, cur_col = 0;
    size_t col = 0;
    for (size_t i = 0;; i = next_cp(input_, i)) {
        if (i == cursor_) {
            cur_row = row_text.size() - 1;
            cur_col = col;
        }
        if (i >= input_.size()) break;
        std::string ch = input_.substr(i, next_cp(input_, i) - i);
        if (ch == "\n") {
            row_text.push_back("");
            col = 0;
            continue;
        }
        if (col == avail) {
            row_text.push_back("");
            col = 0;
            if (i == cursor_) cur_row = row_text.size() - 1, cur_col = 0;
        }
        row_text.back() += ch;
        ++col;
    }
    rows = static_cast<int>(row_text.size());

    Elements out;
    for (size_t r = 0; r < row_text.size(); ++r) {
        Element lead = text(r == 0 ? pre : "  ") | color(vim_ == VimMode::Insert ? Color::Green : Color::Blue);
        if (r != cur_row || vim_ == VimMode::Command || asking) {
            out.push_back(hbox({lead, text(row_text[r])}));
            continue;
        }
        const std::string& s = row_text[r];
        size_t a = cp_offset(s, cur_col), b = next_cp(s, a);
        std::string under = a < s.size() ? s.substr(a, b - a) : " ";
        Element c = text(under);
        c = vim_ == VimMode::Insert ? focusCursorBar(c) : focusCursorBlock(c | inverted);
        out.push_back(hbox({lead, text(s.substr(0, a)), c, text(a < s.size() ? s.substr(b) : "")}));
    }
    return vbox(out);
}

Element App::render_status() {
    if (vim_ == VimMode::Command) {
        return hbox({text(":" + cmdline_), text(" ") | focusCursorBar});
    }
    Mode m = agent_.mode.load();
    Color mode_color = m == Mode::Auto ? Color::Red : m == Mode::Edit ? Color::Yellow : m == Mode::Plan ? Color::Cyan : Color::Blue;
    auto lock = tripwire_state();
    Elements parts = {
        text(vim_ == VimMode::Insert ? " INSERT " : " NORMAL ") | bold | inverted |
            color(vim_ == VimMode::Insert ? Color::Green : Color::Blue),
        text(" "),
        text(std::string(mode_name(m))) | bold | color(mode_color),
        text(" (shift-tab) · ") | dim,
        text(agent_.model),
        text(agent_.think ? " +think" : "") | dim,
        text(" · ") | dim,
        lock ? text("HARNESS TRIPPED") | bold | color(Color::Red) : text("harness armed") | color(Color::Green),
        filler(),
    };
    if (busy_) parts.push_back(text("working… ctrl-c interrupts ") | color(Color::Yellow));
    if (scroll_ > 0) parts.push_back(text("↑" + std::to_string(scroll_) + " (G to follow) ") | dim);
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
                      hbox({text("[y]") | bold | color(Color::Green), text(" yes   "),
                            text("[n]") | bold | color(Color::Red), text(" no   "),
                            text("[a]") | bold, text(" always allow " + key + " this session   "),
                            text("[t]") | bold | color(Color::RedLight), text(" trip the harness")}),
                  })) |
           color(Color::Yellow);
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
    bool asking = false;
    {
        std::lock_guard lock(mu_);
        asking = approval_.has_value();
    }
    int input_rows = 1;
    Element input = render_input(width, input_rows, asking);
    Element approval = render_approval();
    view_height_ = std::max(1, size.dimy - input_rows - 2 - (asking ? 5 : 0));

    std::vector<Line> lines = layout(width);
    if (scroll_ > 0 && lines.size() > last_total_) scroll_ += static_cast<int>(lines.size() - last_total_);  // hold the view still
    last_total_ = lines.size();
    int max_scroll = std::max(0, static_cast<int>(lines.size()) - view_height_);
    scroll_ = std::clamp(scroll_, 0, max_scroll);

    int bottom = static_cast<int>(lines.size()) - scroll_;
    int top = std::max(0, bottom - view_height_);
    Elements rows;
    for (int i = bottom - top; i < view_height_; ++i) rows.push_back(text(""));
    for (int i = top; i < bottom; ++i) rows.push_back(text(lines[i].text) | style(lines[i].kind));

    return vbox({vbox(rows), approval, separator() | dim, input, render_status()});
}

// ---------- key handling ----------

bool App::handle(Event e) {
    if (e == Event::Custom) return true;

    // A fast Esc followed by a key arrives as one Alt-sequence ("\x1b:"). Vim users type that constantly,
    // so split it back into Esc plus the keys. Real escape sequences (CSI "\x1b[", SS3 "\x1bO") pass through.
    const std::string& raw = e.input();
    if (raw.size() >= 2 && raw[0] == '\x1b' && raw[1] != '[' && raw[1] != 'O') {
        handle(Event::Escape);
        for (size_t i = 1; i < raw.size(); i = next_cp(raw, i)) {
            handle(Event::Character(raw.substr(i, next_cp(raw, i) - i)));
        }
        return true;
    }
    if (e.input() != "\x03") quit_armed_ = false;

    bool asking;
    {
        std::lock_guard lock(mu_);
        asking = approval_.has_value();
    }
    if (asking) return handle_approval(e);

    if (e.input() == "\x03") {  // Ctrl-C: interrupt, then clear input, then quit
        if (busy_) {
            cancel_ = true;
        } else if (!input_.empty()) {
            input_.clear();
            cursor_ = 0;
        } else if (quit_armed_) {
            quit();
        } else {
            quit_armed_ = true;
            post(Kind::Notice, "press Ctrl-C again to quit (or :q)");
        }
        return true;
    }
    if (e == Event::TabReverse) {
        agent_.mode = next_mode(agent_.mode.load());
        return true;
    }
    if (e == Event::PageUp) return scroll_by(view_height_ - 2), true;
    if (e == Event::PageDown) return scroll_by(-(view_height_ - 2)), true;

    switch (vim_) {
        case VimMode::Insert: return handle_insert(e);
        case VimMode::Normal: return handle_normal(e);
        case VimMode::Command: return handle_command(e);
    }
    return false;
}

bool App::handle_approval(const Event& e) {
    const std::string& k = e.input();
    if (k == "y" || k == "Y") answer(Approval::Yes);
    else if (k == "n" || k == "N" || e == Event::Escape) answer(Approval::No);
    else if (k == "a" || k == "A") answer(Approval::Always);
    else if (k == "t" || k == "T") answer(Approval::Trip);
    else if (k == "\x03") { answer(Approval::No); cancel_ = true; }
    return true;
}

void App::answer(Approval a) {
    std::lock_guard lock(mu_);
    if (approval_) {
        approval_->answer.set_value(a);
        approval_.reset();
    }
}

bool App::handle_insert(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) {
        vim_ = VimMode::Normal;
        if (cursor_ > 0) cursor_ = prev_cp(input_, cursor_);
        clamp_normal();
        return true;
    }
    if (e == Event::Return) {
        // Backslash-Enter continues on a new line (terminals can't tell Shift-Enter or Ctrl-J from Enter).
        if (cursor_ > 0 && input_[cursor_ - 1] == '\\') {
            input_[cursor_ - 1] = '\n';
            return true;
        }
        return submit(), true;
    }
    if (e == Event::Backspace) {
        if (cursor_ > 0) delete_range(prev_cp(input_, cursor_), cursor_);
        return true;
    }
    if (e == Event::Delete) {
        delete_range(cursor_, next_cp(input_, cursor_));
        return true;
    }
    if (k == "\x17") {  // Ctrl-W
        size_t end = cursor_;
        word_backward();
        delete_range(cursor_, end);
        return true;
    }
    if (k == "\x15") {  // Ctrl-U
        delete_range(0, cursor_);
        return true;
    }
    if (e == Event::ArrowLeft) return cursor_ = prev_cp(input_, cursor_), true;
    if (e == Event::ArrowRight) return cursor_ = next_cp(input_, cursor_), true;
    if (e == Event::Home) return cursor_ = 0, true;
    if (e == Event::End) return cursor_ = input_.size(), true;
    if (e == Event::ArrowUp || k == "\x10") return history_step(-1), true;
    if (e == Event::ArrowDown || k == "\x0e") return history_step(1), true;
    if (e.is_character() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) {
        input_.insert(cursor_, k);
        cursor_ += k.size();
        return true;
    }
    return false;
}

bool App::handle_normal(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) {
        pending_.clear();
        count_ = 0;
        return true;
    }
    if (e == Event::Return) return submit(), true;

    // counts: 5j, 3w, ...
    if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
        count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
        return true;
    }
    int n = std::max(count_, 1);
    count_ = 0;

    // operators waiting for their second key
    if (!pending_.empty()) {
        std::string op = pending_;
        pending_.clear();
        if (op == "g" && k == "g") return scroll_ = std::numeric_limits<int>::max() / 2, true;
        if ((op == "d" && k == "d") || (op == "c" && k == "c")) {
            save_undo();
            input_.clear();
            cursor_ = 0;
            if (op == "c") vim_ = VimMode::Insert;
            return true;
        }
        if ((op == "d" || op == "c") && (k == "w" || k == "e" || k == "b" || k == "$" || k == "0")) {
            save_undo();
            // vim special case: cw on a word changes to the end of the word, like ce
            std::string motion = (op == "c" && k == "w" && cursor_ < input_.size() && char_class(input_[cursor_]) != 0) ? "e" : k;
            size_t start = cursor_;
            for (int i = 0; i < n; ++i) {
                if (motion == "w") word_forward();
                else if (motion == "e") word_end();
                else if (motion == "b") word_backward();
            }
            if (motion == "$") cursor_ = input_.size();
            if (motion == "0") cursor_ = 0;
            size_t end = (motion == "e") ? next_cp(input_, cursor_) : cursor_;
            delete_range(start, end);
            if (op == "c") vim_ = VimMode::Insert;
            clamp_normal();
            return true;
        }
        return true;  // unknown pair: drop it, like vim
    }

    auto enter_insert = [&](size_t at) {
        cursor_ = std::min(at, input_.size());
        vim_ = VimMode::Insert;
        return true;
    };

    // transcript navigation
    if (k == "j" || k == "\x05") return scroll_by(-n), true;          // j, Ctrl-E
    if (k == "k" || k == "\x19") return scroll_by(n), true;           // k, Ctrl-Y
    if (k == "\x04") return scroll_by(-view_height_ / 2), true;       // Ctrl-D
    if (k == "\x15") return scroll_by(view_height_ / 2), true;        // Ctrl-U
    if (k == "\x06") return scroll_by(-(view_height_ - 2)), true;     // Ctrl-F
    if (k == "\x02") return scroll_by(view_height_ - 2), true;        // Ctrl-B
    if (k == "G") return scroll_ = 0, true;
    if (k == "g" || k == "d" || k == "c") return pending_ = k, true;
    if (k == "\x10") return history_step(-1), true;                   // Ctrl-P
    if (k == "\x0e") return history_step(1), true;                    // Ctrl-N

    // input editing
    if (k == "i") return enter_insert(cursor_);
    if (k == "a") return enter_insert(next_cp(input_, cursor_));
    if (k == "I") return enter_insert(0);
    if (k == "A") return enter_insert(input_.size());
    if (k == "h" || e == Event::ArrowLeft) { for (int i = 0; i < n; ++i) cursor_ = prev_cp(input_, cursor_); return true; }
    if (k == "l" || e == Event::ArrowRight) { for (int i = 0; i < n; ++i) cursor_ = next_cp(input_, cursor_); clamp_normal(); return true; }
    if (k == "w") { for (int i = 0; i < n; ++i) word_forward(); clamp_normal(); return true; }
    if (k == "b") { for (int i = 0; i < n; ++i) word_backward(); return true; }
    if (k == "e") { for (int i = 0; i < n; ++i) word_end(); clamp_normal(); return true; }
    if (k == "0" || k == "^") return cursor_ = 0, true;
    if (k == "$") { cursor_ = input_.size(); clamp_normal(); return true; }
    if (k == "x") {
        save_undo();
        for (int i = 0; i < n; ++i) delete_range(cursor_, next_cp(input_, cursor_));
        clamp_normal();
        return true;
    }
    if (k == "X") {
        save_undo();
        for (int i = 0; i < n && cursor_ > 0; ++i) delete_range(prev_cp(input_, cursor_), cursor_);
        return true;
    }
    if (k == "D" || k == "C") {
        save_undo();
        delete_range(cursor_, input_.size());
        if (k == "C") vim_ = VimMode::Insert;
        clamp_normal();
        return true;
    }
    if (k == "S") {
        save_undo();
        input_.clear();
        return enter_insert(0);
    }
    if (k == "u") {
        std::swap(input_, undo_.first);
        std::swap(cursor_, undo_.second);
        clamp_normal();
        return true;
    }
    if (k == ":") {
        vim_ = VimMode::Command;
        cmdline_.clear();
        return true;
    }
    return false;
}

bool App::handle_command(const Event& e) {
    if (e == Event::Escape || (e == Event::Backspace && cmdline_.empty())) {
        vim_ = VimMode::Normal;
        return true;
    }
    if (e == Event::Return) {
        vim_ = VimMode::Normal;
        run_command(cmdline_);
        return true;
    }
    if (e == Event::Backspace) {
        cmdline_.erase(prev_cp(cmdline_, cmdline_.size()));
        return true;
    }
    if (e.is_character() && static_cast<unsigned char>(e.input()[0]) >= 0x20) {
        cmdline_ += e.input();
        return true;
    }
    return true;
}

void App::word_forward() {
    if (cursor_ >= input_.size()) return;
    int cls = char_class(input_[cursor_]);
    while (cursor_ < input_.size() && cls != 0 && char_class(input_[cursor_]) == cls) cursor_ = next_cp(input_, cursor_);
    while (cursor_ < input_.size() && char_class(input_[cursor_]) == 0) cursor_ = next_cp(input_, cursor_);
}

void App::word_backward() {
    while (cursor_ > 0 && char_class(input_[prev_cp(input_, cursor_)]) == 0) cursor_ = prev_cp(input_, cursor_);
    if (cursor_ == 0) return;
    int cls = char_class(input_[prev_cp(input_, cursor_)]);
    while (cursor_ > 0 && char_class(input_[prev_cp(input_, cursor_)]) == cls) cursor_ = prev_cp(input_, cursor_);
}

void App::word_end() {
    if (input_.empty()) return;
    cursor_ = next_cp(input_, cursor_);
    while (cursor_ < input_.size() && char_class(input_[cursor_]) == 0) cursor_ = next_cp(input_, cursor_);
    if (cursor_ >= input_.size()) return;
    int cls = char_class(input_[cursor_]);
    while (next_cp(input_, cursor_) < input_.size() && char_class(input_[next_cp(input_, cursor_)]) == cls) {
        cursor_ = next_cp(input_, cursor_);
    }
}

void App::scroll_by(int lines) {
    scroll_ = std::max(0, scroll_ + lines);
}

void App::history_step(int dir) {
    if (history_.empty()) return;
    if (history_pos_ == history_.size()) history_draft_ = input_;
    if (dir < 0 && history_pos_ > 0) --history_pos_;
    else if (dir > 0 && history_pos_ < history_.size()) ++history_pos_;
    input_ = history_pos_ == history_.size() ? history_draft_ : history_[history_pos_];
    cursor_ = input_.size();
    clamp_normal();
}

// ---------- actions ----------

void App::submit() {
    std::string text = input_;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    if (text.empty()) return;
    if (history_.empty() || history_.back() != text) history_.push_back(text);
    history_pos_ = history_.size();
    input_.clear();
    cursor_ = 0;
    scroll_ = 0;

    if (text[0] == '/') {
        run_command(text.substr(1));
        return;
    }
    if (busy_) {
        post(Kind::Error, "still working; Ctrl-C interrupts, then send again");
        input_ = text;
        cursor_ = input_.size();
        return;
    }
    add(Kind::User, text);
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    cancel_ = false;
    worker_ = std::thread([this, text] {
        try {
            agent_.submit(text, Origin::Local, *this, cancel_);
        } catch (const std::exception& ex) {
            add(Kind::Error, ex.what());
        }
        busy_ = false;
        screen_.PostEvent(Event::Custom);
    });
}

void App::run_command(const std::string& line) {
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
        } else if (cmd == "h" || cmd == "help") {
            post(Kind::Notice,
                 "normal mode: j/k scroll · Ctrl-D/U half page · Ctrl-F/B page · gg/G top/bottom · counts like 5j\n"
                 "editing: i a I A · h l w b e 0 $ · x X D C S · dd dw cw cc · u undo · Ctrl-P/N prompt history\n"
                 "insert mode: Enter sends · \\ then Enter for a new line · Esc to normal\n"
                 "Shift-Tab cycles modes: manual → edit → auto → plan · Ctrl-C interrupts\n"
                 ":mode NAME · :model NAME · :think on|off · :clear · :status · :up/:down SERVICE · :trip REASON · :unlock · :q\n"
                 "Every command also works as /command in the input.");
        } else if (cmd == "mode") {
            if (auto m = parse_mode(arg)) agent_.mode = *m;
            else post(Kind::Error, "modes: manual, edit, auto, plan");
        } else if (cmd == "model") {
            if (arg.empty()) post(Kind::Notice, "model: " + agent_.model);
            else if (idle()) agent_.model = arg, post(Kind::Notice, "model: " + arg);
        } else if (cmd == "think") {
            if (idle()) agent_.think = arg != "off", post(Kind::Notice, agent_.think ? "thinking on (slower, better on hard problems)" : "thinking off");
        } else if (cmd == "clear") {
            if (idle()) {
                agent_.clear();
                std::lock_guard lock(mu_);
                entries_.clear();
            }
        } else if (cmd == "trip") {
            trip_tripwire("manual trip: " + (arg.empty() ? std::string("from the agent session") : arg));
            post(Kind::Error, "HARNESS TRIPPED. Nothing will run until :unlock");
        } else if (cmd == "unlock") {
            if (!tripwire_state()) {
                post(Kind::Notice, "harness is not tripped");
            } else {
                screen_.WithRestoredIO([] {
                    [[maybe_unused]] int rc = std::system("echo 'Unlocking the MAIC harness.'; sudo -k && sudo /usr/local/sbin/maic-lock reset");
                })();
                post(Kind::Notice, tripwire_state() ? "still tripped" : "harness unlocked; carry on");
            }
        } else if (cmd == "status") {
            std::string out = tripwire_state() ? "harness: TRIPPED" : "harness: armed";
            for (const auto& s : services()) {
                auto st = service_status(s);
                out += "\n" + s.name + ": " + (st.state == ServiceState::Running ? "running (pid " + std::to_string(st.pid) + ")"
                                               : st.state == ServiceState::Foreign ? "port in use, not started by MAIC"
                                                                                    : "stopped");
            }
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
            if (!found) post(Kind::Error, "unknown service: " + arg);
        } else if (!cmd.empty()) {
            post(Kind::Error, "unknown command :" + cmd + " (try :help)");
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
    answer(Approval::No);
    if (worker_.joinable()) worker_.join();
}

}  // namespace

int run_tui(const std::string& model) {
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);  // keep the terminal's own text selection working
    App app(screen, model);
    app.welcome();
    auto component = CatchEvent(Renderer([&] { return app.render(); }), [&](Event e) { return app.handle(e); });
    screen.Loop(component);
    return 0;
}

}  // namespace maic
