#include "view.hpp"

#include "editor.hpp"
#include "maid/clipboard.hpp"
#include "style.hpp"

#include <algorithm>
#include <ctime>
#include <cctype>

namespace maid {

using namespace ftxui;

namespace {

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
        case Kind::Shell: return "$ ";
    }
    return "";
}

const char* style_name(Kind k) {
    switch (k) {
        case Kind::User: return "user";
        case Kind::Assistant: return "assistant";
        case Kind::Thinking: return "thinking";
        case Kind::Tool: return "tool";
        case Kind::ToolOk: return "tool_ok";
        case Kind::ToolErr: return "tool_err";
        case Kind::Notice: return "notice";
        case Kind::Error: return "error";
        case Kind::Shell: return "shell";
    }
    return "assistant";
}

bool attached(Kind k) {
    return k == Kind::ToolOk || k == Kind::ToolErr;
}

// A subagent's tool call ("↳ explore: ...") sits indented under the task call that started it.
const char* marker(const Entry& e) {
    if (e.kind == Kind::Tool && e.text.rfind("↳", 0) == 0) return "  ";
    return prefix(e.kind);
}

constexpr size_t kPreviewLines = 8;
// A live entry: the last lines of a running command, at most this many bytes of them.
constexpr size_t kLiveLines = 12;
constexpr size_t kLiveBytes = kLiveLines * 160;

// The first lines of a tool result, with a note about the rest.
std::string preview(const std::string& text) {
    std::string out;
    size_t n = 0, total = 0, start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (nl == std::string::npos && line.empty() && start == text.size()) break;
        if (n < kPreviewLines) out += (n ? "\n" : "") + line, ++n;
        ++total;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (total > n) out += "\n… +" + std::to_string(total - n) + " lines (za shows them)";
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_tool(Kind k) {
    return k == Kind::Tool || attached(k);
}

}  // namespace

unsigned diff_flags(const std::string& line) {
    if (line.rfind("@@", 0) == 0 || line.rfind("+++ ", 0) == 0 || line.rfind("--- ", 0) == 0 || line.rfind("diff --git ", 0) == 0 || line.rfind("index ", 0) == 0) return DiffHunk;
    if (!line.empty() && line[0] == '+') return DiffAdd;
    if (!line.empty() && line[0] == '-') return DiffDel;
    return MdNone;
}

// A hunk header or a pair of file headers settles it; otherwise both an added and a removed line are needed, so
// a listing with `- ` bullets is not taken for one.
bool looks_like_diff(const std::string& text) {
    bool added = false, removed = false;
    for (size_t start = 0; start < text.size();) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        unsigned f = diff_flags(line);
        if (f == DiffHunk && (line.rfind("@@", 0) == 0 || line.rfind("diff --git ", 0) == 0)) return true;
        added = added || f == DiffAdd || line.rfind("+++ ", 0) == 0;
        removed = removed || f == DiffDel || line.rfind("--- ", 0) == 0;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return added && removed;
}

std::vector<StyledLine> diff_lines(const std::string& text) {
    auto lines = plain_lines(text);
    for (auto& l : lines) {
        unsigned f = diff_flags(line_text(l));
        for (auto& sp : l) sp.flags |= f;
    }
    return lines;
}

void View::append(Kind kind, std::string text) {
    std::lock_guard lock(mu_);
    entries_.push_back({kind, std::move(text), attached(kind) && collapse_default_, std::time(nullptr)});
    ++version_;
}

std::string View::last_assistant() const {
    std::lock_guard lock(mu_);
    for (size_t i = entries_.size(); i-- > 0;) {
        if (entries_[i].kind == Kind::Assistant) return entries_[i].text;
    }
    return "";
}

bool View::click(int row) {
    if (row < 0) return false;
    size_t idx = static_cast<size_t>(last_top_ + row);
    if (idx >= lines_.size()) return false;
    std::lock_guard lock(mu_);
    auto& e = entries_[lines_[idx].entry];
    if (!attached(e.kind)) return false;
    e.collapsed = !e.collapsed;
    ++version_;
    return true;
}

void View::set_all_collapsed(bool on) {
    std::lock_guard lock(mu_);
    for (auto& e : entries_) {
        if (attached(e.kind)) e.collapsed = on;
    }
    ++version_;
}

void View::append_to_last(Kind kind, std::string_view delta) {
    std::lock_guard lock(mu_);
    if (entries_.empty() || entries_.back().kind != kind || entries_.back().live) entries_.push_back({kind, "", false, std::time(nullptr)});
    entries_.back().text += delta;
    ++version_;
}

void View::replace_last(Kind kind, std::string text) {
    std::lock_guard lock(mu_);
    auto it = std::find_if(entries_.rbegin(), entries_.rend(), [&](const Entry& e) { return e.kind == kind; });
    if (it == entries_.rend()) return;
    it->text = std::move(text);
    ++version_;
}

void View::live_output(std::string_view text) {
    std::lock_guard lock(mu_);
    auto it = std::find_if(entries_.rbegin(), entries_.rend(), [](const Entry& e) { return e.live; });
    if (it == entries_.rend()) {
        entries_.push_back({Kind::ToolOk, "", false, std::time(nullptr), true});
        it = entries_.rbegin();
    }
    std::string& t = it->text;
    // A carriage return starts its line over, as a progress bar redraws it, and a CRLF is a newline; one that
    // ends a chunk waits at the end of the text for the next chunk to say which it is.
    std::string in;
    if (!t.empty() && t.back() == '\r') {
        t.pop_back();
        in = "\r";
    }
    in += text;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\r') {
            t += in[i];
        } else if (i + 1 == in.size()) {
            t += '\r';
        } else if (in[i + 1] != '\n') {
            size_t nl = t.rfind('\n');
            t.erase(nl == std::string::npos ? 0 : nl + 1);
        }
    }
    // From the start of the kLiveLines-th line before the end (a line still being written counts), then bytes.
    size_t from = 0, pos = t.size() && t.back() == '\n' ? t.size() - 1 : t.size();
    for (size_t n = 0; n < kLiveLines; ++n) {
        size_t nl = pos ? t.rfind('\n', pos - 1) : std::string::npos;
        if (nl == std::string::npos) {
            from = 0;
            break;
        }
        from = nl + 1;
        pos = nl;
    }
    if (t.size() - from > kLiveBytes) from = t.size() - kLiveBytes;
    while (from < t.size() && (static_cast<unsigned char>(t[from]) & 0xC0) == 0x80) ++from;  // not inside a UTF-8 character
    t.erase(0, from);
    ++version_;
}

void View::finish_live(Kind kind, std::string text, std::string full) {
    std::lock_guard lock(mu_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.live; }), entries_.end());
    entries_.push_back({kind, std::move(text), attached(kind) && collapse_default_, std::time(nullptr), false, std::move(full)});
    ++version_;
}

void View::clear() {
    std::lock_guard lock(mu_);
    entries_.clear();
    ++version_;
    scroll_ = 0;
    cur_line_ = cur_col_ = 0;
    visual_ = VisualMode::None;
    matches_.clear();
}

size_t View::size() const {
    std::lock_guard lock(mu_);
    return entries_.size();
}

void View::set_focused(bool on) {
    focused_ = on;
    visual_ = VisualMode::None;
    pending_.clear();
    if (on && !lines_.empty()) {
        // Start on the last visible line.
        int bottom = static_cast<int>(lines_.size()) - scroll_;
        cur_line_ = static_cast<size_t>(std::max(0, bottom - 1));
        cur_col_ = 0;
    }
}

void View::layout(size_t width) {
    std::vector<Entry> snapshot;
    unsigned version;
    {
        std::lock_guard lock(mu_);
        if (version_ == laid_out_version_ && width == laid_out_width_) return;
        snapshot = entries_;
        version = version_;
    }
    lines_.clear();
    for (size_t e = 0; e < snapshot.size(); ++e) {
        const auto& entry = snapshot[e];
        if (e > 0 && !attached(entry.kind)) lines_.push_back({{}, Kind::Assistant, e, 0, 0, false});
        size_t pre = utf8_len(marker(entry)) + (timestamps_ ? 6 : 0);
        bool use_md = markdown_ && (entry.kind == Kind::Assistant || entry.kind == Kind::User || entry.kind == Kind::Notice);
        std::string shown_text = entry.collapsed ? preview(entry.text) : entry.full.empty() ? entry.text : entry.full;
        bool diff = markdown_ && is_tool(entry.kind) && looks_like_diff(shown_text);
        // Tool output and shell output keep tabs; the renderer drops them, so expand.
        auto source = use_md ? markdown_lines(shown_text) : diff ? diff_lines(shown_text) : plain_lines(shown_text);
        size_t offset = 0;
        bool first = true;
        for (const auto& src : source) {
            std::string raw = line_text(src);
            StyledLine expanded;
            for (const auto& sp : src) {
                std::string t = sp.text;
                for (size_t p; (p = t.find('\t')) != std::string::npos;) t.replace(p, 1, "    ");
                expanded.push_back({t, sp.flags});
            }
            auto wrapped = wrap_line(expanded, width > pre ? width - pre : 8);
            size_t line_off = offset;
            for (size_t w = 0; w < wrapped.size(); ++w) {
                // Byte range in the shown text: walk it by the columns this row shows (a tab shows as 4).
                size_t shown = utf8_len(line_text(wrapped[w]));
                size_t end = line_off;
                size_t seen = 0;
                while (end < line_off + raw.size() && seen < shown) {
                    size_t n = utf8_next(shown_text, end);
                    seen += raw[end - line_off] == '\t' ? 4 : 1;
                    end = n;
                }
                lines_.push_back({wrapped[w], entry.kind, e, line_off, end, first});
                first = false;
                line_off = end;
                if (line_off < offset + raw.size() && shown_text[line_off] == ' ') ++line_off;  // the dropped soft-break space
            }
            offset += raw.size() + 1;  // past the newline
        }
    }
    laid_out_version_ = version;
    laid_out_width_ = width;
    if (!pattern_.empty()) find_matches();
    if (cur_line_ >= lines_.size()) cur_line_ = lines_.empty() ? 0 : lines_.size() - 1;
}

std::string View::text_of(const Line& l) const {
    std::lock_guard lock(mu_);
    if (l.entry >= entries_.size()) return "";
    const auto& e = entries_[l.entry];
    std::string t = e.collapsed ? preview(e.text) : e.full.empty() ? e.text : e.full;
    return t.substr(std::min(l.begin, t.size()), l.end > l.begin ? l.end - l.begin : 0);
}

void View::scroll_by(int lines) {
    scroll_ = std::max(0, scroll_ + lines);
}

void View::scroll_to_top() {
    scroll_ = std::max(0, static_cast<int>(lines_.size()) - last_height_);
}

void View::scroll_to_bottom() {
    scroll_ = 0;
}

void View::page(int direction) {
    scroll_by(-direction * std::max(1, last_height_ - 2));
}

void View::half_page(int direction) {
    scroll_by(-direction * std::max(1, last_height_ / 2));
}

void View::move_cursor_line(int delta) {
    if (lines_.empty()) return;
    long target = static_cast<long>(cur_line_) + delta;
    cur_line_ = static_cast<size_t>(std::clamp<long>(target, 0, static_cast<long>(lines_.size()) - 1));
    size_t w = line_width(lines_[cur_line_].spans);
    if (cur_col_ >= w) cur_col_ = w ? w - 1 : 0;
}

void View::jump_message(int direction, bool user_only, int count) {
    if (lines_.empty()) return;
    for (int c = 0; c < count; ++c) {
        size_t i = cur_line_;
        bool moved = false;
        while (direction > 0 ? i + 1 < lines_.size() : i > 0) {
            i += direction > 0 ? 1 : -1;
            const Line& l = lines_[i];
            if (l.first && l.entry != lines_[cur_line_].entry && (!user_only || l.kind == Kind::User)) {
                cur_line_ = i;
                cur_col_ = 0;
                moved = true;
                break;
            }
        }
        if (!moved) break;
    }
}

void View::ensure_cursor_visible(int height) {
    int total = static_cast<int>(lines_.size());
    int bottom = total - scroll_;
    int top = std::max(0, bottom - height);
    last_top_ = top;
    int cur = static_cast<int>(cur_line_);
    if (cur < top) scroll_ = total - (cur + height);
    else if (cur >= bottom) scroll_ = total - cur - 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - height));
}

void View::find_matches() {
    matches_.clear();
    if (pattern_.empty()) return;
    bool fold = lower(pattern_) == pattern_;  // smartcase
    std::string needle = fold ? lower(pattern_) : pattern_;
    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string hay = line_text(lines_[i].spans);
        if (fold) hay = lower(hay);
        for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
            matches_.push_back({i, utf8_len(hay.substr(0, p))});
        }
    }
}

void View::search(const std::string& pattern) {
    pattern_ = pattern;
    find_matches();
}

std::string View::search_next(int direction) {
    if (matches_.empty()) return pattern_.empty() ? "no search pattern" : "no matches for /" + pattern_;
    // First match after (or before) the cursor.
    size_t best = matches_.size();
    for (size_t i = 0; i < matches_.size(); ++i) {
        auto [l, c] = matches_[i];
        bool after = l > cur_line_ || (l == cur_line_ && c > cur_col_);
        bool before = l < cur_line_ || (l == cur_line_ && c < cur_col_);
        if (direction > 0 && after) {
            best = i;
            break;
        }
        if (direction < 0 && before) best = i;
    }
    if (best == matches_.size()) best = direction > 0 ? 0 : matches_.size() - 1;  // wrap around
    cur_line_ = matches_[best].first;
    cur_col_ = matches_[best].second;
    return "match " + std::to_string(best + 1) + " of " + std::to_string(matches_.size());
}

std::string View::yank_selection() {
    if (lines_.empty()) return "";
    size_t a = std::min(cur_line_, anchor_line_), b = std::max(cur_line_, anchor_line_);
    size_t ca = cur_line_ < anchor_line_ || (cur_line_ == anchor_line_ && cur_col_ <= anchor_col_) ? cur_col_ : anchor_col_;
    size_t cb = ca == cur_col_ && a == cur_line_ && !(cur_line_ == anchor_line_ && cur_col_ == anchor_col_) ? anchor_col_ : cur_col_;
    if (a == b) ca = std::min(cur_col_, anchor_col_), cb = std::max(cur_col_, anchor_col_);
    std::string out;
    size_t last_entry = ~size_t(0);
    for (size_t i = a; i <= b; ++i) {
        const Line& l = lines_[i];
        if (l.entry != last_entry && last_entry != ~size_t(0)) out += "\n";
        last_entry = l.entry;
        std::string t = text_of(l);
        if (visual_ == VisualMode::Char) {
            size_t from = i == a ? utf8_offset(t, ca) : 0;
            size_t to = i == b ? utf8_offset(t, cb + 1) : t.size();
            t = t.substr(std::min(from, t.size()), to > from ? to - from : 0);
        }
        // Consecutive wrapped rows of one source line rejoin with the space the wrap dropped.
        if (i > a && lines_[i - 1].entry == l.entry) {
            bool same_source = lines_[i - 1].end <= l.begin && l.begin - lines_[i - 1].end <= 1 && !l.first;
            out += same_source && lines_[i - 1].end != l.begin ? " " : same_source ? "" : "\n";
        }
        out += t;
    }
    return out;
}

// The word under the cursor on the current line, as columns.
std::string View::yank_word_range(char scope, size_t& from_col, size_t& to_col) const {
    if (lines_.empty()) return "";
    std::string t = line_text(lines_[cur_line_].spans);
    if (t.empty()) return "";
    size_t cur = std::min(utf8_offset(t, cur_col_), t.size() - 1);
    int c = char_class(t[cur]);
    size_t from = cur, to = utf8_next(t, cur);
    while (from > 0 && char_class(t[utf8_prev(t, from)]) == c) from = utf8_prev(t, from);
    while (to < t.size() && char_class(t[to]) == c) to = utf8_next(t, to);
    if (scope == 'a') {
        while (to < t.size() && t[to] == ' ') ++to;
    }
    from_col = utf8_len(t.substr(0, from));
    to_col = utf8_len(t.substr(0, to));
    return t.substr(from, to - from);
}

void View::find_on_line(const std::string& kind, const std::string& ch, int n) {
    if (lines_.empty() || ch.empty()) return;
    std::string t = line_text(lines_[cur_line_].spans);
    bool forward = kind == "f" || kind == "t";
    size_t pos = utf8_offset(t, cur_col_);
    for (int i = 0; i < n; ++i) {
        size_t found;
        if (forward) {
            size_t from = utf8_next(t, pos);
            if (kind == "t" && i == 0) from = utf8_next(t, from);
            found = t.find(ch, from);
        } else {
            if (pos == 0) return;
            size_t upto = utf8_prev(t, pos);
            if (kind == "T" && i == 0 && upto > 0) upto = utf8_prev(t, upto);
            found = t.rfind(ch, upto);
        }
        if (found == std::string::npos) return;
        pos = found;
    }
    if (kind == "t") pos = utf8_prev(t, pos);
    if (kind == "T") pos = utf8_next(t, pos);
    cur_col_ = utf8_len(t.substr(0, std::min(pos, t.size())));
}

std::string View::handle(const Event& e, int height) {
    const std::string& k = e.input();
    last_height_ = height;
    auto yank_text = [&](const std::string& text, const std::string& what) {
        *register_ = text;
        return "yanked " + what + " (" + copy_to_clipboard(text) + ")";
    };
    if (pending_ == "leader") {
        pending_.clear();
        if (k == "y" && visual_ != VisualMode::None) {
            std::string text = yank_selection();
            visual_ = VisualMode::None;
            return yank_text(text, "selection");
        }
        return "";
    }
    if (!leader_.empty() && k == leader_ && pending_.empty()) return pending_ = "leader", "";
    if (e == Event::Escape) {
        if (visual_ != VisualMode::None) return visual_ = VisualMode::None, "";
        pending_.clear();
        count_ = 0;
        return "";
    }
    if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])) && (k != "0" || count_ > 0)) {
        count_ = std::min(count_ * 10 + (k[0] - '0'), 9999);
        return "";
    }
    int n = std::max(count_, 1);
    count_ = 0;
    if (!pending_.empty()) {
        std::string op = pending_;
        pending_.clear();
        if (op == "f" || op == "F" || op == "t" || op == "T") {
            if (!k.empty() && static_cast<unsigned char>(k[0]) >= 0x20) {
                last_find_kind_ = op;
                last_find_char_ = k;
                find_on_line(op, k, n);
            }
            ensure_cursor_visible(height);
            return "";
        }
        if (op == "g" && k == "g") {
            cur_line_ = 0;
            cur_col_ = 0;
        } else if (op == "z") {
            if (k == "R") set_all_collapsed(false);
            else if (k == "M") set_all_collapsed(true);
            else if ((k == "a" || k == "o" || k == "c") && !lines_.empty()) {
                std::lock_guard lock(mu_);
                auto& e = entries_[lines_[cur_line_].entry];
                if (attached(e.kind)) {
                    e.collapsed = k == "a" ? !e.collapsed : k == "c";
                    ++version_;
                } else {
                    return "not a tool result (za folds tool output)";
                }
            }
        } else if (op == "y" && k == "y") {
            anchor_line_ = cur_line_;
            visual_ = VisualMode::Line;
            std::string text = yank_selection();
            visual_ = VisualMode::None;
            return yank_text(text, "1 line");
        } else if (op == "y" && (k == "i" || k == "a")) {
            pending_ = "y" + k;
        } else if ((op == "yi" || op == "ya") && k == "w") {
            size_t a, b;
            std::string word = yank_word_range(op[1], a, b);
            if (!word.empty()) return yank_text(word, "word");
        } else if (op == "y" && (k == "w" || k == "e" || k == "$")) {
            std::string t = lines_.empty() ? "" : line_text(lines_[cur_line_].spans);
            size_t from = utf8_offset(t, cur_col_), to = t.size();
            if (k != "$" && from < t.size()) {
                to = from;
                int c = char_class(t[to]);
                while (to < t.size() && char_class(t[to]) == c) to = utf8_next(t, to);
                if (k == "w") while (to < t.size() && char_class(t[to]) == 0) to = utf8_next(t, to);
            }
            if (to > from) return yank_text(t.substr(from, to - from), k == "$" ? "to end of line" : "word");
        } else if (op == "\"" && (k == "+" || k == "*")) {
            pending_ = "\"+";
        } else if (op == "\"+" && k == "y" && visual_ != VisualMode::None) {
            std::string text = yank_selection();
            visual_ = VisualMode::None;
            *register_ = text;
            return "yanked (" + copy_to_clipboard(text) + ")";
        }
        ensure_cursor_visible(height);
        return "";
    }
    if (k == "j" || e == Event::ArrowDown) move_cursor_line(n);
    else if (k == "k" || e == Event::ArrowUp) move_cursor_line(-n);
    else if (k == "\x04") move_cursor_line(height / 2);
    else if (k == "\x15") move_cursor_line(-height / 2);
    else if (k == "\x06" || e == Event::PageDown) move_cursor_line(height - 2);
    else if (k == "\x02" || e == Event::PageUp) move_cursor_line(-(height - 2));
    else if (k == "\x05") scroll_by(-n), (void)0;
    else if (k == "\x19") scroll_by(n), (void)0;
    else if (k == "G") move_cursor_line(static_cast<int>(lines_.size()));
    else if (k == "H" || k == "M" || k == "L") {  // top, middle, bottom of the window; a count moves in from the edge
        int total = static_cast<int>(lines_.size());
        int bottom = total - scroll_, top = std::max(0, bottom - height);
        if (total > 0) {
            int line = k == "H" ? std::min(top + n - 1, bottom - 1) : k == "L" ? std::max(bottom - n, top) : top + (bottom - top) / 2;
            cur_line_ = static_cast<size_t>(std::clamp(line, 0, total - 1));
            cur_col_ = 0;
        }
    } else if (k == "*" || k == "#") {  // search for the keyword under or after the cursor
        if (lines_.empty()) return "";
        std::string t = line_text(lines_[cur_line_].spans);
        size_t p = utf8_offset(t, cur_col_);
        while (p < t.size() && char_class(t[p]) != 1) p = utf8_next(t, p);
        if (p >= t.size()) return "no word under the cursor";
        size_t from = p, to = p;
        while (from > 0 && char_class(t[utf8_prev(t, from)]) == 1) from = utf8_prev(t, from);
        while (to < t.size() && char_class(t[to]) == 1) to = utf8_next(t, to);
        search(t.substr(from, to - from));
        std::string msg = search_next(k == "*" ? 1 : -1);
        ensure_cursor_visible(height);
        return msg;
    } else if (k == "f" || k == "F" || k == "t" || k == "T") pending_ = k;
    else if ((k == ";" || k == ",") && !last_find_kind_.empty()) {
        std::string kind = last_find_kind_;
        if (k == ",") kind = kind == "f" ? "F" : kind == "F" ? "f" : kind == "t" ? "T" : "t";
        find_on_line(kind, last_find_char_, n);
    } else if (k == "}") jump_message(1, false, n);
    else if (k == "{") jump_message(-1, false, n);
    else if (pending_ == "]" || pending_ == "[") {
        std::string op = pending_;
        pending_.clear();
        if (k == op) jump_message(op == "]" ? 1 : -1, true, n);
    } else if (k == "]" || k == "[") pending_ = k;
    else if (k == "Y") {
        anchor_line_ = cur_line_;
        visual_ = VisualMode::Line;
        std::string text = yank_selection();
        visual_ = VisualMode::None;
        return yank_text(text, "1 line");
    }
    else if (k == "y" && visual_ != VisualMode::None) {
        std::string text = yank_selection();
        visual_ = VisualMode::None;
        *register_ = text;
        size_t nl = std::count(text.begin(), text.end(), '\n') + 1;
        ensure_cursor_visible(height);
        return "yanked " + std::to_string(nl) + (nl == 1 ? " line (" : " lines (") + copy_to_clipboard(text) + ")";
    } else if (k == "g" || k == "y" || k == "\"" || k == "z") pending_ = k;
    else if (k == "h" || e == Event::ArrowLeft) cur_col_ = cur_col_ >= static_cast<size_t>(n) ? cur_col_ - n : 0;
    else if (k == "l" || e == Event::ArrowRight) {
        size_t w = lines_.empty() ? 0 : line_width(lines_[cur_line_].spans);
        cur_col_ = std::min(cur_col_ + n, w ? w - 1 : 0);
    } else if (k == "0" || k == "^") cur_col_ = 0;
    else if (k == "$") cur_col_ = lines_.empty() ? 0 : std::max<size_t>(1, line_width(lines_[cur_line_].spans)) - 1;
    else if (k == "w" || k == "b" || k == "e") {
        if (!lines_.empty()) {
            std::string t = line_text(lines_[cur_line_].spans);
            size_t off = utf8_offset(t, cur_col_);
            if (k == "w") {
                int cls = off < t.size() ? char_class(t[off]) : 0;
                while (off < t.size() && cls != 0 && char_class(t[off]) == cls) off = utf8_next(t, off);
                while (off < t.size() && char_class(t[off]) == 0) off = utf8_next(t, off);
                if (off >= t.size() && cur_line_ + 1 < lines_.size()) {
                    move_cursor_line(1);
                    cur_col_ = 0;
                    ensure_cursor_visible(height);
                    return "";
                }
            } else if (k == "b") {
                if (off == 0 && cur_line_ > 0) {
                    move_cursor_line(-1);
                    cur_col_ = std::max<size_t>(1, line_width(lines_[cur_line_].spans)) - 1;
                    ensure_cursor_visible(height);
                    return "";
                }
                while (off > 0 && char_class(t[utf8_prev(t, off)]) == 0) off = utf8_prev(t, off);
                if (off > 0) {
                    int cls = char_class(t[utf8_prev(t, off)]);
                    while (off > 0 && char_class(t[utf8_prev(t, off)]) == cls) off = utf8_prev(t, off);
                }
            } else {
                off = utf8_next(t, off);
                while (off < t.size() && char_class(t[off]) == 0) off = utf8_next(t, off);
                if (off < t.size()) {
                    int cls = char_class(t[off]);
                    while (utf8_next(t, off) < t.size() && char_class(t[utf8_next(t, off)]) == cls) off = utf8_next(t, off);
                } else {
                    off = t.empty() ? 0 : utf8_prev(t, t.size());
                }
            }
            cur_col_ = utf8_len(t.substr(0, std::min(off, t.size())));
        }
    } else if (k == "v" || k == "V") {
        VisualMode want = k == "v" ? VisualMode::Char : VisualMode::Line;
        if (visual_ == want) visual_ = VisualMode::None;
        else {
            if (visual_ == VisualMode::None) anchor_line_ = cur_line_, anchor_col_ = cur_col_;
            visual_ = want;
        }
    } else if (k == "n") {
        std::string msg = search_next(1);
        ensure_cursor_visible(height);
        return msg;
    } else if (k == "N") {
        std::string msg = search_next(-1);
        ensure_cursor_visible(height);
        return msg;
    } else if (k == "o" && visual_ != VisualMode::None) {
        std::swap(anchor_line_, cur_line_);
        std::swap(anchor_col_, cur_col_);
    }
    ensure_cursor_visible(height);
    return "";
}

std::string View::status_hint() const {
    if (focused_ && !lines_.empty()) return std::to_string(cur_line_ + 1) + "/" + std::to_string(lines_.size());
    if (scroll_ > 0) return "↑" + std::to_string(scroll_) + " (G follows)";
    return "";
}

Element View::render(const Settings& settings, size_t width, int height) {
    last_height_ = height;
    layout(width);
    if (scroll_ > 0 && lines_.size() > last_total_) scroll_ += static_cast<int>(lines_.size() - last_total_);  // hold the view still
    last_total_ = lines_.size();
    int total = static_cast<int>(lines_.size());
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - height));
    int bottom = total - scroll_;
    int top = std::max(0, bottom - height);
    last_top_ = top;

    const Style& visual_style = settings.style("visual");
    const Style& search_style = settings.style("search");
    const Style& cursor_line_style = settings.style("cursor_line");
    static const Style cursor_style{std::nullopt, std::nullopt, false, false, false, false, true};

    size_t sel_a = 0, sel_b = 0, sel_ca = 0, sel_cb = 0;
    if (visual_ != VisualMode::None) {
        sel_a = std::min(cur_line_, anchor_line_);
        sel_b = std::max(cur_line_, anchor_line_);
        bool cursor_first = cur_line_ < anchor_line_ || (cur_line_ == anchor_line_ && cur_col_ <= anchor_col_);
        sel_ca = cursor_first ? cur_col_ : anchor_col_;
        sel_cb = cursor_first ? anchor_col_ : cur_col_;
    }

    Elements rows;
    for (int i = bottom - top; i < height; ++i) rows.push_back(text(""));
    for (int i = top; i < bottom; ++i) {
        const Line& l = lines_[static_cast<size_t>(i)];
        size_t pre_len = 0;
        StyledLine shown = l.spans;
        std::string mark;
        {
            std::lock_guard lock(mu_);
            mark = l.entry < entries_.size() ? marker(entries_[l.entry]) : prefix(l.kind);
        }
        std::string pre = l.first ? mark : std::string(utf8_len(mark), ' ');
        if (timestamps_) {
            std::string stamp(6, ' ');
            if (l.first) {
                std::lock_guard lock(mu_);
                if (l.entry < entries_.size() && entries_[l.entry].when) {
                    char buf[8];
                    std::strftime(buf, sizeof(buf), "%H:%M", std::localtime(&entries_[l.entry].when));
                    stamp = std::string(buf) + " ";
                }
            }
            pre = stamp + pre;
        }
        if (!l.spans.empty() || l.first) {
            shown.insert(shown.begin(), Span{pre, MdNone});
            pre_len = utf8_len(pre);
        }
        std::vector<Overlay> overlays;
        size_t li = static_cast<size_t>(i);
        size_t w = line_width(l.spans);
        // Overlays merge in order, so the cursor-line background goes first and the selection paints over it.
        if (focused_ && li == cur_line_ && visual_ == VisualMode::None) {
            overlays.push_back({0, pre_len + std::max<size_t>(w, 1), &cursor_line_style});
        }
        if (visual_ != VisualMode::None && li >= sel_a && li <= sel_b) {
            size_t from = 0, to = w;
            if (visual_ == VisualMode::Char) {
                if (li == sel_a) from = sel_ca;
                if (li == sel_b) to = std::min(w, sel_cb + 1);
            }
            overlays.push_back({pre_len + from, pre_len + std::max(to, from + 1), &visual_style});
        }
        for (const auto& [ml, mc] : matches_) {
            if (ml == li) overlays.push_back({pre_len + mc, pre_len + mc + utf8_len(pattern_), &search_style});
        }
        if (focused_ && li == cur_line_) {
            overlays.push_back({pre_len + cur_col_, pre_len + cur_col_ + 1, &cursor_style});
            if (w == 0) shown.push_back(Span{" ", MdNone});
        }
        rows.push_back(render_line(settings, shown, settings.style(style_name(l.kind)), overlays));
    }
    return vbox(rows);
}

}  // namespace maid
