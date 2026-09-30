// The vim input editor and the markdown renderer.
#include "check.hpp"

#include "editor.hpp"
#include "maic/markdown.hpp"

using namespace maic;
using ftxui::Event;

namespace {

std::string reg;

// Feeds keys vim-style: "<esc>", "<cr>", "<bs>", "<c-w>", "<c-u>" and plain characters.
Editor::Result keys(Editor& ed, const std::string& seq) {
    Editor::Result last;
    for (size_t i = 0; i < seq.size();) {
        Event e;
        if (seq.compare(i, 5, "<esc>") == 0) e = Event::Escape, i += 5;
        else if (seq.compare(i, 4, "<cr>") == 0) e = Event::Return, i += 4;
        else if (seq.compare(i, 4, "<bs>") == 0) e = Event::Backspace, i += 4;
        else if (seq.compare(i, 5, "<c-w>") == 0) e = Event::Character("\x17"), i += 5;
        else if (seq.compare(i, 5, "<c-u>") == 0) e = Event::Character("\x15"), i += 5;
        else e = Event::Character(seq.substr(i, utf8_next(seq, i) - i)), i = utf8_next(seq, i);
        last = ed.handle(e);
    }
    return last;
}

Editor fresh(const std::string& typed = "") {
    Editor ed(&reg);
    keys(ed, typed);
    return ed;
}

void check(const std::string& typed, const std::string& then, const std::string& want, const std::string& what) {
    Editor ed = fresh(typed);
    keys(ed, then);
    expect(ed.text() == want, what + " -> \"" + ed.text() + "\"");
}

}  // namespace

int main() {
    section("insert mode");
    check("hello world", "", "hello world", "typing");
    check("hello world", "<bs><bs>", "hello wor", "backspace");
    check("hello world", "<c-w>", "hello ", "Ctrl-W deletes a word");
    check("hello world", "<c-u>", "", "Ctrl-U deletes the line");
    check("héllo wörld", "<bs>", "héllo wörl", "backspace is UTF-8 aware");
    {
        Editor ed = fresh("line one\\");
        keys(ed, "<cr>line two");
        expect(ed.text() == "line one\nline two", "backslash-Enter inserts a newline");
        Editor sender = fresh("send me");
        auto r = keys(sender, "<cr>");
        expect(r.action == Editor::Action::Submit, "Enter submits");
    }

    section("normal mode motions and operators");
    check("one two three", "<esc>bdw", "one two ", "b then dw deletes the last word");
    check("one two three", "<esc>0dw", "two three", "0 then dw");
    check("one two three", "<esc>0wcwTWO<esc>", "one TWO three", "cw changes to the end of the word");
    check("a `code` here", "<esc>bbcwX<esc>", "a `codeX here", "cw on punctuation changes only that character");
    check("one two three", "<esc>0d$", "", "d$ deletes to the end");
    check("one two three", "<esc>0xx", "e two three", "x with a count of one, twice");
    check("one two three", "<esc>03x", " two three", "3x");
    check("one two three", "<esc>dd", "", "dd clears");
    check("one two three", "<esc>ddu", "one two three", "u undoes");
    check("one two three", "<esc>0ea!<esc>", "one! two three", "e then a inserts after the word end");
    check("one two three", "<esc>Ino <esc>", "no one two three", "I inserts at the start");
    check("one two three", "<esc>A!<esc>", "one two three!", "A appends at the end");
    check("one two three", "<esc>0cc", "", "cc clears and enters insert");
    check("one two three", "<esc>0Dinew<esc>", "new", "D, then i and typing");
    check("one two three", "<esc>02w", "one two three", "counts before motions are accepted");
    {
        Editor ed = fresh("one two three");
        keys(ed, "<esc>02w");
        expect(ed.cursor() == 8, "2w lands on the third word");
        keys(ed, "0");
        expect(ed.cursor() == 0, "0 goes to the start");
        keys(ed, "$");
        expect(ed.cursor() == 12, "$ goes to the last character in normal mode");
    }

    section("register, yank and paste");
    {
        Editor ed = fresh("one two three");
        keys(ed, "<esc>0yw");
        expect(reg == "one ", "yw yanks the word and the space");
        keys(ed, "$p");
        expect(ed.text() == "one two threeone ", "p pastes after the cursor");
        keys(ed, "0dw");
        expect(reg == "one ", "dw fills the register");
        keys(ed, "$p");
        expect(ed.text().find("one ") != std::string::npos, "the deleted text pastes back");
        Editor ed2 = fresh("abc");
        keys(ed2, "<esc>yy");
        expect(reg == "abc", "yy yanks the whole input");
    }

    section("visual mode");
    check("one two three", "<esc>0vey", "one two three", "vey leaves the text");
    expect(reg == "one", "vey yanked the word");
    check("one two three", "<esc>0ved", " two three", "ved deletes the selection");
    check("one two three", "<esc>0vecX<esc>", "X two three", "vec changes the selection");
    check("one two three", "<esc>Vd", "", "V d deletes the line");
    {
        Editor ed = fresh("one two three");
        keys(ed, "<esc>0v");
        expect(ed.mode() == Editor::Mode::Visual, "v enters visual");
        keys(ed, "e");
        auto [a, b] = ed.selection();
        expect(a == 0 && b == 3, "the selection covers the word inclusively");
        keys(ed, "<esc>");
        expect(ed.mode() == Editor::Mode::Normal, "Esc leaves visual");
    }

    section("command line");
    {
        Editor ed = fresh("draft");
        keys(ed, "<esc>");
        auto r = keys(ed, ":w now<cr>");
        expect(r.action == Editor::Action::Command && r.text == "w now", ": returns the command");
        expect(ed.text() == "draft" && ed.mode() == Editor::Mode::Normal, "the draft survives a command");
        r = keys(ed, "/needle<cr>");
        expect(r.action == Editor::Action::Search && r.text == "needle", "/ returns a search");
        keys(ed, ":abc<esc>");
        expect(ed.mode() == Editor::Mode::Normal, "Esc cancels the command line");
    }

    section("history");
    {
        Editor ed = fresh("");
        ed.remember("first");
        ed.remember("second");
        ed.history_step(-1);
        expect(ed.text() == "second", "up recalls the last prompt");
        ed.history_step(-1);
        expect(ed.text() == "first", "up again recalls the one before");
        ed.history_step(1);
        ed.history_step(1);
        expect(ed.text().empty(), "down returns to the empty draft");
    }

    section("markdown");
    {
        auto lines = markdown_lines("# Title\nplain **bold** and `code` [link](http://x) text\n```\ncode block\n```\n- item\n> quote");
        expect(lines.size() == 7, "one styled line per source line");
        expect(!lines[0].empty() && (lines[0][0].flags & MdHeading), "heading detected");
        bool bold = false, code = false, link = false, url = false;
        for (const auto& s : lines[1]) {
            if ((s.flags & MdBold) && s.text == "bold") bold = true;
            if ((s.flags & MdCode) && s.text == "`code`") code = true;
            if ((s.flags & MdLink) && s.text == "link") link = true;
            if ((s.flags & MdUrl) && s.text.find("http://x") != std::string::npos) url = true;
        }
        expect(bold && code && link && url, "inline bold, code, link text and url");
        expect(line_text(lines[1]) == "plain **bold** and `code` [link](http://x) text", "markup characters are kept");
        expect(lines[3][0].flags & MdCodeBlock, "fenced block content");
        expect(lines[5][0].flags & MdBullet, "bullet marker");
        expect(lines[6][0].flags & MdQuote, "quote");
        auto plain = plain_lines("**not** bold");
        expect(plain.size() == 1 && plain[0][0].flags == MdNone, "plain_lines interprets nothing");
        auto snake = markdown_lines("snake_case_name stays");
        expect(snake[0].size() == 1 && snake[0][0].flags == MdNone, "underscores inside words are not italics");
    }

    section("wrapping");
    {
        StyledLine l = {{"the quick brown fox jumps over the lazy dog", MdNone}};
        auto rows = wrap_line(l, 16);
        expect(rows.size() >= 3 && line_width(rows[0]) <= 16 && line_text(rows[0]) == "the quick brown", "wraps at a space");
        auto hard = wrap_line({{"abcdefghijklmnopqrstuvwxyz", MdNone}}, 10);
        expect(hard.size() == 3 && line_text(hard[0]) == "abcdefghij", "a long word is cut hard");
        auto empty = wrap_line({}, 10);
        expect(empty.size() == 1, "an empty line stays one row");
        auto spans = wrap_line({{"aaa ", MdNone}, {"bbb", MdBold}, {" ccc", MdNone}}, 8);
        expect(spans.size() == 2 && line_text(spans[0]) == "aaa bbb" && spans[0][1].flags == MdBold && line_text(spans[1]) == "ccc",
               "styles survive the wrap");
    }

    return finish();
}
