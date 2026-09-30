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

namespace {

bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

}  // namespace

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
    op_.clear();
    pending_.clear();
    count_ = 0;
    op_count_ = 0;
    reg_sel_ = 0;
    clip_next_ = false;
    leader_pending_ = false;
    insert_once_ = 0;
    insert_reg_pending_ = false;
    undo_.clear();
    redo_.clear();
    marks_.clear();
    prev_pos_ = 0;
    keys_.clear();
    rec_groups_.clear();
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
    no_repeat_ = true;
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
    if (mode_ == Mode::Insert || mode_ == Mode::Replace) leave_insert();
    mode_ = Mode::Normal;
    op_.clear();
    pending_.clear();
    count_ = 0;
    leader_pending_ = false;
    clip_next_ = false;
    reg_sel_ = 0;
    insert_once_ = 0;
    insert_reg_pending_ = false;
    clamp_normal();
    finish_command();
}

void Editor::begin_command(char prefix) {
    mode_ = Mode::Command;
    cmd_prefix_ = prefix;
    cmdline_.clear();
}

void Editor::clamp_normal() {
    if (mode_ == Mode::Insert || mode_ == Mode::Command || mode_ == Mode::Replace || insert_once_) return;
    if (cursor_ >= text_.size()) cursor_ = text_.empty() ? 0 : utf8_prev(text_, text_.size());
    // Normal mode keeps the cursor off the line break, except on an empty line.
    if (mode_ == Mode::Normal && cursor_ < text_.size() && text_[cursor_] == '\n' && cursor_ > 0 && text_[cursor_ - 1] != '\n') {
        cursor_ = utf8_prev(text_, cursor_);
    }
}

bool Editor::idle() const {
    return op_.empty() && pending_.empty() && count_ == 0 && !leader_pending_ && !clip_next_ && reg_sel_ == 0 && !insert_reg_pending_;
}

bool Editor::wants_char() const {
    return pending_.size() == 1 && std::string("rm\"fFtT'`ia").find(pending_[0]) != std::string::npos;
}

void Editor::enter_insert(size_t at, bool snapshot, int repeat, char kind) {
    if (snapshot) save_undo();  // the whole insert session undoes as one step
    cursor_ = std::min(at, text_.size());
    mode_ = Mode::Insert;
    insert_repeat_ = repeat;
    insert_kind_ = kind;
    session_keys_.clear();
    replaced_.clear();
}

// Esc from insert or replace mode: a count on i a o O R repeats what was typed, then the cursor steps back.
void Editor::leave_insert() {
    if (insert_repeat_ > 1) {
        std::vector<Event> typed = session_keys_;
        int reps = insert_repeat_ - 1;
        insert_repeat_ = 1;
        for (int i = 0; i < reps; ++i) {
            if (insert_kind_ == 'o' || insert_kind_ == 'O') {
                insert_text(cursor_, "\n");
                ++cursor_;
            }
            for (const auto& e : typed) handle_insert(e);
        }
    }
    insert_repeat_ = 1;
    insert_kind_ = 0;
    session_keys_.clear();
    replaced_.clear();
    insert_reg_pending_ = false;
    if (cursor_ > 0 && text_[cursor_ - 1] != '\n') cursor_ = utf8_prev(text_, cursor_);
    mode_ = Mode::Normal;
    clamp_normal();
}

void Editor::insert_text(size_t at, const std::string& s) {
    text_.insert(at, s);
    for (auto& [c, p] : marks_) {
        if (p > at) p += s.size();
    }
    if (prev_pos_ > at) prev_pos_ += s.size();
}

void Editor::erase_text(size_t from, size_t to) {
    text_.erase(from, to - from);
    auto shift = [&](size_t p) { return p >= to ? p - (to - from) : p > from ? from : p; };
    for (auto& [c, p] : marks_) p = shift(p);
    prev_pos_ = shift(prev_pos_);
}

void Editor::delete_range(size_t from, size_t to, bool yank) {
    if (from > to) std::swap(from, to);
    to = std::min(to, text_.size());
    if (yank && to > from) yank_range(from, to);
    erase_text(from, to);
    cursor_ = std::min(from, text_.size());
}

Editor::Register Editor::current_register() const {
    if (clip_next_) return {paste_from_clipboard(), false};
    if (reg_sel_) {
        auto it = registers_.find(static_cast<char>(std::tolower(static_cast<unsigned char>(reg_sel_))));
        return it == registers_.end() ? Register{} : it->second;
    }
    return {*register_, unnamed_linewise_ && *register_ == unnamed_text_};
}

void Editor::yank_range(size_t from, size_t to, bool linewise) {
    if (from > to) std::swap(from, to);
    to = std::min(to, text_.size());
    Register r{text_.substr(from, to - from), linewise};
    if (reg_sel_ >= 'a' && reg_sel_ <= 'z') registers_[reg_sel_] = r;
    else if (reg_sel_ >= 'A' && reg_sel_ <= 'Z') {  // "A appends to "a
        Register& t = registers_[static_cast<char>(reg_sel_ - 'A' + 'a')];
        if ((t.linewise || r.linewise) && !t.text.empty()) t.text += "\n";
        t.text += r.text;
        t.linewise = t.linewise || r.linewise;
        r = t;
    }
    *register_ = r.text;  // the unnamed register follows every yank and delete, as in vim
    unnamed_text_ = r.text;
    unnamed_linewise_ = r.linewise;
    if (clip_next_) copy_to_clipboard(r.text);
    clip_next_ = false;
    reg_sel_ = 0;
}

void Editor::insert_register(const Register& r) {
    std::string s = r.text + (r.linewise ? "\n" : "");
    insert_text(cursor_, s);
    cursor_ += s.size();
}

void Editor::paste(bool after, int n) {
    Register src = current_register();
    clip_next_ = false;
    reg_sel_ = 0;
    if (src.text.empty()) return;
    save_undo();
    if (src.linewise) {
        std::string block;
        for (int i = 0; i < n; ++i) block += (i ? "\n" : "") + src.text;
        if (text_.empty()) {
            insert_text(0, block);
            cursor_ = first_nonblank(0);
        } else if (after) {
            size_t e = line_finish(cursor_);
            insert_text(e, "\n" + block);
            cursor_ = first_nonblank(e + 1);
        } else {
            size_t b = line_begin(cursor_);
            insert_text(b, block + "\n");
            cursor_ = first_nonblank(b);
        }
        clamp_normal();
        return;
    }
    std::string s;
    for (int i = 0; i < n; ++i) s += src.text;
    size_t at = after && cursor_ < text_.size() && text_[cursor_] != '\n' ? utf8_next(text_, cursor_) : cursor_;
    insert_text(at, s);
    cursor_ = at + s.size();
    if (s.find('\n') == std::string::npos) cursor_ = utf8_prev(text_, cursor_);  // on the last pasted character
    else cursor_ = at;
    clamp_normal();
}

size_t Editor::line_begin(size_t pos) const {
    pos = std::min(pos, text_.size());
    while (pos > 0 && text_[pos - 1] != '\n') --pos;
    return pos;
}

size_t Editor::line_finish(size_t pos) const {
    while (pos < text_.size() && text_[pos] != '\n') ++pos;
    return pos;
}

size_t Editor::first_nonblank(size_t pos) const {
    pos = line_begin(pos);
    while (pos < text_.size() && is_blank(text_[pos])) ++pos;
    return pos;
}

size_t Editor::next_line(size_t pos) const {
    size_t e = line_finish(pos);
    return e < text_.size() ? e + 1 : std::string::npos;
}

bool Editor::blank_line(size_t pos) const {
    return pos >= text_.size() || text_[pos] == '\n';
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

// ge: back to the end of the previous word.
void Editor::word_end_backward() {
    if (cursor_ == 0) return;
    size_t p = std::min(cursor_, text_.size() - 1);
    int cls = char_class(text_[p]);
    while (p > 0 && cls != 0 && char_class(text_[utf8_prev(text_, p)]) == cls) p = utf8_prev(text_, p);
    if (p > 0) p = utf8_prev(text_, p);
    while (p > 0 && char_class(text_[p]) == 0) p = utf8_prev(text_, p);
    cursor_ = p;
}

// End of the word under the cursor (no advance first): what `cw` changes.
void Editor::current_word_end() {
    if (cursor_ >= text_.size()) return;
    int cls = char_class(text_[cursor_]);
    while (utf8_next(text_, cursor_) < text_.size() && char_class(text_[utf8_next(text_, cursor_)]) == cls) cursor_ = utf8_next(text_, cursor_);
}

void Editor::line_start() {
    cursor_ = line_begin(cursor_);
}

void Editor::line_end() {
    cursor_ = line_finish(cursor_);
}

// f F t T on the cursor line. A repeat (; ,) of t or T skips the character right next to the cursor, as vim does
// without cpo-;, so it moves instead of standing still.
size_t Editor::find_char(const std::string& kind, const std::string& ch, int n, bool repeat) const {
    if (ch.empty() || text_.empty()) return std::string::npos;
    bool forward = kind == "f" || kind == "t";
    size_t ls = line_begin(cursor_), le = line_finish(cursor_);
    size_t pos = cursor_;
    for (int i = 0; i < n; ++i) {
        size_t found;
        if (forward) {
            size_t from = utf8_next(text_, pos);
            if (kind == "t" && i == 0 && repeat) from = utf8_next(text_, from);
            found = text_.find(ch, from);
            if (found == std::string::npos || found >= le) return std::string::npos;
        } else {
            if (pos <= ls) return std::string::npos;
            size_t upto = utf8_prev(text_, pos);
            if (kind == "T" && i == 0 && repeat && upto > ls) upto = utf8_prev(text_, upto);
            found = text_.rfind(ch, upto);
            if (found == std::string::npos || found < ls) return std::string::npos;
        }
        pos = found;
    }
    if (kind == "t") return utf8_prev(text_, pos);
    if (kind == "T") return utf8_next(text_, pos);
    return pos;
}

// } : the next empty line after a paragraph, or the end of the text.
size_t Editor::paragraph_forward(size_t pos, int n) const {
    for (int i = 0; i < n; ++i) {
        size_t p = line_begin(pos);
        while (p < text_.size() && blank_line(p)) ++p;
        while (p < text_.size() && !blank_line(p)) p = line_finish(p) < text_.size() ? line_finish(p) + 1 : text_.size();
        pos = p;
    }
    return pos;
}

// { : the empty line before the paragraph, or the start of the text.
size_t Editor::paragraph_backward(size_t pos, int n) const {
    for (int i = 0; i < n; ++i) {
        size_t p = line_begin(pos);
        if (p == 0) return 0;
        p = line_begin(p - 1);
        while (p > 0 && blank_line(p)) p = line_begin(p - 1);
        while (p > 0 && !blank_line(p)) p = line_begin(p - 1);
        pos = p;
    }
    return pos;
}

// Where sentences begin: after . ! ? (plus closing ) ] " ') followed by white space, and around empty lines.
std::vector<size_t> Editor::sentence_starts() const {
    std::vector<size_t> out;
    if (text_.empty()) return out;
    auto next_nonspace = [&](size_t i) {
        while (i < text_.size() && is_space(text_[i])) ++i;
        return i;
    };
    out.push_back(0);
    for (size_t i = 0; i < text_.size(); ++i) {
        char c = text_[i];
        if (c == '\n' && (i == 0 || text_[i - 1] == '\n')) {
            out.push_back(i);
            out.push_back(next_nonspace(i + 1));
        } else if (c == '.' || c == '!' || c == '?') {
            size_t j = i + 1;
            while (j < text_.size() && (text_[j] == ')' || text_[j] == ']' || text_[j] == '"' || text_[j] == '\'')) ++j;
            if (j >= text_.size() || is_space(text_[j])) out.push_back(next_nonspace(j));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    while (!out.empty() && out.back() >= text_.size()) out.pop_back();
    return out;
}

size_t Editor::sentence_forward(size_t pos, int n) const {
    auto starts = sentence_starts();
    for (int i = 0; i < n; ++i) {
        auto it = std::upper_bound(starts.begin(), starts.end(), pos);
        pos = it == starts.end() ? text_.size() : *it;
    }
    return pos;
}

size_t Editor::sentence_backward(size_t pos, int n) const {
    auto starts = sentence_starts();
    for (int i = 0; i < n; ++i) {
        auto it = std::lower_bound(starts.begin(), starts.end(), pos);
        pos = it == starts.begin() ? 0 : *(it - 1);
    }
    return pos;
}

bool Editor::text_object(char scope, char obj, int n, Range& r) const {
    if (text_.empty()) return false;
    r = Range{};
    r.object = true;
    size_t cur = std::min(cursor_, text_.size() - 1);
    size_t& from = r.from;
    size_t& to = r.to;
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
    if (obj == 'p') {  // lines: the paragraph (or run of empty lines) under the cursor, and n-1 more runs
        bool blank = blank_line(line_begin(cur));
        size_t s = line_begin(cur);
        while (s > 0 && blank_line(line_begin(s - 1)) == blank) s = line_begin(s - 1);
        auto run_end = [&](size_t start, bool kind) {
            size_t last = start;
            for (size_t q = next_line(start); q != std::string::npos && blank_line(q) == kind; q = next_line(q)) last = q;
            return last;
        };
        size_t e = run_end(line_begin(cur), blank);
        for (int i = 1; i < n; ++i) {
            size_t q = next_line(e);
            if (q == std::string::npos) break;
            e = run_end(q, blank_line(q));
        }
        if (scope == 'a') {
            size_t q = next_line(e);
            if (q != std::string::npos && blank_line(q) != blank_line(e)) e = run_end(q, blank_line(q));
            else if (!blank) while (s > 0 && blank_line(line_begin(s - 1))) s = line_begin(s - 1);
        }
        from = s;
        to = line_finish(e);
        r.linewise = true;
        return true;
    }
    if (obj == 's') {
        auto starts = sentence_starts();
        size_t i = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), cur) - starts.begin()) - 1;
        auto end_of = [&](size_t idx) { return idx + 1 < starts.size() ? starts[idx + 1] : text_.size(); };
        auto trim = [&](size_t s, size_t e) {
            while (e > s && is_space(text_[e - 1])) --e;
            return e;
        };
        // Trailing white space stops before an empty line: that belongs to the paragraph, not the sentence.
        auto space_end = [&](size_t s, size_t e) {
            size_t q = s;
            while (q < e && !(text_[q] == '\n' && (q + 1 >= text_.size() || text_[q + 1] == '\n'))) ++q;
            return q;
        };
        size_t s = starts[i], e = end_of(i), te = trim(s, e);
        if (cur >= te) {  // in the white space between sentences: `is` takes it, `as` adds the sentence after
            from = te;
            if (scope == 'i') to = std::max(space_end(te, e), te + 1);
            else to = i + n < starts.size() ? trim(starts[i + n], end_of(i + n)) : text_.size();
            return true;
        }
        size_t last = std::min(i + static_cast<size_t>(n) - 1, starts.size() - 1);
        size_t le = end_of(last), lte = trim(starts[last], le);
        from = s;
        to = lte;
        if (scope == 'a') {
            size_t we = space_end(lte, le);
            if (we > lte) to = we;
            else while (from > 0 && is_blank(text_[from - 1])) --from;  // no white space after: take the space before
        }
        return true;
    }
    if (obj == '"' || obj == '\'' || obj == '`') {
        // The pair on this line around the cursor: the quote at or before the cursor opens, the next one closes.
        size_t ls = line_begin(cur), le = line_finish(cur);
        size_t open;
        // Count quotes before the cursor: an even count means the cursor sits before a pair that starts after it.
        size_t count = 0, last = std::string::npos;
        for (size_t i = ls; i < cur; ++i) {
            if (text_[i] == obj) ++count, last = i;
        }
        if (text_[cur] == obj) open = (count % 2 == 1) ? last : cur;
        else if (count % 2 == 1) open = last;
        else open = text_.find(obj, cur);
        if (open == std::string::npos || open >= le) return false;
        size_t close = text_.find(obj, open + 1);
        if (close == std::string::npos || close >= le) return false;
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

Editor::Parse Editor::motion(const std::string& k, int count, bool objects, Range& r) {
    int n = std::max(count, 1);
    r = Range{};
    r.from = cursor_;
    std::string p = pending_;
    if (p.empty()) {
        if (k == "i" || k == "a") {
            if (!objects) return Parse::None;
            pending_ = k;
            return Parse::More;
        }
        if (k == "f" || k == "F" || k == "t" || k == "T" || k == "'" || k == "`" || k == "g") {
            pending_ = k;
            return Parse::More;
        }
    } else {
        pending_.clear();
        if (k.empty()) return Parse::None;
        if (p == "i" || p == "a") return text_object(p[0], k[0], n, r) ? Parse::Done : Parse::None;
        if (p == "g") {
            if (k == "g") {
                size_t ls = 0;
                for (int i = 1; i < count; ++i) {
                    size_t q = next_line(ls);
                    if (q == std::string::npos) break;
                    ls = q;
                }
                r.to = first_nonblank(ls);
                r.linewise = r.jump = true;
                return Parse::Done;
            }
            if (k == "e") {
                size_t save = cursor_;
                for (int i = 0; i < n; ++i) word_end_backward();
                r.to = cursor_;
                cursor_ = save;
                r.inclusive = true;
                return Parse::Done;
            }
            return Parse::None;
        }
        if (p == "'" || p == "`") {
            size_t pos;
            if (k == "'" || k == "`") pos = prev_pos_;
            else if (k[0] >= 'a' && k[0] <= 'z' && marks_.count(k[0])) pos = marks_[k[0]];
            else return Parse::None;
            pos = std::min(pos, text_.size());
            r.to = p == "'" ? first_nonblank(pos) : pos;
            r.linewise = p == "'";
            r.jump = true;
            return Parse::Done;
        }
        size_t d = find_char(p, k, n, false);
        last_find_kind_ = p;
        last_find_char_ = k;
        if (d == std::string::npos) return Parse::None;
        r.to = d;
        r.inclusive = p == "f" || p == "t";
        return Parse::Done;
    }
    if (k == ";" || k == ",") {
        if (last_find_kind_.empty()) return Parse::None;
        std::string kind = last_find_kind_;
        if (k == ",") kind = kind == "f" ? "F" : kind == "F" ? "f" : kind == "t" ? "T" : "t";
        size_t d = find_char(kind, last_find_char_, n, true);
        if (d == std::string::npos) return Parse::None;
        r.to = d;
        r.inclusive = kind == "f" || kind == "t";
        return Parse::Done;
    }
    if (k == "w" || k == "b" || k == "e") {
        size_t save = cursor_;
        for (int i = 0; i < n; ++i) {
            if (k == "w") word_forward();
            else if (k == "b") word_backward();
            else word_end();
        }
        r.to = cursor_;
        cursor_ = save;
        r.inclusive = k == "e";
        return Parse::Done;
    }
    if (k == "h" || k == "l") {
        size_t ls = line_begin(cursor_), le = line_finish(cursor_);
        r.to = cursor_;
        for (int i = 0; i < n; ++i) {
            if (k == "h" && r.to > ls) r.to = utf8_prev(text_, r.to);
            else if (k == "l" && r.to < le) r.to = utf8_next(text_, r.to);
        }
        return Parse::Done;
    }
    if (k == "j" || k == "k") {
        size_t ls = line_begin(cursor_), col = cursor_ - ls;
        int moved = 0;
        for (int i = 0; i < n; ++i) {
            if (k == "j") {
                size_t q = next_line(ls);
                if (q == std::string::npos) break;
                ls = q;
            } else {
                if (ls == 0) break;
                ls = line_begin(ls - 1);
            }
            ++moved;
        }
        if (!moved) return Parse::None;
        r.to = std::min(ls + col, line_finish(ls));
        r.linewise = true;
        return Parse::Done;
    }
    if (k == "0") return r.to = line_begin(cursor_), Parse::Done;
    if (k == "^") return r.to = first_nonblank(cursor_), Parse::Done;
    if (k == "$") {
        size_t e = line_finish(cursor_);
        for (int i = 1; i < n; ++i) {
            size_t q = next_line(e);
            if (q == std::string::npos) break;
            e = line_finish(q);
        }
        if (e > line_begin(e) && !insert_once_) {  // after Ctrl-O the cursor may sit past the last character
            r.to = utf8_prev(text_, e);
            r.inclusive = true;
        } else {
            r.to = e;
        }
        return Parse::Done;
    }
    if (k == "G") {
        size_t ls = line_begin(text_.size());
        if (count > 0) {
            ls = 0;
            for (int i = 1; i < count; ++i) {
                size_t q = next_line(ls);
                if (q == std::string::npos) break;
                ls = q;
            }
        }
        r.to = first_nonblank(ls);
        r.linewise = r.jump = true;
        return Parse::Done;
    }
    if (k == "+" || k == "-") {  // first non-blank of the line below / above (Enter is +)
        size_t ls = line_begin(cursor_);
        int moved = 0;
        for (int i = 0; i < n; ++i) {
            if (k == "+") {
                size_t q = next_line(ls);
                if (q == std::string::npos) break;
                ls = q;
            } else {
                if (ls == 0) break;
                ls = line_begin(ls - 1);
            }
            ++moved;
        }
        if (!moved) return Parse::None;
        r.to = first_nonblank(ls);
        r.linewise = true;
        return Parse::Done;
    }
    if (k == "}" || k == "{") {
        r.to = k == "}" ? paragraph_forward(cursor_, n) : paragraph_backward(cursor_, n);
        r.jump = true;
        return Parse::Done;
    }
    if (k == ")" || k == "(") {
        r.to = k == ")" ? sentence_forward(cursor_, n) : sentence_backward(cursor_, n);
        r.jump = true;
        return Parse::Done;
    }
    return Parse::None;
}

void Editor::move_to(const Range& r) {
    if (r.jump) prev_pos_ = cursor_;
    cursor_ = std::min(r.to, text_.size());
    clamp_normal();
}

void Editor::line_range(int n, Range& r) const {
    r = Range{};
    r.from = line_begin(cursor_);
    size_t e = r.from;
    for (int i = 1; i < n; ++i) {
        size_t q = next_line(e);
        if (q == std::string::npos) break;
        e = q;
    }
    r.to = line_finish(e);
    r.linewise = true;
}

void Editor::change_case(size_t from, size_t to, char how) {
    for (size_t i = from; i < to && i < text_.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text_[i]);
        if (c >= 0x80) continue;
        if (how == 'u') text_[i] = static_cast<char>(std::tolower(c));
        else if (how == 'U') text_[i] = static_cast<char>(std::toupper(c));
        else text_[i] = static_cast<char>(std::isupper(c) ? std::tolower(c) : std::toupper(c));
    }
}

void Editor::shift_lines(size_t from, size_t to, int dir, int times) {
    std::vector<size_t> starts;
    for (size_t ls = line_begin(from); ls <= to && ls != std::string::npos; ls = next_line(ls)) {
        starts.push_back(ls);
        if (line_finish(ls) >= to) break;
    }
    size_t width = static_cast<size_t>(std::max(shiftwidth_, 1) * times);
    for (size_t i = starts.size(); i-- > 0;) {
        size_t ls = starts[i];
        if (dir > 0) {
            if (!blank_line(ls)) insert_text(ls, std::string(width, ' '));
        } else {
            size_t e = ls;
            while (e < text_.size() && e - ls < width && is_blank(text_[e])) ++e;
            erase_text(ls, e);
        }
    }
}

// gq: re-wrap the lines from `from` to `to` at textwidth; empty lines separate paragraphs and stay.
void Editor::format_lines(size_t from, size_t to) {
    size_t s = line_begin(from), e = line_finish(to);
    std::vector<std::string> lines;
    for (size_t p = s; p <= e;) {
        size_t nl = text_.find('\n', p);
        size_t stop = nl == std::string::npos || nl > e ? e : nl;
        lines.push_back(text_.substr(p, stop - p));
        if (stop >= e) break;
        p = stop + 1;
    }
    size_t width = static_cast<size_t>(textwidth_ > 0 ? textwidth_ : 79);
    auto empty = [](const std::string& l) { return l.find_first_not_of(" \t") == std::string::npos; };
    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size();) {
        if (empty(lines[i])) {
            out.push_back("");
            ++i;
            continue;
        }
        std::string indent = lines[i].substr(0, lines[i].find_first_not_of(" \t"));
        std::string cur = indent;
        for (; i < lines.size() && !empty(lines[i]); ++i) {
            const std::string& l = lines[i];
            for (size_t w = 0; w < l.size();) {
                w = l.find_first_not_of(" \t", w);
                if (w == std::string::npos) break;
                size_t end = l.find_first_of(" \t", w);
                if (end == std::string::npos) end = l.size();
                std::string word = l.substr(w, end - w);
                if (cur.size() == indent.size()) cur += word;
                else if (utf8_len(cur) + 1 + utf8_len(word) <= width) cur += " " + word;
                else out.push_back(cur), cur = indent + word;
                w = end;
            }
        }
        out.push_back(cur);
    }
    std::string joined;
    for (size_t i = 0; i < out.size(); ++i) joined += (i ? "\n" : "") + out[i];
    erase_text(s, e);
    insert_text(s, joined);
    cursor_ = first_nonblank(s + joined.size() - (out.empty() ? 0 : out.back().size()));
}

// J: join n lines (at least two). One space between them, none when the line already ends in white space,
// the next line is empty or starts with ')'.
void Editor::join_lines(int n) {
    int joins = std::max(n - 1, 1);
    if (line_finish(cursor_) >= text_.size()) return;
    save_undo();
    for (int i = 0; i < joins; ++i) {
        size_t e = line_finish(cursor_);
        if (e >= text_.size()) break;
        size_t s = e + 1;
        while (s < text_.size() && is_blank(text_[s])) ++s;
        bool next_empty = s >= text_.size() || text_[s] == '\n';
        bool ends_in_space = e > line_begin(e) && is_blank(text_[e - 1]);
        bool cur_empty = e == line_begin(e);
        std::string sep = next_empty || ends_in_space || cur_empty || text_[s] == ')' ? "" : " ";
        erase_text(e, s);
        insert_text(e, sep);
        cursor_ = e;
    }
    clamp_normal();
}

// r{c}: replace n characters with c; r<CR> replaces them with one line break.
void Editor::replace_chars(const std::string& ch, int n) {
    size_t end = cursor_;
    for (int i = 0; i < n; ++i) {
        if (end >= text_.size() || text_[end] == '\n') return;  // not enough characters: nothing happens
        end = utf8_next(text_, end);
    }
    save_undo();
    erase_text(cursor_, end);
    if (ch == "\n") {
        insert_text(cursor_, "\n");
        cursor_ = first_nonblank(cursor_ + 1);
        return;
    }
    std::string s;
    for (int i = 0; i < n; ++i) s += ch;
    insert_text(cursor_, s);
    cursor_ += s.size() - ch.size();
}

void Editor::apply_operator(const std::string& op, Range r, int n) {
    size_t a = std::min(r.from, r.to), b = std::max(r.from, r.to);
    if (r.inclusive) b = utf8_next(text_, b);
    bool linewise = r.linewise;
    size_t col = cursor_ - line_begin(cursor_);
    // vim's rule for exclusive motions that end at the start of a line: the line break is not included, and
    // when the motion also started at or before the first non-blank the whole thing is linewise (d} on a paragraph).
    if (!linewise && !r.inclusive && b > a && b > 0 && text_[b - 1] == '\n') {
        linewise = a <= first_nonblank(a);
        --b;
    }
    if (linewise) {
        a = line_begin(a);
        b = line_finish(b);
    }
    b = std::min(b, text_.size());
    auto at_line = [&](size_t ls) { return std::min(ls + col, line_finish(ls)); };
    if (op == "y") {
        yank_range(a, b, linewise);
        if (cursor_ > a) cursor_ = linewise ? at_line(line_begin(a)) : a;
        clamp_normal();
        return;
    }
    save_undo();
    if (op == "d" || op == "c") {
        yank_range(a, b, linewise);
        if (linewise && op == "d") {
            if (b < text_.size()) erase_text(a, b + 1);
            else erase_text(a > 0 ? a - 1 : a, b);
            cursor_ = first_nonblank(std::min(a, text_.size()));
            clamp_normal();
        } else {
            erase_text(a, b);
            cursor_ = a;
            if (op == "c") enter_insert(a, false);
            else clamp_normal();
        }
        return;
    }
    if (op == "gq") {
        format_lines(a, b);
        return;
    }
    if (op == ">" || op == "<") {
        shift_lines(a, b, op == ">" ? 1 : -1, n);
        cursor_ = first_nonblank(a);
        clamp_normal();
        return;
    }
    change_case(a, b, op[1]);  // gu gU g~
    if (cursor_ > a) cursor_ = linewise ? at_line(line_begin(a)) : a;
    clamp_normal();
}

void Editor::repeat_change(int count) {
    no_repeat_ = true;
    if (last_change_.empty()) return;
    std::vector<Event> ks = last_change_;
    int c = count > 0 ? count : last_count_;
    replaying_ = true;
    if (c > 0) {
        for (char d : std::to_string(c)) handle(Event::Character(std::string(1, d)));
    }
    for (const auto& e : ks) handle(e);
    replaying_ = false;
}

// After every key: Ctrl-O's one command returns to insert mode, and a finished command that changed the text
// becomes what `.` repeats.
void Editor::finish_command() {
    if (mode_ == Mode::Insert || mode_ == Mode::Replace) insert_once_ = 0;
    else if (insert_once_ == 1) insert_once_ = 2;  // the Ctrl-O itself
    else if (insert_once_ == 2 && mode_ == Mode::Normal && idle()) {
        insert_once_ = 0;
        mode_ = Mode::Insert;
    }
    if (replaying_) return;
    if (mode_ != Mode::Normal && mode_ != Mode::Command) return;  // an insert session or a selection is still part of the change
    if (mode_ == Mode::Normal && !idle()) return;
    if (!keys_.empty() && !no_repeat_ && text_ != change_start_) {
        last_change_ = keys_;
        last_count_ = 0;
        for (int g : rec_groups_) last_count_ = last_count_ ? last_count_ * g : g;
    }
    keys_.clear();
    rec_groups_.clear();
    no_repeat_ = false;
}

Editor::Result Editor::handle(const Event& e) {
    Result result;
    if (!replaying_ && mode_ != Mode::Command) {
        if (keys_.empty() && rec_groups_.empty()) change_start_ = text_;
        keys_.push_back(e);
    }
    switch (mode_) {
        case Mode::Insert:
        case Mode::Replace: handle_insert(e); break;
        case Mode::Normal: handle_normal(e); break;
        case Mode::Visual:
        case Mode::VisualLine: handle_visual(e); break;
        case Mode::Command: handle_command(e, result); break;
    }
    finish_command();
    return result;
}

bool Editor::handle_insert(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) return leave_insert(), true;
    session_keys_.push_back(e);
    if (insert_reg_pending_) {  // Ctrl-R {register}
        insert_reg_pending_ = false;
        if (k.size() != 1) return true;
        char c = k[0];
        Register r;
        if (std::isalpha(static_cast<unsigned char>(c))) {
            auto it = registers_.find(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (it != registers_.end()) r = it->second;
        } else if (c == '+' || c == '*') {
            r = {paste_from_clipboard(), false};
        } else if (c == '"' || c == '0') {
            r = {*register_, unnamed_linewise_ && *register_ == unnamed_text_};
        }
        insert_register(r);
        return true;
    }
    if (k == "\x12") return insert_reg_pending_ = true, true;  // Ctrl-R
    if (k == "\x0f") {  // Ctrl-O: one normal-mode command
        session_keys_.pop_back();
        insert_repeat_ = 1;
        mode_ = Mode::Normal;
        insert_once_ = 1;
        return true;
    }
    if (mode_ == Mode::Replace) {
        if (e == Event::Backspace) {  // steps back over what was typed and restores what it covered
            if (replaced_.empty()) {
                if (cursor_ > line_begin(cursor_)) cursor_ = utf8_prev(text_, cursor_);
                return true;
            }
            std::string original = replaced_.back();
            replaced_.pop_back();
            size_t prev = utf8_prev(text_, cursor_);
            erase_text(prev, cursor_);
            insert_text(prev, original);
            cursor_ = prev;
            return true;
        }
        if (e == Event::Return) {
            insert_text(cursor_, "\n");
            ++cursor_;
            replaced_.push_back("");
            return true;
        }
        if (e.is_character() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) {
            if (cursor_ < text_.size() && text_[cursor_] != '\n') {
                size_t next = utf8_next(text_, cursor_);
                replaced_.push_back(text_.substr(cursor_, next - cursor_));
                erase_text(cursor_, next);
            } else {
                replaced_.push_back("");
            }
            insert_text(cursor_, k);
            cursor_ += k.size();
            return true;
        }
    }
    if (e == Event::Return) {
        insert_text(cursor_, "\n");
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
        delete_range(cursor_, end, false);
        return true;
    }
    if (k == "\x15") return delete_range(line_begin(cursor_), cursor_, false), true;  // Ctrl-U
    if (k == "\x19") return insert_register(current_register()), true;               // Ctrl-Y: paste the register
    if (e == Event::ArrowLeft) return cursor_ = utf8_prev(text_, cursor_), true;
    if (e == Event::ArrowRight) return cursor_ = utf8_next(text_, cursor_), true;
    if (e == Event::Home) return line_start(), true;
    if (e == Event::End) return line_end(), true;
    if (e == Event::ArrowUp || k == "\x10") return history_step(-1), true;
    if (e == Event::ArrowDown || k == "\x0e") return history_step(1), true;
    if (e.is_character() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) {
        insert_text(cursor_, k);
        cursor_ += k.size();
        return true;
    }
    return false;
}

bool Editor::handle_normal(const Event& e) {
    const std::string& k = e.input();
    if (e == Event::Escape) {
        op_.clear();
        pending_.clear();
        count_ = 0;
        leader_pending_ = clip_next_ = false;
        reg_sel_ = 0;
        return true;
    }
    bool literal = wants_char();
    if (!literal) {
        if (leader_pending_) {
            leader_pending_ = false;
            if (k == "y") {  // <leader>y: the line to the clipboard
                clip_next_ = true;
                Range r;
                line_range(1, r);
                yank_range(r.from, r.to, true);
            } else if (k == "p" || k == "P") {
                clip_next_ = true;
                paste(k == "p", std::max(count_, 1));
            }
            count_ = 0;
            return true;
        }
        if (!leader_.empty() && k == leader_ && op_.empty() && pending_.empty()) return leader_pending_ = true, true;
        if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
            if (!replaying_ && !keys_.empty()) keys_.pop_back();  // counts are kept apart from the keys `.` replays
            if (count_ == 0) rec_groups_.push_back(0);
            count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
            rec_groups_.back() = count_;
            return true;
        }
    }
    int raw = count_;
    count_ = 0;
    int n = std::max(raw, 1);

    if (literal && (pending_ == "\"" || pending_ == "m" || pending_ == "r")) {
        std::string arg = pending_;
        pending_.clear();
        if (arg == "r") {
            if (e == Event::Return) replace_chars("\n", n);
            else if (e.is_character() && !k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) replace_chars(k, n);
            return true;
        }
        if (k.size() != 1) return true;
        char c = k[0];
        if (arg == "m") {
            if (c >= 'a' && c <= 'z') marks_[c] = cursor_;
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) reg_sel_ = c;
        else if (c == '+' || c == '*') clip_next_ = true;
        count_ = raw;
        return true;
    }
    std::string key = literal ? k : e == Event::ArrowLeft ? "h" : e == Event::ArrowRight ? "l" : e == Event::ArrowUp ? "k" : e == Event::ArrowDown ? "j"
                      : e == Event::Return ? "+" : k;

    if (!op_.empty()) {
        int count = raw && op_count_ ? raw * op_count_ : raw ? raw : op_count_;
        int cn = std::max(count, 1);
        std::string tail = op_.substr(op_.size() - 1);
        if ((pending_.empty() && key == tail) || (pending_ == "g" && op_.size() == 2 && key == tail)) {  // dd cc yy >> gqq gugu ...
            pending_.clear();
            Range r;
            line_range(cn, r);
            apply_operator(op_, r, 1);
            op_.clear();
            op_count_ = 0;
            return true;
        }
        if (op_ == "c" && pending_.empty() && key == "w" && cursor_ < text_.size() && char_class(text_[cursor_]) != 0) {
            // vim special case: cw on a word changes to the end of that word (then n-1 more words)
            Range r;
            r.from = cursor_;
            current_word_end();
            for (int i = 1; i < cn; ++i) word_end();
            r.to = cursor_;
            cursor_ = r.from;
            r.inclusive = true;
            apply_operator(op_, r, 1);
            op_.clear();
            op_count_ = 0;
            return true;
        }
        Range r;
        Parse res = motion(key, count, true, r);
        if (res == Parse::More) return count_ = raw, true;
        if (res == Parse::Done) apply_operator(op_, r, 1);
        op_.clear();
        op_count_ = 0;
        return true;
    }
    if (pending_ == "g" && (key == "q" || key == "u" || key == "U" || key == "~")) {
        pending_.clear();
        op_ = "g" + key;
        op_count_ = raw;
        return true;
    }
    if (!pending_.empty()) {  // g, or a motion waiting for its character
        Range r;
        Parse res = motion(key, raw, false, r);
        if (res == Parse::More) count_ = raw;
        else if (res == Parse::Done) move_to(r);
        return true;
    }

    if (k == "\x12") {  // Ctrl-R
        for (int i = 0; i < n; ++i) redo();
        return no_repeat_ = true, true;
    }
    if (k == "u") {
        for (int i = 0; i < n; ++i) undo();
        return no_repeat_ = true, true;
    }
    if (k == ".") return repeat_change(raw), true;
    if (k == "\"" || k == "r" || k == "m") return pending_ = k, count_ = raw, true;
    if (k == "i") return enter_insert(cursor_, true, n, 'i'), true;
    if (k == "a") return enter_insert(cursor_ < text_.size() && text_[cursor_] != '\n' ? utf8_next(text_, cursor_) : cursor_, true, n, 'a'), true;
    if (k == "I") return enter_insert(first_nonblank(cursor_), true, n, 'I'), true;
    if (k == "A") return enter_insert(line_finish(cursor_), true, n, 'A'), true;
    if (k == "o" || k == "O") {
        save_undo();
        size_t at = k == "o" ? line_finish(cursor_) : line_begin(cursor_);
        insert_text(at, "\n");
        return enter_insert(k == "o" ? at + 1 : at, false, n, k[0]), true;
    }
    if (k == "R") {
        save_undo();
        enter_insert(cursor_, false, n, 'R');
        mode_ = Mode::Replace;
        return true;
    }
    if (k == "v") return anchor_ = cursor_, mode_ = Mode::Visual, true;
    if (k == "V") return anchor_ = cursor_, mode_ = Mode::VisualLine, true;
    if (k == "p") return paste(true, n), true;
    if (k == "P") return paste(false, n), true;
    if (k == "d" || k == "c" || k == "y" || k == ">" || k == "<") return op_ = k, op_count_ = raw, true;
    if (k == "Y") {
        Range r;
        line_range(n, r);
        apply_operator("y", r, 1);
        return true;
    }
    if (k == "x" || k == "~" || k == "s") {
        size_t end = cursor_;
        for (int i = 0; i < n && end < text_.size() && text_[end] != '\n'; ++i) end = utf8_next(text_, end);
        if (end == cursor_ && k != "s") return true;
        save_undo();
        if (k == "~") {
            change_case(cursor_, end, '~');
            cursor_ = end;
            return clamp_normal(), true;
        }
        if (end > cursor_) yank_range(cursor_, end);
        erase_text(cursor_, end);
        if (k == "s") enter_insert(cursor_, false);
        else clamp_normal();
        return true;
    }
    if (k == "X") {
        size_t start = cursor_, ls = line_begin(cursor_);
        for (int i = 0; i < n && start > ls; ++i) start = utf8_prev(text_, start);
        if (start == cursor_) return true;
        save_undo();
        yank_range(start, cursor_);
        erase_text(start, cursor_);
        cursor_ = start;
        return true;
    }
    if (k == "D" || k == "C") {
        Range r;
        motion("$", raw, false, r);
        apply_operator(k == "D" ? "d" : "c", r, 1);
        return true;
    }
    if (k == "S") {
        Range r;
        line_range(n, r);
        apply_operator("c", r, 1);
        return true;
    }
    if (k == "J") return join_lines(n), true;
    if (k == ":" || k == "/") {
        begin_command(k[0]);
        return true;
    }
    if (k == "\x10") return history_step(-1), true;
    if (k == "\x0e") return history_step(1), true;
    Range r;
    Parse res = motion(key, raw, false, r);
    if (res == Parse::More) return count_ = raw, true;
    if (res == Parse::Done) return move_to(r), true;
    return false;
}

bool Editor::handle_visual(const Event& e) {
    std::string k = e.input();
    if (e == Event::Escape) {
        pending_.clear();
        count_ = 0;
        mode_ = Mode::Normal;
        return clamp_normal(), true;
    }
    bool literal = wants_char();
    if (!literal) {
        if (k == "v" || k == "V") {
            if (k == "V" && mode_ == Mode::Visual) return mode_ = Mode::VisualLine, true;
            if (k == "v" && mode_ == Mode::VisualLine) return mode_ = Mode::Visual, true;
            mode_ = Mode::Normal;
            return clamp_normal(), true;
        }
        if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
            if (!replaying_ && !keys_.empty()) keys_.pop_back();
            if (count_ == 0) rec_groups_.push_back(0);
            count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
            rec_groups_.back() = count_;
            return true;
        }
        if (leader_pending_) {
            leader_pending_ = false;
            if (k == "y") clip_next_ = true;
            else return true;
        } else if (!leader_.empty() && k == leader_ && pending_.empty()) {
            return leader_pending_ = true, true;
        }
    }
    int raw = count_;
    count_ = 0;
    int n = std::max(raw, 1);
    auto [a, b] = selection();
    Range sel;
    sel.from = a;
    sel.to = b;
    sel.linewise = mode_ == Mode::VisualLine;
    auto finish = [&](const std::string& op) {
        apply_operator(op, sel, n);
        if (mode_ != Mode::Insert) mode_ = Mode::Normal, clamp_normal();
        return true;
    };
    if (literal && (pending_ == "\"" || pending_ == "m" || pending_ == "r")) {
        std::string arg = pending_;
        pending_.clear();
        if (k.size() != 1) return true;
        char c = k[0];
        if (arg == "r") {  // every selected character becomes c
            if (static_cast<unsigned char>(c) < 0x20) return true;
            save_undo();
            std::string s;
            for (size_t i = a; i < b; i = utf8_next(text_, i)) s += text_[i] == '\n' ? "\n" : k;
            erase_text(a, b);
            insert_text(a, s);
            cursor_ = a;
            mode_ = Mode::Normal;
            return clamp_normal(), true;
        }
        if (arg == "m") {
            if (c >= 'a' && c <= 'z') marks_[c] = cursor_;
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) reg_sel_ = c;
        else if (c == '+' || c == '*') clip_next_ = true;
        return true;
    }
    if (pending_ == "g" && (k == "q" || k == "u" || k == "U" || k == "~")) {
        pending_.clear();
        return finish("g" + k);
    }
    std::string key = literal ? k : e == Event::ArrowLeft ? "h" : e == Event::ArrowRight ? "l" : e == Event::ArrowUp ? "k" : e == Event::ArrowDown ? "j" : k;
    if (pending_.empty()) {
        if (k == "y") return finish("y");
        if (k == "d" || k == "x") return finish("d");
        if (k == "c" || k == "s") return finish("c");
        if (k == "~") return finish("g~");
        if (k == "u") return finish("gu");
        if (k == "U") return finish("gU");
        if (k == ">" || k == "<") return finish(k);
        if (k == "J") {
            cursor_ = a;
            mode_ = Mode::Normal;
            join_lines(std::max<int>(2, static_cast<int>(std::count(text_.begin() + a, text_.begin() + b, '\n')) + 1));
            return true;
        }
        if (k == "\"" || k == "r" || k == "m") return pending_ = k, true;
        if (k == "o") return std::swap(anchor_, cursor_), true;
        if (k == "p" || k == "P") {
            Register src = current_register();
            clip_next_ = false;
            reg_sel_ = 0;
            save_undo();
            delete_range(a, b, true);
            std::string s = src.text + (src.linewise ? "\n" : "");
            insert_text(cursor_, s);
            cursor_ += s.size();
            mode_ = Mode::Normal;
            return clamp_normal(), true;
        }
    }
    Range r;
    Parse res = motion(key, raw, true, r);
    if (res == Parse::More) count_ = raw;
    else if (res == Parse::Done) {
        if (r.object) {
            if (r.to > r.from) {
                anchor_ = r.from;
                cursor_ = utf8_prev(text_, r.to);
                if (r.linewise) mode_ = Mode::VisualLine;
            }
        } else {
            if (r.jump) prev_pos_ = cursor_;
            cursor_ = std::min(r.to, text_.size());
            if (cursor_ == text_.size() && !text_.empty()) cursor_ = utf8_prev(text_, cursor_);
        }
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
