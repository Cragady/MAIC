#include "editor.hpp"

#include "maic/clipboard.hpp"

#include <algorithm>
#include <cctype>

namespace maic {

using namespace ftxui;

size_t utf8_next(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t utf8_prev(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

size_t utf8_offset(const std::string& s, size_t code_points) {
    size_t i = 0;
    while (code_points-- && i < s.size()) i = utf8_next(s, i);
    return i;
}

int char_class(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n') return 0;
    if (c >= 0x80 || std::isalnum(c) || c == '_') return 1;
    return 2;
}

std::pair<size_t, size_t> Editor::selection() const {
    size_t a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
    if (mode_ == Mode::VisualLine) {
        while (a > 0 && text_[a - 1] != '\n') --a;
        while (b < text_.size() && text_[b] != '\n') ++b;
        return {a, b};
    }
    return {a, std::min(utf8_next(text_, b), text_.size())};
}

void Editor::set_text(std::string text) {
    text_ = std::move(text);
    cursor_ = text_.size();
    clamp_normal();
}

void Editor::replace_text(std::string text) {
    save_undo();
    set_text(std::move(text));
}

void Editor::clear() {
    text_.clear();
    cursor_ = 0;
    pending_.clear();
    count_ = 0;
    undo_.clear();
    redo_.clear();
}

void Editor::save_undo() {
    if (!undo_.empty() && undo_.back().first == text_) {
        undo_.back().second = cursor_;
        return;
    }
    undo_.push_back({text_, cursor_});
    redo_.clear();
    if (undo_.size() > 200) undo_.erase(undo_.begin());
}

void Editor::undo() {
    while (!undo_.empty() && undo_.back().first == text_) undo_.pop_back();  // skip no-op snapshots
    if (undo_.empty()) return;
    redo_.push_back({text_, cursor_});
    text_ = undo_.back().first;
    cursor_ = std::min(undo_.back().second, text_.size());
    undo_.pop_back();
    clamp_normal();
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back({text_, cursor_});
    text_ = redo_.back().first;
    cursor_ = std::min(redo_.back().second, text_.size());
    redo_.pop_back();
    clamp_normal();
}

void Editor::remember(const std::string& text) {
    if (history_.empty() || history_.back() != text) history_.push_back(text);
    history_pos_ = history_.size();
}

void Editor::history_step(int dir) {
    if (history_.empty()) return;
    if (history_pos_ == history_.size()) history_draft_ = text_;
    if (dir < 0 && history_pos_ > 0) --history_pos_;
    else if (dir > 0 && history_pos_ < history_.size()) ++history_pos_;
    else return;
    text_ = history_pos_ == history_.size() ? history_draft_ : history_[history_pos_];
    cursor_ = text_.size();
    clamp_normal();
}

void Editor::escape() {
    if (mode_ == Mode::Insert && cursor_ > 0) cursor_ = utf8_prev(text_, cursor_);
    mode_ = Mode::Normal;
    pending_.clear();
    count_ = 0;
    clamp_normal();
}

void Editor::begin_command(char prefix) {
    mode_ = Mode::Command;
    cmd_prefix_ = prefix;
    cmdline_.clear();
}

void Editor::clamp_normal() {
    if (mode_ != Mode::Insert && mode_ != Mode::Command && cursor_ >= text_.size() && !text_.empty()) cursor_ = utf8_prev(text_, text_.size());
}

void Editor::enter_insert(size_t at, bool snapshot) {
    if (snapshot) save_undo();  // the whole insert session undoes as one step
    cursor_ = std::min(at, text_.size());
    mode_ = Mode::Insert;
}

void Editor::delete_range(size_t from, size_t to, bool yank) {
    if (from > to) std::swap(from, to);
    to = std::min(to, text_.size());
    if (yank && to > from) *register_ = text_.substr(from, to - from);
    text_.erase(from, to - from);
    cursor_ = std::min(from, text_.size());
}

void Editor::yank_range(size_t from, size_t to) {
    if (from > to) std::swap(from, to);
    to = std::min(to, text_.size());
    *register_ = text_.substr(from, to - from);
    if (clip_next_) copy_to_clipboard(*register_);
    clip_next_ = false;
}

void Editor::paste(bool after) {
    std::string source = *register_;
    if (clip_next_) {
        clip_next_ = false;
        source = paste_from_clipboard();
    }
    if (source.empty()) return;
    save_undo();
    size_t at = after ? utf8_next(text_, cursor_) : cursor_;
    if (mode_ != Mode::Insert && text_.empty()) at = 0;
    text_.insert(at, source);
    cursor_ = at + source.size();
    if (mode_ != Mode::Insert) cursor_ = utf8_prev(text_, cursor_ + 1 > text_.size() ? text_.size() : cursor_);
    clamp_normal();
}

void Editor::word_forward() {
    if (cursor_ >= text_.size()) return;
    int cls = char_class(text_[cursor_]);
    while (cursor_ < text_.size() && cls != 0 && char_class(text_[cursor_]) == cls) cursor_ = utf8_next(text_, cursor_);
    while (cursor_ < text_.size() && char_class(text_[cursor_]) == 0) cursor_ = utf8_next(text_, cursor_);
}

void Editor::word_backward() {
    while (cursor_ > 0 && char_class(text_[utf8_prev(text_, cursor_)]) == 0) cursor_ = utf8_prev(text_, cursor_);
    if (cursor_ == 0) return;
    int cls = char_class(text_[utf8_prev(text_, cursor_)]);
    while (cursor_ > 0 && char_class(text_[utf8_prev(text_, cursor_)]) == cls) cursor_ = utf8_prev(text_, cursor_);
}

void Editor::word_end() {
    if (text_.empty()) return;
    cursor_ = utf8_next(text_, cursor_);
    while (cursor_ < text_.size() && char_class(text_[cursor_]) == 0) cursor_ = utf8_next(text_, cursor_);
    if (cursor_ >= text_.size()) {
        cursor_ = utf8_prev(text_, text_.size());
        return;
    }
    int cls = char_class(text_[cursor_]);
    while (utf8_next(text_, cursor_) < text_.size() && char_class(text_[utf8_next(text_, cursor_)]) == cls) cursor_ = utf8_next(text_, cursor_);
}

// End of the word under the cursor (no advance first): what `cw` changes.
void Editor::current_word_end() {
    if (cursor_ >= text_.size()) return;
    int cls = char_class(text_[cursor_]);
    while (utf8_next(text_, cursor_) < text_.size() && char_class(text_[utf8_next(text_, cursor_)]) == cls) cursor_ = utf8_next(text_, cursor_);
}

void Editor::line_start() {
    while (cursor_ > 0 && text_[cursor_ - 1] != '\n') --cursor_;
}

void Editor::line_end() {
    while (cursor_ < text_.size() && text_[cursor_] != '\n') ++cursor_;
}

bool Editor::text_object(char scope, char obj, size_t& from, size_t& to) const {
    if (text_.empty()) return false;
    size_t cur = std::min(cursor_, text_.size() - 1);
    if (obj == 'w' || obj == 'W') {
        auto cls = [&](size_t i) { return obj == 'W' ? (char_class(text_[i]) == 0 ? 0 : 1) : char_class(text_[i]); };
        int c = cls(cur);
        from = cur;
        to = utf8_next(text_, cur);
        while (from > 0 && text_[from - 1] != '\n' && cls(utf8_prev(text_, from)) == c) from = utf8_prev(text_, from);
        while (to < text_.size() && text_[to] != '\n' && cls(to) == c) to = utf8_next(text_, to);
        if (scope == 'a' && c != 0) {
            size_t t = to;
            while (t < text_.size() && text_[t] == ' ') ++t;
            if (t > to) to = t;
            else while (from > 0 && text_[from - 1] == ' ') --from;
        }
        return true;
    }
    if (obj == '"' || obj == '\'' || obj == '`') {
        // The pair on this line around the cursor: the quote at or before the cursor opens, the next one closes.
        size_t line_start = cur;
        while (line_start > 0 && text_[line_start - 1] != '\n') --line_start;
        size_t line_end = cur;
        while (line_end < text_.size() && text_[line_end] != '\n') ++line_end;
        size_t open = std::string::npos;
        for (size_t i = line_start; i <= cur && i < line_end; ++i) {
            if (text_[i] == obj) open = (open == std::string::npos || (text_[cur] != obj && i <= cur)) ? i : open;
        }
        // Count quotes before the cursor: an even count means the cursor sits before a pair that starts after it.
        size_t count = 0, last = std::string::npos;
        for (size_t i = line_start; i < cur; ++i) {
            if (text_[i] == obj) ++count, last = i;
        }
        if (text_[cur] == obj) open = (count % 2 == 1) ? last : cur;
        else if (count % 2 == 1) open = last;
        else open = text_.find(obj, cur);
        if (open == std::string::npos || open >= line_end) return false;
        size_t close = text_.find(obj, open + 1);
        if (close == std::string::npos || close >= line_end) return false;
        from = scope == 'i' ? open + 1 : open;
        to = scope == 'i' ? close : close + 1;
        return true;
    }
    char o = 0, c = 0;
    switch (obj) {
        case '(': case ')': case 'b': o = '(', c = ')'; break;
        case '[': case ']': o = '[', c = ']'; break;
        case '{': case '}': case 'B': o = '{', c = '}'; break;
        case '<': case '>': o = '<', c = '>'; break;
        default: return false;
    }
    // Nearest enclosing pair, counting nesting; a cursor on a bracket belongs to that pair.
    size_t open = std::string::npos;
    int depth = 0;
    for (size_t i = cur + 1; i-- > 0;) {
        if (text_[i] == c && i != cur) ++depth;
        else if (text_[i] == o) {
            if (depth == 0) {
                open = i;
                break;
            }
            --depth;
        }
    }
    if (open == std::string::npos) return false;
    depth = 0;
    size_t close = std::string::npos;
    for (size_t i = open + 1; i < text_.size(); ++i) {
        if (text_[i] == o) ++depth;
        else if (text_[i] == c) {
            if (depth == 0) {
                close = i;
                break;
            }
            --depth;
        }
    }
    if (close == std::string::npos) return false;
    from = scope == 'i' ? open + 1 : open;
    to = scope == 'i' ? close : close + 1;
    return true;
}

size_t Editor::find_char(const std::string& kind, const std::string& ch, int n) const {
    if (ch.empty() || text_.empty()) return std::string::npos;
    bool forward = kind == "f" || kind == "t";
    size_t line_start = cursor_, line_end = cursor_;
    while (line_start > 0 && text_[line_start - 1] != '\n') --line_start;
    while (line_end < text_.size() && text_[line_end] != '\n') ++line_end;
    size_t pos = cursor_;
    for (int i = 0; i < n; ++i) {
        size_t found = std::string::npos;
        if (forward) {
            size_t from = utf8_next(text_, pos);
            if (kind == "t" && i == 0) from = utf8_next(text_, from);  // t x right before an x: skip it, like vim
            found = text_.find(ch, from);
            if (found >= line_end) return std::string::npos;
        } else {
            if (pos <= line_start) return std::string::npos;
            size_t upto = utf8_prev(text_, pos);
            if (kind == "T" && i == 0 && upto > line_start) upto = utf8_prev(text_, upto);
            found = text_.rfind(ch, upto);
            if (found == std::string::npos || found < line_start) return std::string::npos;
        }
        pos = found;
    }
    if (kind == "t") return utf8_prev(text_, pos);
    if (kind == "T") return utf8_next(text_, pos);
    return pos;
}

bool Editor::handle_find(const std::string& k, int n) {
    std::string kind, ch;
    if (!find_pending_.empty()) {
        kind = find_pending_;
        ch = k;
        find_pending_.clear();
        last_find_kind_ = kind;
        last_find_char_ = ch;
    } else if (k == "f" || k == "F" || k == "t" || k == "T") {
        find_pending_ = k;
        find_count_ = n;
        return true;
    } else if (k == ";" || k == ",") {
        if (last_find_kind_.empty()) return true;
        kind = last_find_kind_;
        if (k == ",") kind = kind == "f" ? "F" : kind == "F" ? "f" : kind == "t" ? "T" : "t";
        ch = last_find_char_;
    } else {
        return false;
    }
    if (kind == last_find_kind_ && ch == last_find_char_ && k != ";" && k != ",") n = find_count_;  // the count came before the f
    size_t dest = find_char(kind, ch, n);
    if (dest == std::string::npos) {
        pending_.clear();
        return true;
    }
    std::string op = pending_;
    pending_.clear();
    if (op == "d" || op == "c" || op == "y") {
        bool inclusive = kind == "f" || kind == "t";  // forward finds include the character; backward ones stop before the cursor
        size_t start = cursor_, end = inclusive ? utf8_next(text_, dest) : dest;
        size_t a = std::min(start, end), b = std::max(start, end);
        if (op == "y") {
            yank_range(a, b);
            cursor_ = a;
            return clamp_normal(), true;
        }
        save_undo();
        yank_range(a, b);
        delete_range(a, b, false);
        if (op == "c") enter_insert(cursor_, false);
        else clamp_normal();
        return true;
    }
    cursor_ = dest;
    if (mode_ != Mode::Visual && mode_ != Mode::VisualLine) clamp_normal();
    return true;
}

// Moves the cursor by a motion key and reports whether an operator over it includes the final character.
size_t Editor::motion_target(const std::string& k, int n, bool& inclusive) {
    inclusive = false;
    for (int i = 0; i < n; ++i) {
        if (k == "w") word_forward();
        else if (k == "b") word_backward();
        else if (k == "e") word_end(), inclusive = true;
        else if (k == "h") cursor_ = utf8_prev(text_, cursor_);
        else if (k == "l") cursor_ = utf8_next(text_, cursor_);
        else if (k == "j" || k == "k") {
            size_t col = cursor_;
            line_start();
            col = cursor_ == col ? 0 : col - cursor_;
            if (k == "j") {
                line_end();
                if (cursor_ < text_.size()) ++cursor_;
            } else if (cursor_ > 0) {
                --cursor_;
                line_start();
            }
            size_t end = cursor_;
            while (end < text_.size() && text_[end] != '\n') ++end;
            cursor_ = std::min(cursor_ + col, end);
        }
    }
    if (k == "0" || k == "^") line_start();
    if (k == "$") line_end(), inclusive = true;
    return cursor_;
}

Editor::Result Editor::handle(const Event& e) {
    Result result;
    switch (mode_) {
        case Mode::Insert: handle_insert(e); break;
        case Mode::Normal: handle_normal(e); break;
        case Mode::Visual:
        case Mode::VisualLine: handle_visual(e); break;
        case Mode::Command: handle_command(e, result); break;
    }
    return result;
}

bool Editor::handle_insert(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) return escape(), true;
    if (e == Event::Return) {
        text_.insert(cursor_, "\n");
        ++cursor_;
        return true;
    }
    if (e == Event::Backspace) {
        if (cursor_ > 0) delete_range(utf8_prev(text_, cursor_), cursor_, false);
        return true;
    }
    if (e == Event::Delete) return delete_range(cursor_, utf8_next(text_, cursor_), false), true;
    if (k == "\x17") {  // Ctrl-W
        size_t end = cursor_;
        word_backward();
        delete_range(cursor_, end, true);
        return true;
    }
    if (k == "\x15") return delete_range(0, cursor_, true), true;  // Ctrl-U
    if (k == "\x19") return paste(false), true;                   // Ctrl-Y: paste the register
    if (e == Event::ArrowLeft) return cursor_ = utf8_prev(text_, cursor_), true;
    if (e == Event::ArrowRight) return cursor_ = utf8_next(text_, cursor_), true;
    if (e == Event::Home) return line_start(), true;
    if (e == Event::End) return line_end(), true;
    if (e == Event::ArrowUp || k == "\x10") return history_step(-1), true;
    if (e == Event::ArrowDown || k == "\x0e") return history_step(1), true;
    if (e.is_character() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) {
        text_.insert(cursor_, k);
        cursor_ += k.size();
        return true;
    }
    return false;
}

bool Editor::handle_normal(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) return pending_.clear(), find_pending_.clear(), count_ = 0, leader_pending_ = false, clip_next_ = false, true;
    if (!find_pending_.empty() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) return handle_find(k, std::max(count_, 1)), count_ = 0, true;
    if (k == "\x12") return redo(), true;  // Ctrl-R
    if (leader_pending_) {
        leader_pending_ = false;
        if (k == "y") {  // <leader>y: the line to the clipboard
            clip_next_ = true;
            size_t a = cursor_, b = cursor_;
            while (a > 0 && text_[a - 1] != '\n') --a;
            while (b < text_.size() && text_[b] != '\n') ++b;
            yank_range(a, b);
        } else if (k == "p" || k == "P") {
            clip_next_ = true;
            paste(k == "p");
        }
        return true;
    }
    if (!leader_.empty() && k == leader_ && pending_.empty()) return leader_pending_ = true, true;

    if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
        count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
        return true;
    }
    int n = std::max(count_, 1);
    count_ = 0;

    if ((pending_ == "d" || pending_ == "c" || pending_ == "y" || pending_.empty()) && (k == "f" || k == "F" || k == "t" || k == "T" || k == ";" || k == ",")) {
        return handle_find(k, n);
    }
    if (!pending_.empty()) {
        std::string op = pending_;
        pending_.clear();
        if (op == "\"") {
            if (k == "+" || k == "*") clip_next_ = true;
            return true;
        }
        if ((op == "d" || op == "c" || op == "y") && (k == "i" || k == "a")) return pending_ = op + k, true;
        if (op.size() == 2 && (op[1] == 'i' || op[1] == 'a')) {
            size_t from, to;
            if (!text_object(op[1], k.empty() ? 0 : k[0], from, to)) return true;
            if (op[0] == 'y') {
                yank_range(from, to);
                cursor_ = from;
                return clamp_normal(), true;
            }
            save_undo();
            yank_range(from, to);
            text_.erase(from, to - from);
            cursor_ = std::min(from, text_.size());
            if (op[0] == 'c') enter_insert(cursor_, false);
            else clamp_normal();
            return true;
        }
        if (op == "g" && k == "g") return cursor_ = 0, true;
        bool whole = (op == "d" && k == "d") || (op == "c" && k == "c") || (op == "y" && k == "y");
        if (whole) {
            if (op == "y") {
                yank_range(0, text_.size());
                return true;
            }
            save_undo();
            yank_range(0, text_.size());
            text_.clear();
            cursor_ = 0;
            if (op == "c") enter_insert(0, false);
            return true;
        }
        static const char* motions[] = {"w", "b", "e", "h", "l", "0", "^", "$", "j", "k"};
        if (std::find(std::begin(motions), std::end(motions), k) != std::end(motions)) {
            size_t start = cursor_;
            bool inclusive = false;
            size_t end;
            if (op == "c" && k == "w" && cursor_ < text_.size() && char_class(text_[cursor_]) != 0) {
                // vim special case: cw on a word changes to the end of that word (then n-1 more words)
                current_word_end();
                for (int i = 1; i < n; ++i) word_end();
                end = utf8_next(text_, cursor_);
            } else {
                end = motion_target(k, n, inclusive);
                if (inclusive) end = utf8_next(text_, end);
            }
            if (op == "y") {
                size_t a = std::min(start, end), b = std::max(start, end);
                yank_range(a, b);
                cursor_ = a;
                clamp_normal();
                return true;
            }
            save_undo();
            yank_range(start, end);
            delete_range(start, end, false);
            if (op == "c") enter_insert(cursor_, false);
            else clamp_normal();
            return true;
        }
        return true;  // unknown pair: dropped, like vim
    }

    if (k == "i") return enter_insert(cursor_), true;
    if (k == "a") return enter_insert(utf8_next(text_, cursor_)), true;
    if (k == "I") return line_start(), enter_insert(cursor_), true;
    if (k == "A") return line_end(), enter_insert(cursor_), true;
    if (k == "o") return line_end(), text_.insert(cursor_, "\n"), enter_insert(cursor_ + 1), true;
    if (k == "O") return line_start(), text_.insert(cursor_, "\n"), enter_insert(cursor_), true;
    if (k == "v") return anchor_ = cursor_, mode_ = Mode::Visual, true;
    if (k == "V") return anchor_ = cursor_, mode_ = Mode::VisualLine, true;
    if (k == "p") return paste(true), true;
    if (k == "P") return paste(false), true;
    if (k == "g" || k == "d" || k == "c" || k == "y" || k == "\"") return pending_ = k, true;
    if (k == "Y") {  // yank the line, like yy
        size_t a = cursor_, b = cursor_;
        while (a > 0 && text_[a - 1] != '\n') --a;
        while (b < text_.size() && text_[b] != '\n') ++b;
        yank_range(a, b);
        return true;
    }
    if (k == "G") return cursor_ = text_.size(), clamp_normal(), true;
    if (k == "x") {
        save_undo();
        for (int i = 0; i < n; ++i) delete_range(cursor_, utf8_next(text_, cursor_), i == 0);
        return clamp_normal(), true;
    }
    if (k == "X") {
        save_undo();
        for (int i = 0; i < n && cursor_ > 0; ++i) delete_range(utf8_prev(text_, cursor_), cursor_, i == 0);
        return true;
    }
    if (k == "D" || k == "C") {
        save_undo();
        size_t end = cursor_;
        while (end < text_.size() && text_[end] != '\n') ++end;
        delete_range(cursor_, end, true);
        if (k == "C") enter_insert(cursor_, false);
        else clamp_normal();
        return true;
    }
    if (k == "S") {
        save_undo();
        text_.clear();
        return enter_insert(0, false), true;
    }
    if (k == "u") return undo(), true;
    if (k == ":" || k == "/") {
        begin_command(k[0]);
        return true;
    }
    if (k == "\x10") return history_step(-1), true;
    if (k == "\x0e") return history_step(1), true;
    static const char* motions[] = {"w", "b", "e", "h", "l", "0", "^", "$", "j", "k"};
    std::string key = e == Event::ArrowLeft ? "h" : e == Event::ArrowRight ? "l" : e == Event::ArrowUp ? "k" : e == Event::ArrowDown ? "j"
                      : e == Event::Return ? "j" : k;
    if (std::find(std::begin(motions), std::end(motions), key) != std::end(motions)) {
        bool inclusive = false;
        motion_target(key, n, inclusive);
        return clamp_normal(), true;
    }
    return false;
}

bool Editor::handle_visual(const Event& e) {
    std::string k = e.input();
    if (!find_pending_.empty() && e != Event::Escape && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) return handle_find(k, std::max(count_, 1)), count_ = 0, true;
    if (k == "f" || k == "F" || k == "t" || k == "T" || k == ";" || k == ",") return handle_find(k, std::max(count_, 1)), count_ = 0, true;
    if (e == Event::Escape || k == "v" || k == "V") {
        if (e == Event::Escape) find_pending_.clear();
        if (k == "V" && mode_ == Mode::Visual) return mode_ = Mode::VisualLine, true;
        if (k == "v" && mode_ == Mode::VisualLine) return mode_ = Mode::Visual, true;
        mode_ = Mode::Normal;
        return clamp_normal(), true;
    }
    if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
        count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
        return true;
    }
    int n = std::max(count_, 1);
    count_ = 0;
    if (leader_pending_) {
        leader_pending_ = false;
        if (k == "y") clip_next_ = true, k = "y";
        else return true;
    } else if (!leader_.empty() && k == leader_) {
        return leader_pending_ = true, true;
    }
    if (pending_ == "i" || pending_ == "a") {  // a text object extends the selection: viw, va"
        char scope = pending_[0];
        pending_.clear();
        size_t from, to;
        if (text_object(scope, k.empty() ? 0 : k[0], from, to) && to > from) {
            anchor_ = from;
            cursor_ = utf8_prev(text_, to);
        }
        return true;
    }
    if (pending_ == "\"") {
        pending_.clear();
        if (k == "+" || k == "*") clip_next_ = true;
        return true;
    }
    if (k == "\"") return pending_ = "\"", true;
    if (k == "i" || k == "a") return pending_ = k, true;
    auto [a, b] = selection();
    if (k == "y" || k == "d" || k == "x" || k == "c") {
        if (k == "y") {
            yank_range(a, b);
            cursor_ = a;
            mode_ = Mode::Normal;
            return clamp_normal(), true;
        }
        save_undo();
        yank_range(a, b);
        delete_range(a, b, false);
        if (k == "c") enter_insert(cursor_, false);
        else mode_ = Mode::Normal, clamp_normal();
        return true;
    }
    if (k == "o") return std::swap(anchor_, cursor_), true;
    if (k == "p") {
        save_undo();
        std::string reg = *register_;
        delete_range(a, b, true);
        text_.insert(cursor_, reg);
        cursor_ += reg.size();
        mode_ = Mode::Normal;
        return clamp_normal(), true;
    }
    if (k == "G") return cursor_ = text_.empty() ? 0 : utf8_prev(text_, text_.size()), true;
    if (k == "g") return pending_ = "g", true;
    if (pending_ == "g") {
        pending_.clear();
        if (k == "g") cursor_ = 0;
        return true;
    }
    static const char* motions[] = {"w", "b", "e", "h", "l", "0", "^", "$", "j", "k"};
    std::string key = e == Event::ArrowLeft ? "h" : e == Event::ArrowRight ? "l" : e == Event::ArrowUp ? "k" : e == Event::ArrowDown ? "j" : k;
    if (std::find(std::begin(motions), std::end(motions), key) != std::end(motions)) {
        bool inclusive = false;
        motion_target(key, n, inclusive);
        return clamp_normal(), true;
    }
    return true;
}

bool Editor::handle_command(const Event& e, Result& result) {
    if (e == Event::Escape || (e == Event::Backspace && cmdline_.empty())) return mode_ = Mode::Normal, true;
    if (e == Event::Return) {
        mode_ = Mode::Normal;
        result.action = cmd_prefix_ == '/' ? Action::Search : Action::Command;
        result.text = cmdline_;
        return true;
    }
    if (e == Event::Backspace) return cmdline_.erase(utf8_prev(cmdline_, cmdline_.size())), true;
    if (e.input() == "\x15") return cmdline_.clear(), true;
    if (e.is_character() && static_cast<unsigned char>(e.input()[0]) >= 0x20) return cmdline_ += e.input(), true;
    return true;
}

}  // namespace maic
