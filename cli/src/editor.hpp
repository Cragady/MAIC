#pragma once

#include <ftxui/component/event.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace maid {

// The input box: a small vim. Starts in normal mode. Insert, normal, replace, visual and visual-line modes,
// counts, operators (d c y > < gq gu gU g~), text objects, marks, named registers, macros, `.` repeat,
// multi-level undo/redo (an insert session is one step), and prompt history. Enter inserts a newline; sending
// is the app's job (Alt+Enter or :w), unless enter_sends is on and the input is one line. Cursor positions are
// byte offsets.
class Editor {
public:
    enum class Mode { Insert, Normal, Visual, VisualLine, Command, Replace };
    // Search / SearchBack: `/` and `*` / `#`, which search the conversation. Send: Enter with enter_sends on.
    enum class Action { None, Command, Search, SearchBack, Send };

    struct Result {
        Action action = Action::None;
        std::string text;  // the command line for Command, the pattern for Search / SearchBack
    };

    struct Register {
        std::string text;
        bool linewise = false;  // whole lines (dd, yy, yj): p puts them on a new line
    };

    Editor(std::string* shared_register) : register_(shared_register) {}

    void set_leader(std::string leader) { leader_ = std::move(leader); }
    void set_textwidth(int cols) { textwidth_ = cols; }    // gq wraps to this
    void set_shiftwidth(int cols) { shiftwidth_ = cols; }  // > and < shift by this
    void set_enter_sends(bool on) { enter_sends_ = on; }   // Enter sends a one-line input (Result::Send)

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
    // The named registers "a to "z that hold something, for :reg. The shared register is the unnamed one.
    // A recorded macro is the register's text, with special keys as the bytes the terminal sends.
    const std::map<char, Register>& registers() const { return registers_; }
    char recording() const { return recording_; }  // the register q{a-z} is recording into, 0 when none

    void set_text(std::string text);
    void replace_text(std::string text);  // like set_text, but undoable (external editor round trip)
    void clear();
    void undo();
    void redo();
    void remember(const std::string& text);  // prompt history
    void set_history(std::vector<std::string> items) { history_ = std::move(items); history_pos_ = history_.size(); }
    void history_step(int dir);
    void escape();  // to normal mode, like pressing Esc
    void begin_command(char prefix);  // ':' or '/'
    void newline();  // a line break at the cursor in insert mode: Shift+Enter / Alt+Enter when enter_sends is on

private:
    struct Range {
        size_t from = 0, to = 0;  // [from, to)
        bool linewise = false;
        bool inclusive = false;
        bool jump = false;    // sets the previous position for '' and ``
        bool object = false;  // a text object: in visual mode it replaces the selection
    };
    enum class Parse { Done, More, None };

    bool handle_insert(const ftxui::Event& e, Result& result);
    bool handle_normal(const ftxui::Event& e, Result& result);
    bool handle_visual(const ftxui::Event& e);
    void run_macro(char reg, int n, Result& result);  // @{a-z}, @@
    void stop_recording();
    bool handle_command(const ftxui::Event& e, Result& result);
    void finish_command();
    bool idle() const;        // nothing half typed
    bool wants_char() const;  // the next key is an argument: after r m " f F t T ' ` i a q @
    void leave_insert();
    void repeat_change(int n);

    size_t line_begin(size_t pos) const;
    size_t line_finish(size_t pos) const;  // the '\n' or the end of the text
    size_t first_nonblank(size_t pos) const;
    size_t next_line(size_t pos) const;  // start of the next line, or npos
    bool blank_line(size_t pos) const;
    // Word motions; `big` is the WORD kind (W B E gE: runs of non-blanks).
    int cls_at(size_t i, bool big) const;
    void word_forward(bool big);
    void word_backward(bool big);
    void word_end(bool big);
    void word_end_backward(bool big);
    void current_word_end(bool big);
    size_t match_bracket() const;  // %: the partner of the bracket under or after the cursor on the line, or npos
    std::string word_under_cursor() const;  // * #: the keyword under or after the cursor on the line
    void line_start();
    void line_end();
    size_t find_char(const std::string& kind, const std::string& ch, int n, bool repeat) const;  // f F t T on the line, or npos
    size_t paragraph_forward(size_t pos, int n) const;
    size_t paragraph_backward(size_t pos, int n) const;
    std::vector<size_t> sentence_starts() const;
    size_t sentence_forward(size_t pos, int n) const;
    size_t sentence_backward(size_t pos, int n) const;
    // iw aw iW aW i" a" i' a' i` a` i( a( ib ab i[ a[ i{ a{ iB aB i< a< ip ap is as
    bool text_object(char scope, char obj, int n, Range& r) const;
    // Reads one key of a motion; pending_ holds the keys already typed for it (i a f F t T ' ` g). Text
    // objects are allowed after an operator and in visual mode. Done fills r, More wants another key, None drops it.
    Parse motion(const std::string& k, int count, bool objects, Range& r);
    void move_to(const Range& r);
    void apply_operator(const std::string& op, Range r, int n);
    void line_range(int n, Range& r) const;  // n lines from the cursor, for dd yy >> gqq ...
    void shift_lines(size_t from, size_t to, int dir, int times);
    void format_lines(size_t from, size_t to);
    void change_case(size_t from, size_t to, char how);  // 'u' 'U' '~'
    void join_lines(int n);
    void replace_chars(const std::string& ch, int n);

    void save_undo();
    void insert_text(size_t at, const std::string& s);
    void erase_text(size_t from, size_t to);
    void delete_range(size_t from, size_t to, bool yank);
    void clamp_normal();
    void enter_insert(size_t at, bool snapshot = true, int repeat = 1, char kind = 0);
    Register current_register() const;  // the one "x picked, the clipboard for "+, else the unnamed one
    void yank_range(size_t from, size_t to, bool linewise = false);  // to the register(s), and the clipboard when "+ or leader asked
    void paste(bool after, int n);
    void insert_register(const Register& r);  // at the cursor, in insert mode

    std::string text_;
    size_t cursor_ = 0;
    Mode mode_ = Mode::Normal;
    std::string op_;       // operator waiting for a motion: d c y > < gq gu gU g~
    std::string pending_;  // keys of a half-typed command or motion: g, i, a, f, ', m, r, " ...
    std::string leader_ = " ";
    bool leader_pending_ = false;
    bool clip_next_ = false;  // "+ or "* was typed: the next yank goes to the clipboard, the next paste comes from it
    char reg_sel_ = 0;        // "a .. "z / "A .. "Z was typed
    int count_ = 0;
    int op_count_ = 0;  // the count typed before the operator (2d3w deletes six words)
    size_t anchor_ = 0;    // visual selection start
    std::string cmdline_;
    char cmd_prefix_ = ':';
    std::vector<std::pair<std::string, size_t>> undo_, redo_;
    std::string* register_;
    std::map<char, Register> registers_;
    bool unnamed_linewise_ = false;
    std::string unnamed_text_;  // what the unnamed register held when the editor last wrote it (the view writes it too)
    std::vector<std::string> history_;
    size_t history_pos_ = 0;
    std::string history_draft_;
    int textwidth_ = 80;
    int shiftwidth_ = 4;
    bool enter_sends_ = false;

    std::string last_find_kind_, last_find_char_;  // for ; and ,
    std::map<char, size_t> marks_;
    size_t prev_pos_ = 0;  // before the last jump: '' and ``

    // `.`: the keys of the last change, without their count; an insert session is part of its change.
    std::vector<ftxui::Event> keys_, last_change_;
    std::string change_start_;  // the text when keys_ started
    std::vector<int> rec_groups_;  // the counts typed in it, multiplied together for `.`
    int last_count_ = 0;
    bool replaying_ = false;
    bool no_repeat_ = false;    // this command is not a change for `.` (u, Ctrl-R, history)
    int insert_once_ = 0;  // Ctrl-O: 1 just pressed, 2 running one normal command, then back to insert
    std::vector<ftxui::Event> session_keys_;  // typed in this insert session, for a count on i a o O R
    int insert_repeat_ = 1;
    char insert_kind_ = 0;
    std::vector<std::string> replaced_;  // replace mode: what each typed character covered ("" when it was inserted)
    bool insert_reg_pending_ = false;    // Ctrl-R waiting for its register

    // Macros: q{a-z} appends every key to macro_text_ until q; @{a-z} feeds the register's text back as keys.
    char recording_ = 0;
    std::string macro_text_;
    char last_macro_ = 0;
    int macro_depth_ = 0;        // > 0 while a macro runs: its keys are not recorded again
    bool motion_failed_ = false;  // a motion found nothing: a running macro stops, as vim's does on an error
};

// UTF-8 helpers shared by the editor and the view.
size_t utf8_next(const std::string& s, size_t i);
size_t utf8_prev(const std::string& s, size_t i);
size_t utf8_len(const std::string& s);
size_t utf8_offset(const std::string& s, size_t code_points);
int char_class(unsigned char c);  // 0 space, 1 word, 2 punctuation

}  // namespace maid
