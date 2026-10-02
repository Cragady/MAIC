#pragma once

#include "maic/markdown.hpp"
#include "maic/settings.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace maic {

enum class Kind { User, Assistant, Thinking, Tool, ToolOk, ToolErr, Notice, Error, Shell };

struct Entry {
    Kind kind;
    std::string text;        // the full text; tool results show a preview while collapsed
    bool collapsed = false;
    time_t when = 0;  // appended at; shown when timestamps are on
    bool live = false;  // a running command's last lines, until its result takes their place
};

// The conversation window. Appends are thread-safe; everything else runs on the UI thread. When focused
// (Ctrl-W k) it behaves like a read-only vim buffer: motions, visual selection, yank, search.
class View {
public:
    explicit View(std::string* shared_register) : register_(shared_register) {}
    void set_leader(std::string leader) { leader_ = std::move(leader); }

    // thread-safe
    void append(Kind kind, std::string text);
    void set_collapse_default(bool on) { collapse_default_ = on; }
    bool collapse_default() const { return collapse_default_; }
    void set_all_collapsed(bool on);  // zR / zM
    void append_to_last(Kind kind, std::string_view delta);  // streaming: extends the last entry if it has this kind
    // A running command's output: extends the live entry (made under the tool call on the first chunk), which
    // keeps only the last kLiveLines lines. finish_live appends the result and drops the live entry.
    void live_output(std::string_view text);
    void finish_live(Kind kind, std::string text);
    void clear();
    size_t size() const;

    // UI thread
    void set_markdown(bool on) { markdown_ = on; ++version_; }
    void set_timestamps(bool on) { timestamps_ = on; ++version_; }
    std::string last_assistant() const;  // the newest reply's text, "" when none
    bool markdown() const { return markdown_; }
    void set_focused(bool on);
    bool focused() const { return focused_; }
    bool visual() const { return visual_ != VisualMode::None; }
    bool follows() const { return scroll_ == 0; }

    void scroll_by(int lines);       // positive = older
    void scroll_to_top();
    void scroll_to_bottom();
    void page(int direction);        // direction -1 = up
    void half_page(int direction);

    // Keys while focused. Returns a status message to show, or "" for none.
    std::string handle(const ftxui::Event& e, int view_height);
    void search(const std::string& pattern);
    std::string search_next(int direction);

    ftxui::Element render(const Settings& settings, size_t width, int height);
    // A mouse click on row `row` of the last rendered window (0 = its top line): toggles the fold of the
    // tool entry under it. Returns true when something was toggled.
    bool click(int row);
    std::string status_hint() const;  // "↑12" while scrolled, position when focused
    size_t match_count() const { return matches_.size(); }

private:
    struct Line {
        StyledLine spans;
        Kind kind;
        size_t entry;
        size_t begin, end;  // byte range in the entry's text
        bool first;         // first line of its entry (gets the prefix)
    };
    enum class VisualMode { None, Char, Line };

    void layout(size_t width);
    std::string yank_selection();
    void move_cursor_line(int delta);
    void jump_message(int direction, bool user_only, int count);  // } { ]] [[
    void ensure_cursor_visible(int height);
    std::string text_of(const Line& l) const;
    void find_matches();

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    unsigned version_ = 0;  // bumped on every change; layout is cached against it

    std::vector<Line> lines_;
    unsigned laid_out_version_ = ~0u;
    size_t laid_out_width_ = 0;
    bool markdown_ = true;
    bool timestamps_ = false;

    int scroll_ = 0;          // lines up from the bottom; 0 follows new output
    size_t last_total_ = 0;
    int last_height_ = 10;
    int last_top_ = 0;        // first line index shown by the last render

    bool focused_ = false;
    size_t cur_line_ = 0, cur_col_ = 0;
    size_t anchor_line_ = 0, anchor_col_ = 0;
    VisualMode visual_ = VisualMode::None;
    std::string pending_;
    int count_ = 0;
    std::string* register_;
    bool collapse_default_ = true;
    std::string leader_ = " ";
    std::string yank_word_range(char scope, size_t& from_col, size_t& to_col) const;  // iw / aw on the cursor line

    std::string last_find_kind_, last_find_char_;  // f F t T target for ; and ,
    void find_on_line(const std::string& kind, const std::string& ch, int n);
    std::string pattern_;
    std::vector<std::pair<size_t, size_t>> matches_;  // (line, column)
};

// Diff text in tool output and approval previews: `+` lines, `-` lines, and `@@` or file headers.
bool looks_like_diff(const std::string& text);
unsigned diff_flags(const std::string& line);  // DiffAdd, DiffDel, DiffHunk or MdNone
std::vector<StyledLine> diff_lines(const std::string& text);

}  // namespace maic
