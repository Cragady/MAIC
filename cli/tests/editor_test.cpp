// The vim input editor and the markdown renderer.
#include "check.hpp"

#include "commands.hpp"

#include <algorithm>
#include "editor.hpp"
#include "view.hpp"
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
        else if (seq.compare(i, 5, "<c-r>") == 0) e = Event::Character("\x12"), i += 5;
        else e = Event::Character(seq.substr(i, utf8_next(seq, i) - i)), i = utf8_next(seq, i);
        last = ed.handle(e);
    }
    return last;
}

// A fresh editor with `typed` entered in insert mode (the editor itself starts in normal mode).
Editor fresh(const std::string& typed = "") {
    Editor ed(&reg);
    keys(ed, "i" + typed);
    return ed;
}

void check(const std::string& typed, const std::string& then, const std::string& want, const std::string& what) {
    Editor ed = fresh(typed);
    keys(ed, then);
    expect(ed.text() == want, what + " -> \"" + ed.text() + "\"");
}

}  // namespace

int main() {
    setenv("MAIC_NO_CLIPBOARD", "1", 1);  // never touch the real clipboard from a test
    section("insert mode");
    {
        Editor ed(&reg);
        expect(ed.mode() == Editor::Mode::Normal, "starts in normal mode");
    }
    check("hello world", "", "hello world", "typing");
    check("hello world", "<bs><bs>", "hello wor", "backspace");
    check("hello world", "<c-w>", "hello ", "Ctrl-W deletes a word");
    check("hello world", "<c-u>", "", "Ctrl-U deletes the line");
    check("héllo wörld", "<bs>", "héllo wörl", "backspace is UTF-8 aware");
    {
        Editor ed = fresh("line one");
        auto r = keys(ed, "<cr>line two");
        expect(ed.text() == "line one\nline two" && r.action == Editor::Action::None, "Enter inserts a newline and sends nothing");
        keys(ed, "<esc>gg");
        expect(ed.cursor() == 0, "gg goes to the start");
        keys(ed, "<cr>");
        expect(ed.cursor() == 9, "Enter in normal mode moves down a line");
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
    {
        Editor ed = fresh("one two three");
        keys(ed, "<esc>0dwdw");
        expect(ed.text() == "three", "two deletes");
        keys(ed, "u");
        expect(ed.text() == "two three", "u undoes the last one");
        keys(ed, "u");
        expect(ed.text() == "one two three", "u again undoes the first");
        keys(ed, "<c-r>");
        expect(ed.text() == "two three", "Ctrl-R redoes");
        keys(ed, "<c-r><c-r>");
        expect(ed.text() == "three", "redo stops at the end of the stack");
        keys(ed, "0x");
        keys(ed, "<c-r>");
        expect(ed.text() == "hree", "a new change clears the redo stack");
        Editor ins = fresh("abc");
        keys(ins, "<esc>A def<esc>A ghi<esc>u");
        expect(ins.text() == "abc def", "an insert session undoes as one step");
        keys(ins, "u");
        expect(ins.text() == "abc", "and the earlier session as another");
        Editor cw = fresh("one two");
        keys(cw, "<esc>0cwONE<esc>u");
        expect(cw.text() == "one two", "cw and its typing undo together");
    }
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

    section("text objects");
    check("one two three", "<esc>0wciwX<esc>", "one X three", "ciw changes the word under the cursor");
    check("one two three", "<esc>0wdaw", "one three", "daw deletes the word and its trailing space");
    check("say \"hello world\" now", "<esc>0di\"", "say \"\" now", "di\" with the cursor before the quotes operates on the pair ahead, as vim does");
    {
        Editor ed = fresh("say \"hello world\" now");
        keys(ed, "<esc>0");
        for (int i = 0; i < 7; ++i) keys(ed, "l");  // onto 'e' of hello
        keys(ed, "di\"");
        expect(ed.text() == "say \"\" now", "di\" empties the quotes -> \"" + ed.text() + "\"");
        Editor ed2 = fresh("say \"hello world\" now");
        keys(ed2, "<esc>0");
        for (int i = 0; i < 7; ++i) keys(ed2, "l");
        keys(ed2, "da\"");
        expect(ed2.text() == "say  now", "da\" removes the quotes too -> \"" + ed2.text() + "\"");
    }
    {
        Editor ed = fresh("f(a, (b)) x");
        keys(ed, "<esc>0ll");  // on 'a'
        keys(ed, "da(");
        expect(ed.text() == "f x", "da( removes the enclosing parens -> \"" + ed.text() + "\"");
        Editor ed2 = fresh("f(a, (b)) x");
        keys(ed2, "<esc>0");
        for (int i = 0; i < 6; ++i) keys(ed2, "l");  // on 'b'
        keys(ed2, "ci)Z<esc>");
        expect(ed2.text() == "f(a, (Z)) x", "ci) works on the innermost pair -> \"" + ed2.text() + "\"");
        Editor ed3 = fresh("call {a b} done");
        keys(ed3, "<esc>0");
        for (int i = 0; i < 6; ++i) keys(ed3, "l");
        keys(ed3, "yi{");
        expect(reg == "a b", "yi{ yanks inside braces");
        keys(ed3, "$p");
        expect(ed3.text() == "call {a b} donea b", "and p pastes it");
    }
    check("one two three", "<esc>0wviwd", "one  three", "viw then d deletes the selected word");
    {
        Editor ed = fresh("say \"hi there\" ok");
        keys(ed, "<esc>0");
        for (int i = 0; i < 6; ++i) keys(ed, "l");
        keys(ed, "va\"y");
        expect(reg == "\"hi there\"", "va\" then y yanks the quoted text with quotes");
    }

    section("registers and the leader");
    {
        Editor ed = fresh("alpha beta");
        keys(ed, "<esc>Y");
        expect(reg == "alpha beta", "Y yanks the line");
        reg.clear();
        keys(ed, "0\"+yw");
        expect(reg == "alpha ", "\"+yw yanks to the register too (clipboard disabled in tests)");
        reg.clear();
        keys(ed, " y");  // Space is the leader
        expect(reg == "alpha beta", "<leader>y yanks the line");
        Editor ins = fresh("a b");
        expect(ins.text() == "a b", "Space in insert mode is still a space");
        Editor v = fresh("one two");
        keys(v, "<esc>0viw y");
        expect(reg == "one" && v.mode() == Editor::Mode::Normal, "<leader>y in visual mode yanks the selection");
        Editor custom = fresh("x y");
        custom.set_leader(",");
        keys(custom, "<esc>,y");
        expect(reg == "x y", "the leader can be changed");
        reg = "PASTED";
        Editor p = fresh("ab");
        keys(p, "<esc>\"+p");
        expect(p.text() == "ab", "\"+p pastes the clipboard, which is empty here, so nothing changes");
        keys(p, "p");
        expect(p.text() == "abPASTED", "a plain p still pastes the register (after the cursor, which sits on b)");
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

    section("commands and help");
    {
        auto m = match_commands("w");
        expect(!m.empty() && m.front()->name == "w", "an exact command name matches first");
        m = match_commands("mo");
        expect(m.size() == 3 && m[0]->name == "mode" && m[1]->name == "model" && m[2]->name == "models", "a prefix lists mode, model and models");
        m = match_commands("ww");
        expect(m.size() == 1 && m.front()->name == "ww", ":ww is its own command");
        m = match_commands("quit");
        expect(m.size() == 1 && m.front()->name == "q", "aliases match");
        expect(match_commands("zzz").empty(), "nothing matches nonsense");
        CompletionContext ctx{{"ollama", "comfyui"}, {"ollama", "anthropic"}};
        auto a = complete_argument("mode", "au", ctx);
        expect(a.size() == 2 && a[0] == "auto" && a[1] == "auto-read", "mode arguments complete");
        expect(complete_argument("up", "c", ctx) == std::vector<std::string>{"comfyui"}, "service names complete");
        expect(complete_argument("model", "an", ctx) == std::vector<std::string>{"anthropic/"}, "provider prefixes complete");
        auto h = complete_argument("h", "sess", ctx);
        expect(std::find(h.begin(), h.end(), "sessions") != h.end() && std::find(h.begin(), h.end(), "session") != h.end(), "help topics and commands complete");
        expect(help_text("").find(":w") != std::string::npos && help_text("").find("modes") != std::string::npos, ":h alone is an index");
        expect(help_text("w").find("*:w*") == 0, ":h w is the :w page");
        expect(help_text(":w") == help_text("w") && help_text("write") == help_text("w"), "a colon or an alias also finds it");
        expect(help_text("Ctrl-W").find("*conversation*") == 0 && help_text("<C-w>") == help_text("ctrl-w"), "key names normalise");
        expect(help_text("Alt+Enter").find("Sends") != std::string::npos && help_text("M-CR") == help_text("alt-enter"), "Alt+Enter in several spellings");
        expect(help_text("mod").find("several") != std::string::npos, "an ambiguous prefix lists the candidates");
        expect(help_text("harn").find("*harness*") == 0, "a unique prefix resolves");
        expect(help_text("sess").find("several") != std::string::npos, "sess matches :session and sessions, so it lists both");
        expect(help_text("nope").find("no help") == 0, "an unknown topic says so");
    }

    section("conversation window folds");
    {
        std::string r2;
        View v(&r2);
        std::string big;
        for (int i = 1; i <= 20; ++i) big += "line " + std::to_string(i) + "\n";
        v.append(Kind::Tool, "$ ls");
        v.append(Kind::ToolOk, big);
        Settings s;
        v.render(s, 80, 40);  // lays out
        v.set_focused(true);
        auto rendered_lines = [&] {
            v.render(s, 80, 40);
            return v.status_hint();  // "cursor/total"
        };
        std::string before = rendered_lines();
        expect(before.find("/") != std::string::npos && std::stoul(before.substr(before.find('/') + 1)) < 15, "a long tool result is folded to a preview");
        v.handle(Event::Character("z"), 40);
        std::string msg = v.handle(Event::Character("a"), 40);
        std::string after = rendered_lines();
        expect(std::stoul(after.substr(after.find('/') + 1)) > 20, "za unfolds it (" + after + ")");
        v.handle(Event::Character("z"), 40);
        v.handle(Event::Character("M"), 40);
        expect(rendered_lines() == before, "zM folds everything again");
        v.set_collapse_default(false);
        v.append(Kind::ToolOk, big);
        expect(std::stoul(rendered_lines().substr(rendered_lines().find('/') + 1)) > 30, "with tooldetails on, new results arrive unfolded");
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
