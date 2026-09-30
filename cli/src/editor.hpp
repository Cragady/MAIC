#pragma once

#include <ftxui/component/event.hpp>

#include <string>
#include <utility>
#include <vector>

namespace maic {

// The input box: a small vim. Starts in normal mode. Insert, normal, visual and visual-line modes, counts,
// operators, a register, multi-level undo/redo (an insert session is one step), and prompt history.
// Enter inserts a newline; sending is the app's job (Alt+Enter or :w). Cursor positions are byte offsets.
class Editor {
public:
    enum class Mode { Insert, Normal, Visual, VisualLine, Command };
    enum class Action { None, Command, Search };

    struct Result {
        Action action = Action::None;
        std::string text;  // the command line for Command / Search
    };

    Editor(std::string* shared_register) : register_(shared_register) {}

    Result handle(const ftxui::Event& e);

    Mode mode() const { return mode_; }
    const std::string& text() const { return text_; }
    size_t cursor() const { return cursor_; }
    const std::string& cmdline() const { return cmdline_; }
    char cmd_prefix() const { return cmd_prefix_; }  // ':' or '/' while in command mode
    void set_cmdline(std::string line) { cmdline_ = std::move(line); }
    bool empty() const { return text_.empty(); }
    // Selection as [begin, end) byte offsets, valid in the visual modes.
    std::pair<size_t, size_t> selection() const;

    void set_text(std::string text);
    void replace_text(std::string text);  // like set_text, but undoable (external editor round trip)
    void clear();
    void undo();
    void redo();
    void remember(const std::string& text);  // prompt history
    void history_step(int dir);
    void escape();  // to normal mode, like pressing Esc
    void begin_command(char prefix);  // ':' or '/'

private:
    bool handle_insert(const ftxui::Event& e);
    bool handle_normal(const ftxui::Event& e);
    bool handle_visual(const ftxui::Event& e);
    bool handle_command(const ftxui::Event& e, Result& result);

    void word_forward();
    void word_backward();
    void word_end();
    void current_word_end();
    void line_start();
    void line_end();
    void save_undo();
    void delete_range(size_t from, size_t to, bool yank);
    void clamp_normal();
    void enter_insert(size_t at, bool snapshot = true);
    void paste(bool after);
    size_t motion_target(const std::string& k, int n, bool& inclusive);

    std::string text_;
    size_t cursor_ = 0;
    Mode mode_ = Mode::Normal;
    std::string pending_;  // operator waiting for a motion: d, c, y, g
    int count_ = 0;
    size_t anchor_ = 0;    // visual selection start
    std::string cmdline_;
    char cmd_prefix_ = ':';
    std::vector<std::pair<std::string, size_t>> undo_, redo_;
    std::string* register_;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    std::string history_draft_;
};

// UTF-8 helpers shared by the editor and the view.
size_t utf8_next(const std::string& s, size_t i);
size_t utf8_prev(const std::string& s, size_t i);
size_t utf8_len(const std::string& s);
size_t utf8_offset(const std::string& s, size_t code_points);
int char_class(unsigned char c);  // 0 space, 1 word, 2 punctuation

}  // namespace maic
