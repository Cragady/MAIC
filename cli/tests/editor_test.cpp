// The vim input editor and the markdown renderer.
#include "check.hpp"

#include "commands.hpp"
#include "maic/agent.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include "editor.hpp"
#include "highlight.hpp"
#include "msgpack.hpp"
#include "style.hpp"
#include "view.hpp"
#include "maic/markdown.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace fs = std::filesystem;
using namespace maic;
using ftxui::Event;

namespace {

std::string reg;

// Feeds keys vim-style: "<esc>", "<cr>", "<bs>", "<c-w>", "<c-u>", "<c-r>", "<c-o>" and plain characters.
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
        else if (seq.compare(i, 5, "<c-o>") == 0) e = Event::Character("\x0f"), i += 5;
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

    section("f t F T ; ,");
    {
        Editor ed = fresh("say (hello) and (bye)");
        keys(ed, "<esc>0f(");
        expect(ed.cursor() == 4, "f( lands on the first paren");
        keys(ed, ";");
        expect(ed.cursor() == 16, "; repeats forward");
        keys(ed, ",");
        expect(ed.cursor() == 4, ", reverses");
        keys(ed, "$Fa");
        expect(ed.cursor() == 12, "F finds backward: " + std::to_string(ed.cursor()));
        keys(ed, "0t)");
        expect(ed.cursor() == 9, "t stops before the character");
        keys(ed, "$T(");
        expect(ed.cursor() == 17, "T stops after the character");
        keys(ed, "0fz");
        expect(ed.cursor() == 0, "a missing character does not move");
        keys(ed, "02f(");
        expect(ed.cursor() == 16, "a count selects the nth match");
    }
    check("say (hello) and (bye)", "<esc>0f(dt)", "say ) and (bye)", "dt) deletes up to the paren");
    check("say (hello) and (bye)", "<esc>0f(df)", "say  and (bye)", "df) deletes through the paren");
    check("say (hello) and (bye)", "<esc>0f(ct)X<esc>", "say X) and (bye)", "ct) changes up to the paren");
    check("say (hello) and (bye)", "<esc>$dF(", "say (hello) and )", "dF( deletes back to the paren, keeping the cursor char");
    check("one two three", "<esc>0vf y", "one two three", "v then f extends the selection");
    expect(reg == "one ", "and y yanks it");

    section("paragraphs and sentences");
    {
        Editor ed = fresh("a b\nc d\n\ne f\ng h");
        keys(ed, "<esc>gg}");
        expect(ed.cursor() == 8, "} goes to the empty line after the paragraph: " + std::to_string(ed.cursor()));
        keys(ed, "}");
        expect(ed.cursor() == 15, "} at the last paragraph goes to the end: " + std::to_string(ed.cursor()));
        keys(ed, "{");
        expect(ed.cursor() == 8, "{ goes back to the empty line");
        keys(ed, "{");
        expect(ed.cursor() == 0, "{ at the first paragraph goes to the start");
        keys(ed, "G2{");
        expect(ed.cursor() == 0, "2{ takes a count");
    }
    check("a b\nc d\n\ne f", "<esc>ggd}", "\ne f", "d} from the line start deletes the paragraph's lines");
    check("a b\nc d\n\ne f", "<esc>ggld}", "a\n\ne f", "d} from mid-line keeps the line break");
    check("a b\nc d\n\ne f\ng h", "<esc>ggdap", "e f\ng h", "dap deletes the paragraph and the empty line");
    check("a b\nc d\n\ne f\ng h", "<esc>ggdip", "\ne f\ng h", "dip leaves the empty line");
    check("a b\nc d\n\ne f\ng h", "<esc>Gdap", "a b\nc d", "dap on the last paragraph takes the empty line before it");
    check("a b\n\n\nc d", "<esc>ggjdip", "a b\nc d", "dip on empty lines deletes the run of them");
    check("a b\nc d\n\ne f\ng h", "<esc>Gvipd", "a b\nc d\n", "vip selects the paragraph linewise");
    check("a b\nc d\n\ne f\ng h", "<esc>ggcipX<esc>", "X\n\ne f\ng h", "cip changes the paragraph to one line");
    {
        Editor ed = fresh("a b\nc d\n\ne f\ng h");
        keys(ed, "<esc>Gyip");
        expect(reg == "e f\ng h", "yip yanks the paragraph");
        keys(ed, "ggP");
        expect(ed.text() == "e f\ng h\na b\nc d\n\ne f\ng h", "and P puts the lines above");
    }
    {
        Editor ed = fresh("One two. Three four! Five six? Seven");
        keys(ed, "<esc>0)");
        expect(ed.cursor() == 9, ") goes to the next sentence: " + std::to_string(ed.cursor()));
        keys(ed, "2)");
        expect(ed.cursor() == 31, "2) takes a count: " + std::to_string(ed.cursor()));
        keys(ed, "(");
        expect(ed.cursor() == 21, "( goes to the start of the previous sentence");
        keys(ed, "l(");
        expect(ed.cursor() == 21, "( mid-sentence goes to its start");
        keys(ed, "$)");
        expect(ed.cursor() == 35, ") at the last sentence goes to the end");
    }
    check("One two. Three four! Five six? Seven", "<esc>0d)", "Three four! Five six? Seven", "d) deletes the sentence and its space");
    check("One two. Three four! Five six? Seven", "<esc>0das", "Three four! Five six? Seven", "das deletes a sentence with its trailing space");
    check("One two. Three four! Five six? Seven", "<esc>0dis", " Three four! Five six? Seven", "dis keeps the space");
    check("One two. Three four! Five six? Seven", "<esc>$das", "One two. Three four! Five six?", "das on the last sentence takes the space before it");
    check("One two. Three four! Five six? Seven", "<esc>0)cisX<esc>", "One two. X Five six? Seven", "cis changes the sentence under the cursor");
    check("One two. Three four! Five six?", "<esc>0d2as", "Five six?", "2as takes two sentences");
    check("One two.  Three four.", "<esc>0fThdis", "One two.Three four.", "is on the white space between sentences takes the white space");
    check("Hi there.\n\nBye now.", "<esc>gg0das", "\n\nBye now.", "as stops before an empty line");
    check("One two. Three four.", "<esc>0visd", " Three four.", "vis selects the sentence");

    section("dot repeat");
    check("a b c d", "<esc>0dw.", "c d", ". repeats dw");
    check("a b c d", "<esc>0dw..", "d", ". repeats again");
    check("one two three four", "<esc>0cwX<esc>w.", "X X three four", ". repeats a change with its typed text");
    check("abcdefgh", "<esc>02x.", "efgh", ". keeps the count");
    check("abcdefgh", "<esc>0x3.", "efgh", "a count on . replaces the old one");
    check("", "<esc>ifoo<esc>.", "fofooo", ". repeats an insert session");
    check("x\ny", "<esc>ggA!<esc>j.", "x!\ny!", ". repeats A and the typed text on another line");
    check("abc", "<esc>xu.", "ab", "u is not a change: . repeats the x");
    check("abcdef", "<esc>0vld.", "ef", ". repeats a visual delete on the same amount of text");
    check("a b c", "<esc>0d2w.", "", ". repeats d2w");
    check("one two three four", "<esc>0dt .", " three four", ". repeats an f/t target");
    check("aa bb cc", "<esc>0ciwX<esc>w.", "X X cc", ". repeats a text object change");
    check("ab", "<esc>$ylp.", "abbb", ". repeats p");
    check("abc", "<esc>0rx.", "xbc", ". repeats r without moving");
    check("abc", "<esc>0rxl.", "xxc", ". repeats r on the next character");
    check("a\nb\nc\nd", "<esc>ggJ.", "a b c\nd", ". repeats J");
    check("a\nb", "<esc>gg>>j.", "    a\n    b", ". repeats >>");
    check("aaa", "<esc>0~.", "AAa", ". repeats ~");
    check("", "<esc>3ix<esc>", "xxx", "a count on i repeats the insert");
    check("", "<esc>2ofoo<esc>", "\nfoo\nfoo", "a count on o opens that many lines");
    check("123456", "<esc>02Rab<esc>", "abab56", "a count on R repeats the replacement");
    check("", "<esc>3ix<esc>.", "xxxxxx", ". repeats a counted insert with its count");

    section("macros");
    {
        Editor ed = fresh("a b c d e");
        keys(ed, "<esc>0qa");
        expect(ed.recording() == 'a', "qa starts recording into a");
        keys(ed, "dwq");
        expect(ed.recording() == 0 && ed.text() == "b c d e", "q stops; the keys ran while recording");
        expect(ed.registers().count('a') && ed.registers().at('a').text == "dw", "the macro is register a's text, without the closing q");
        keys(ed, "@a");
        expect(ed.text() == "c d e", "@a replays it");
        keys(ed, "@@");
        expect(ed.text() == "d e", "@@ repeats the last macro");
        keys(ed, "2@a");
        expect(ed.text() == "", "a count replays that many times -> \"" + ed.text() + "\"");
        Editor lines = fresh("1\n2\n3\n4\n5");
        keys(lines, "<esc>ggqaA!<esc>jq");
        expect(lines.text() == "1!\n2\n3\n4\n5", "a macro with an insert session and Esc");
        keys(lines, "3@a");
        expect(lines.text() == "1!\n2!\n3!\n4!\n5", "3@a appends on three more lines -> \"" + lines.text() + "\"");
        keys(lines, "9@a");
        expect(lines.text() == "1!\n2!\n3!\n4!\n5!", "a failed motion (j on the last line) stops the replay -> \"" + lines.text() + "\"");
        Editor dot = fresh("a b c d e");
        keys(dot, "<esc>0qadwdwq.");
        expect(dot.text() == "d e", ". after a macro repeats the last change inside it, not the macro");
        Editor cmd = fresh("draft");
        keys(cmd, "<esc>qa:w<cr>q");
        auto r = keys(cmd, "@a");
        expect(r.action == Editor::Action::Command && r.text == "w", "a macro that ends in a command line hands the command out");
        Editor rec = fresh("x");
        keys(rec, "<esc>qbqqaq");
        expect(rec.registers().at('b').text == "" && rec.registers().at('a').text == "" && rec.recording() == 0, "qb q records nothing; qa q starts and stops again");
        Editor cr = fresh("one\ntwo");
        keys(cr, "<esc>ggqcIhi <esc>jq@c");
        expect(cr.text() == "hi one\nhi two", "a macro replays I and the typed text -> \"" + cr.text() + "\"");
        Editor none = fresh("abc");
        keys(none, "<esc>@z");
        expect(none.text() == "abc", "an empty register runs nothing");
        Editor paste = fresh("abc");
        keys(paste, "<esc>0\"myl");  // register m = "a": as a macro, `a` appends
        paste.set_text("xyz");
        keys(paste, "<esc>0@mQ<esc>");
        expect(paste.text() == "xQyz", "a yanked register runs as a macro too (its text is a: append)");
    }

    section("W B E gE % * #");
    {
        Editor ed = fresh("foo-bar baz.qux end");
        keys(ed, "<esc>0W");
        expect(ed.cursor() == 8, "W skips a WORD with punctuation: " + std::to_string(ed.cursor()));
        keys(ed, "W");
        expect(ed.cursor() == 16, "W again");
        keys(ed, "B");
        expect(ed.cursor() == 8, "B goes back a WORD");
        keys(ed, "0E");
        expect(ed.cursor() == 6, "E goes to the end of the WORD: " + std::to_string(ed.cursor()));
        keys(ed, "$gE");
        expect(ed.cursor() == 14, "gE goes back to the end of the previous WORD: " + std::to_string(ed.cursor()));
        keys(ed, "0w");
        expect(ed.cursor() == 3, "w still stops at punctuation");
    }
    check("foo-bar baz.qux end", "<esc>0dW", "baz.qux end", "dW deletes the WORD and its space");
    check("foo-bar baz.qux end", "<esc>0cWX<esc>", "X baz.qux end", "cW changes to the end of the WORD");
    check("foo-bar baz.qux end", "<esc>02dW", "end", "2dW");
    check("foo-bar baz.qux end", "<esc>$dB", "foo-bar baz.qux d", "dB deletes back to the WORD start");
    check("foo-bar baz", "<esc>0dE", " baz", "dE is inclusive");
    check("a b c d", "<esc>0vEEy", "a b c d", "E extends a selection");
    expect(reg == "a b c", "vEEy yanked through the third word");
    {
        Editor ed = fresh("f(a, [b]) x");
        keys(ed, "<esc>0%");
        expect(ed.cursor() == 8, "% from before a bracket goes to the match of the first one on the line: " + std::to_string(ed.cursor()));
        keys(ed, "%");
        expect(ed.cursor() == 1, "% on a closing bracket goes back to the opening one");
        keys(ed, "f[%");
        expect(ed.cursor() == 7, "% matches brackets of the same kind: " + std::to_string(ed.cursor()));
        keys(ed, "$%");
        expect(ed.cursor() == 10, "% with no bracket after the cursor does not move");
        keys(ed, "0%``");
        expect(ed.cursor() == 0, "% is a jump");
        Editor multi = fresh("{\n  a\n}");
        keys(multi, "<esc>gg%");
        expect(multi.cursor() == 6, "% crosses lines: " + std::to_string(multi.cursor()));
    }
    check("f(a, (b)) x", "<esc>0ld%", "f x", "d% deletes through the matching bracket, nesting counted");
    check("f(a) x", "<esc>0y%$p", "f(a) xf(a)", "y% yanks both brackets");
    {
        Editor ed = fresh("hello world");
        auto r = keys(ed, "<esc>0*");
        expect(r.action == Editor::Action::Search && r.text == "hello", "* asks for a search for the word under the cursor");
        r = keys(ed, "w#");
        expect(r.action == Editor::Action::SearchBack && r.text == "world", "# searches backward");
        Editor gap = fresh("a  b");
        r = keys(gap, "<esc>0l*");
        expect(r.action == Editor::Action::Search && r.text == "b", "on white space * takes the next word");
        Editor punct = fresh("x = (y)");
        r = keys(punct, "<esc>0ll*");
        expect(r.action == Editor::Action::Search && r.text == "y", "* skips punctuation to a keyword");
        Editor none = fresh("...");
        r = keys(none, "<esc>0*");
        expect(r.action == Editor::Action::None, "* with no word does nothing");
    }

    section("enter_sends");
    {
        Editor ed(&reg);
        ed.set_enter_sends(true);
        keys(ed, "ihello");
        auto r = keys(ed, "<cr>");
        expect(r.action == Editor::Action::Send && r.text == "hello" && ed.text() == "hello", "Enter on a one-line input sends it");
        ed.newline();
        keys(ed, "world");
        r = keys(ed, "<cr>");
        expect(r.action == Editor::Action::None && ed.text() == "hello\nworld\n", "after a line break Enter is a line break again -> \"" + ed.text() + "\"");
        Editor off(&reg);
        r = keys(off, "ia<cr>");
        expect(r.action == Editor::Action::None && off.text() == "a\n", "off by default: Enter is a line break");
        Editor n(&reg);
        n.set_enter_sends(true);
        keys(n, "<esc>");
        n.newline();
        expect(n.text().empty(), "newline() does nothing outside insert mode");
        Editor rep_(&reg);
        rep_.set_enter_sends(true);
        keys(rep_, "<esc>3ia<cr>");
        expect(rep_.text() == "a" && rep_.mode() == Editor::Mode::Insert, "a Send does not count as typed text for the insert repeat");
    }

    section("marks");
    {
        Editor ed = fresh("one\n  two\nthree");
        keys(ed, "<esc>ggjll");
        expect(ed.cursor() == 6, "setup: on the w of two");
        keys(ed, "mbG");
        expect(ed.cursor() == 10, "G goes to the last line");
        keys(ed, "`b");
        expect(ed.cursor() == 6, "`b jumps to the exact position");
        keys(ed, "G'b");
        expect(ed.cursor() == 6, "'b jumps to the first non-blank of the line");
        keys(ed, "``");
        expect(ed.cursor() == 10, "`` returns to where the jump left from");
        keys(ed, "''");
        expect(ed.cursor() == 6, "'' returns to the line of the jump");
        keys(ed, "ggmaG");
        keys(ed, "y'a");
        expect(reg == "one\n  two\nthree", "y'a yanks the lines up to the mark");
        keys(ed, "G`bd`a");
        expect(ed.text() == "two\nthree", "d`a deletes to the exact mark");
        Editor moved = fresh("abc def");
        keys(moved, "<esc>$ma0iXX<esc>`a");
        expect(moved.cursor() == 8, "a mark follows text inserted before it: " + std::to_string(moved.cursor()));
        keys(moved, "0dwma");
        keys(moved, "$`a");
        expect(moved.cursor() == 0, "a mark inside deleted text moves to the start of the deletion");
        Editor none = fresh("abc");
        keys(none, "<esc>0l`z");
        expect(none.cursor() == 1, "an unset mark does not move");
    }

    section("gq and case");
    {
        Editor ed = fresh("aaa bbb ccc");
        ed.set_textwidth(7);
        keys(ed, "<esc>gqq");
        expect(ed.text() == "aaa bbb\nccc", "gqq wraps the line at textwidth -> \"" + ed.text() + "\"");
        expect(ed.cursor() == 8, "the cursor lands on the last formatted line");
        keys(ed, "u");
        expect(ed.text() == "aaa bbb ccc", "gq undoes as one step");
        Editor para = fresh("aa\nbb cc\ndd\n\nee ff");
        para.set_textwidth(5);
        keys(para, "<esc>gggqip");
        expect(para.text() == "aa bb\ncc dd\n\nee ff", "gqip rejoins and rewraps the paragraph -> \"" + para.text() + "\"");
        keys(para, "Ggqq");
        expect(para.text() == "aa bb\ncc dd\n\nee ff", "an already wrapped line stays");
        Editor ind = fresh("  aa bb cc");
        ind.set_textwidth(7);
        keys(ind, "<esc>gqgq");
        expect(ind.text() == "  aa bb\n  cc", "gqgq keeps the indent -> \"" + ind.text() + "\"");
        Editor two = fresh("aa\nbb\ncc");
        keys(two, "<esc>gggqj");
        expect(two.text() == "aa bb\ncc", "gqj joins two lines that fit -> \"" + two.text() + "\"");
        Editor vis = fresh("aa\nbb\ncc");
        keys(vis, "<esc>ggVjgq");
        expect(vis.text() == "aa bb\ncc", "gq on a visual selection");
        Editor deflt = fresh("");
        std::string longline;
        for (int i = 0; i < 30; ++i) longline += "word ";
        keys(deflt, longline + "<esc>gqq");
        expect(deflt.text().find('\n') == 79 && deflt.text().find('\n', 80) == std::string::npos, "the default textwidth is 80");
    }
    check("Hello World", "<esc>guu", "hello world", "guu lowercases the line");
    check("Hello World", "<esc>gUU", "HELLO WORLD", "gUU uppercases the line");
    check("Hello World", "<esc>g~~", "hELLO wORLD", "g~~ toggles the line");
    check("Hello World", "<esc>gugu", "hello world", "gugu is guu");
    check("hello world", "<esc>0wgUiw", "hello WORLD", "gUiw uppercases the word");
    check("hello world", "<esc>0gUw", "HELLO world", "gUw takes a motion");
    check("Hello World", "<esc>0g~$", "hELLO wORLD", "g~$ to the end of the line");
    check("HELLO World", "<esc>0gue", "hello World", "gue");
    check("a\nb\nc", "<esc>gggUj", "A\nB\nc", "gUj is linewise");
    check("abc", "<esc>0~", "Abc", "~ toggles one character");
    check("abc", "<esc>0~~", "ABc", "~ moves right");
    check("abc", "<esc>02~", "ABc", "2~");
    check("abc", "<esc>0viwU", "ABC", "U in visual mode");
    check("ABC", "<esc>0viwu", "abc", "u in visual mode");
    check("aBc", "<esc>0v$~", "AbC", "~ in visual mode");
    check("Hello", "<esc>0vlgU", "HEllo", "gU in visual mode");
    {
        Editor ed = fresh("hello world");
        keys(ed, "<esc>$gUiw");
        expect(ed.cursor() == 6, "gUiw leaves the cursor at the start of the word");
        keys(ed, "$g~~");
        expect(ed.cursor() == 10, "g~~ leaves the cursor where it was");
    }

    section("counts");
    check("abcdef", "<esc>05x", "f", "5x");
    check("a\nb\nc\nd", "<esc>gg3dd", "d", "3dd deletes three lines");
    check("a\nb\nc\nd", "<esc>gg2dj", "d", "2dj deletes three lines");
    check("a\nb\nc\nd", "<esc>gg2d2j", "", "2d2j deletes four");
    check("ab", "<esc>$yl2p", "abbb", "2p pastes twice");
    check("a\nb", "<esc>ggyy2p", "a\na\na\nb", "2p with lines");
    check("a b c d e", "<esc>03dw", "d e", "3dw");
    check("a b c d e", "<esc>0d3w", "d e", "d3w");
    check("a b c d e f g", "<esc>02d3w", "g", "2d3w deletes six words");
    check("aXbXcXd", "<esc>03fXD", "aXbXc", "3fX then D");
    check("a b c d", "<esc>03fx", "a b c d", "a count that is not there does nothing");
    check("abcdef", "<esc>03rx", "xxxdef", "3rx");
    check("a\nb\nc\nd", "<esc>gg3J", "a b c\nd", "3J joins three lines");
    check("abc", "<esc>02sX<esc>", "Xc", "2s substitutes two characters");
    check("a\nb\nc", "<esc>gg2>>", "    a\n    b\nc", "2>> shifts two lines");
    check("abc", "<esc>0xx2u", "abc", "2u undoes twice");
    check("one two", "<esc>02cwX<esc>", "X", "2cw");
    {
        Editor ed = fresh("a\nb\nc");
        keys(ed, "<esc>2G");
        expect(ed.cursor() == 2, "2G goes to line 2");
        keys(ed, "3gg");
        expect(ed.cursor() == 4, "3gg goes to line 3");
        keys(ed, "gg2$");
        expect(ed.cursor() == 2, "2$ goes to the end of the next line");
        keys(ed, "gg2<cr>");
        expect(ed.cursor() == 4, "2 Enter moves two lines down");
    }
    check("x\ny", "<esc>gg3ia<esc>", "aaax\ny", "3ia");
    check("abcdef", "<esc>03X", "abcdef", "3X at the start does nothing");
    check("abcdef", "<esc>$3X", "abf", "3X");
    check("abcdef", "<esc>$2X", "abcf", "2X");
    check("ab\ncd\nef", "<esc>gg2D", "\nef", "2D deletes to the end of the next line");

    section("J r R s > <");
    {
        Editor ed = fresh("one\n  two\nthree");
        keys(ed, "<esc>ggJ");
        expect(ed.text() == "one two\nthree", "J joins and drops the indent -> \"" + ed.text() + "\"");
        expect(ed.cursor() == 3, "the cursor sits on the inserted space");
        keys(ed, "J");
        expect(ed.text() == "one two three", "J again");
        keys(ed, "J");
        expect(ed.text() == "one two three", "J on the last line does nothing");
    }
    check("one \ntwo", "<esc>ggJ", "one two", "no extra space after trailing white space");
    check("one\n)two", "<esc>ggJ", "one)two", "no space before a closing paren");
    check("one\n\ntwo", "<esc>ggJ", "one\ntwo", "joining an empty line adds no space");
    check("\ntwo", "<esc>ggJ", "two", "joining onto an empty line adds no space");
    check("a\nb\nc\nd", "<esc>ggVjJ", "a b\nc\nd", "J in visual mode joins the selected lines");
    check("a\nb\nc", "<esc>ggVJ", "a b\nc", "J on a one-line selection joins two");
    check("a\nb", "<esc>ggJu", "a\nb", "J undoes");
    {
        Editor ed = fresh("abc");
        keys(ed, "<esc>0rx");
        expect(ed.text() == "xbc" && ed.cursor() == 0, "rx replaces and stays");
        keys(ed, "2rY");
        expect(ed.text() == "YYc" && ed.cursor() == 1, "2rY ends on the last replaced character");
        keys(ed, "05rz");
        expect(ed.text() == "YYc", "a count past the end changes nothing");
        keys(ed, "0lr<cr>");
        expect(ed.text() == "Y\nc" && ed.cursor() == 2, "r Enter breaks the line -> \"" + ed.text() + "\"");
        keys(ed, "u");
        expect(ed.text() == "YYc", "r undoes");
        keys(ed, "r<esc>");
        expect(ed.text() == "YYc", "Esc cancels r");
        keys(ed, "0ré");
        expect(ed.text() == "éYc", "r with a multi-byte character");
        keys(ed, "$r1");
        expect(ed.text() == "éY1", "r with a digit");
    }
    check("abc", "<esc>0vlrx", "xxc", "r in visual mode replaces the selection");
    check("ab\ncd", "<esc>ggVjrx", "xx\nxx", "r on a linewise selection keeps the line breaks");
    {
        Editor ed = fresh("abcdef");
        keys(ed, "<esc>0R");
        expect(ed.mode() == Editor::Mode::Replace, "R enters replace mode");
        keys(ed, "xyz");
        expect(ed.text() == "xyzdef", "typing replaces");
        keys(ed, "<bs><bs>");
        expect(ed.text() == "xbcdef", "backspace restores what was replaced");
        keys(ed, "<esc>");
        expect(ed.mode() == Editor::Mode::Normal && ed.cursor() == 0, "Esc leaves replace mode and steps back");
        keys(ed, "u");
        expect(ed.text() == "abcdef", "a replace session undoes as one step");
        keys(ed, "$Rxyz<esc>");
        expect(ed.text() == "abcdexyz", "R past the end appends");
        Editor nl = fresh("abc");
        keys(nl, "<esc>0lR<cr>x<esc>");
        expect(nl.text() == "a\nxc", "Enter in replace mode breaks the line -> \"" + nl.text() + "\"");
    }
    check("abc", "<esc>0sX<esc>", "Xbc", "s substitutes a character");
    check("abc", "<esc>$sX<esc>", "abX", "s at the end");
    check("a\nb", "<esc>ggSx<esc>", "x\nb", "S changes the line");
    {
        Editor ed = fresh("a\nb");
        keys(ed, "<esc>gg>>");
        expect(ed.text() == "    a\nb", ">> shifts by shiftwidth -> \"" + ed.text() + "\"");
        expect(ed.cursor() == 4, "the cursor goes to the first non-blank");
        keys(ed, "<<");
        expect(ed.text() == "a\nb", "<< shifts back");
        keys(ed, "<<");
        expect(ed.text() == "a\nb", "<< on an unindented line does nothing");
        keys(ed, ">j");
        expect(ed.text() == "    a\n    b", ">j shifts both lines");
        keys(ed, "G<k");
        expect(ed.text() == "a\nb", "<k shifts both back");
        ed.set_shiftwidth(2);
        keys(ed, "gg>>");
        expect(ed.text() == "  a\nb", "shiftwidth is settable");
        keys(ed, "Vj>");
        expect(ed.text() == "    a\n  b", "> in visual mode");
        keys(ed, "ggVj2<");
        expect(ed.text() == "a\nb", "a count in visual mode shifts that many times");
        keys(ed, "gg>ip");
        expect(ed.text() == "  a\n  b", ">ip");
    }
    check("a\n\nb", "<esc>gg>ip", "    a\n\nb", ">> skips empty lines");

    section("named registers");
    {
        Editor ed = fresh("one two three");
        keys(ed, "<esc>0\"ayw");
        expect(ed.registers().count('a') && ed.registers().at('a').text == "one ", "\"ayw yanks into a");
        expect(reg == "one ", "and the unnamed register follows");
        keys(ed, "w\"byw");
        expect(ed.registers().at('b').text == "two " && ed.registers().at('a').text == "one ", "\"byw fills b, a stays");
        keys(ed, "w\"Ayw");
        expect(ed.registers().at('a').text == "one three", "\"Ayw appends to a");
        keys(ed, "$\"ap");
        expect(ed.text() == "one two threeone three", "\"ap pastes a");
        keys(ed, "$\"bp");
        expect(ed.text() == "one two threeone threetwo ", "\"bp pastes b");
        keys(ed, "\"zp");
        expect(ed.text() == "one two threeone threetwo ", "an empty register pastes nothing");
        Editor lines = fresh("a\nb");
        keys(lines, "<esc>gg\"qdd");
        expect(lines.text() == "b" && lines.registers().at('q').linewise, "\"qdd deletes the line into q as lines");
        keys(lines, "\"qp");
        expect(lines.text() == "b\na", "\"qp puts it below");
        keys(lines, "gg\"Qyy");
        expect(lines.registers().at('q').text == "a\nb", "\"Qyy appends a line");
        keys(lines, "\"qP");
        expect(lines.text() == "a\nb\nb\na", "and \"qP puts both lines above -> \"" + lines.text() + "\"");
        Editor vis = fresh("one two");
        keys(vis, "<esc>0viw\"cy");
        expect(vis.registers().at('c').text == "one", "\"c before y in visual mode");
        keys(vis, "$\"cp");
        expect(vis.text() == "one twoone", "\"cp");
        Editor ins = fresh("x");
        keys(ins, "<esc>\"ryl");
        keys(ins, "A<c-r>r<c-r>r<esc>");
        expect(ins.text() == "xxx", "Ctrl-R r in insert mode pastes register r twice");
        keys(ins, "\"syyA-<c-r>s<esc>");
        expect(ins.text() == "xxx-xxx\n", "Ctrl-R with a linewise register adds the line break");
        keys(ins, "gg0i<c-r>\"<esc>");
        expect(ins.text() == "xxx\nxxx-xxx\n", "Ctrl-R \" pastes the unnamed register");
    }

    section("Ctrl-O in insert mode");
    check("hello world", "<esc>0ihi <c-o>$!<esc>", "hi hello world!", "Ctrl-O $ goes past the end and returns to insert");
    check("abc def", "<esc>A<c-o>db!<esc>", "abc !", "Ctrl-O with an operator runs the whole command");
    check("abcdef", "<esc>0i<c-o>lX<esc>", "aXbcdef", "Ctrl-O l");
    check("abc", "<esc>A<c-o>2h-<esc>", "a-bc", "Ctrl-O with a count");
    check("abc", "<esc>A<c-o>\"ayiw-<c-r>a<esc>", "-abcabc", "Ctrl-O yank into a register (the cursor moves to its start), then Ctrl-R");
    {
        Editor ed = fresh("abc");
        keys(ed, "<esc>0i<c-o>");
        expect(ed.mode() == Editor::Mode::Normal, "Ctrl-O switches to normal mode");
        keys(ed, "$");
        expect(ed.mode() == Editor::Mode::Insert && ed.cursor() == 3, "and back after one command, past the last character");
        keys(ed, "<c-o><esc>");
        expect(ed.mode() == Editor::Mode::Insert, "Ctrl-O then Esc stays in insert mode");
        keys(ed, "<c-o>u");
        expect(ed.mode() == Editor::Mode::Insert, "Ctrl-O u stays in insert mode");
    }

    section("lines");
    check("one\ntwo", "<esc>ggyyjp", "one\ntwo\none", "yy then p puts the line below");
    check("one\ntwo", "<esc>ggyyjP", "one\none\ntwo", "P puts it above");
    check("one\ntwo", "<esc>ggddp", "two\none", "ddp swaps lines");
    check("one\ntwo", "<esc>Gdd", "one", "dd on the last line takes the line break before it");
    check("one\ntwo\nthree", "<esc>ggjdd", "one\nthree", "dd in the middle");
    check("a\nb\nc", "<esc>ggdj", "c", "dj deletes two lines");
    check("a\nb\nc", "<esc>Gdk", "a", "dk deletes two lines upward");
    check("a\nb\nc", "<esc>ggjdG", "a", "dG deletes to the end");
    check("a\nb\nc", "<esc>ggjdgg", "c", "dgg deletes to the start");
    check("a\nb\nc", "<esc>ggjcck<esc>", "a\nk\nc", "cc keeps the line's break");
    check("one two\nthree", "<esc>ggwdw", "one \nthree", "dw on the last word of a line keeps the line break");
    check("one two\nthree", "<esc>ggwd$", "one \nthree", "d$ keeps the line break");
    check("one two\nthree", "<esc>Gdb", "one \nthree", "db from the start of a line deletes the previous word but not the break");
    check("one\ntwo", "<esc>Gdb", "two", "db onto a one-word line is linewise, as in vim");
    check("a\nb", "<esc>ggAx<esc>", "ax\nb", "A appends at the end of the line, not the text");
    check("abc\ndef", "<esc>ggllax<esc>", "abcx\ndef", "a on the last character inserts before the line break");
    check("abc\ndef", "<esc>ggllx", "ab\ndef", "x on the last character");
    check("abc\ndef", "<esc>ggllxx", "a\ndef", "x again moves back first");
    check("  abc", "<esc>$Ix<esc>", "  xabc", "I inserts at the first non-blank");
    check("abc\ndef", "<esc>ggox<esc>u", "abc\ndef", "o undoes with its line");
    {
        Editor ed = fresh("abc\ndef");
        keys(ed, "<esc>gg$");
        expect(ed.cursor() == 2, "$ on a middle line stops on its last character");
        keys(ed, "l");
        expect(ed.cursor() == 2, "l does not cross the line break");
        keys(ed, "jh");
        expect(ed.cursor() == 5, "j then h stays on the line: " + std::to_string(ed.cursor()));
        keys(ed, "^");
        expect(ed.cursor() == 4, "^ on a line without indent");
        keys(ed, "$ge");
        expect(ed.cursor() == 2, "ge goes back to the end of the previous word");
        Editor ind = fresh("  abc");
        keys(ind, "<esc>$^");
        expect(ind.cursor() == 2, "^ goes to the first non-blank");
        keys(ind, "0");
        expect(ind.cursor() == 0, "0 goes to column 0");
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
        CompletionContext ctx{{"llamacpp", "comfyui"}, {"llamacpp", "anthropic"}};
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
        {
            Settings sp;
            std::string name = apply_preset(sp, "Opus 5.5");
            bool ctx = false;
            for (const auto& p : sp.providers) ctx = ctx || (p.name == "anthropic" && p.options.value("context_window", 0) == 1000000);
            expect(name == "opus-5.5" && sp.model == "anthropic/claude-opus-5-5" && sp.reviewer_model.empty() && sp.think && ctx,
                   "applying the Opus preset sets the model, thinking and the context window, and leaves reviewer_model (the user's pin) alone");
            Settings sl;
            apply_preset(sl, "qwen-9b");
            expect(sl.model == "llamacpp/Qwen3.5-9B-Q4_K_M-text" && sl.context == 16384, "a local preset also sets the server's context size");
            std::string listing = preset_lines(Settings{});
            expect(listing.find("\n  fable-5.1  anthropic/claude-fable-5-1  tier 50, limited, context 1000000, subagents on opus-5.5 (fable-5.1 is limited), reviewer haiku-4.5") != std::string::npos &&
                       listing.find("\n  opus-5.5  anthropic/claude-opus-5-5  tier 40, context 1000000, subagents on itself, reviewer haiku-4.5") != std::string::npos &&
                       listing.find("\n  qwen-4b  llamacpp/Qwen3.5-4B-Q4_K_M  tier 10, context 16384, subagents on itself, reviewer itself") != std::string::npos,
                   ":model lists each preset with its tier, limited, where its subagents run and its reviewer:" + listing);
            expect(help_text("profile").find("*agent*") == 0 && help_text("delegate").find("*task*") == 0 && help_text("agent") == help_text("profile"),
                   ":h profile finds the agent page and :h delegate the task page");
            {
                std::filesystem::path empty = std::filesystem::temp_directory_path() / ("maic-editor-agent-" + std::to_string(getpid()));
                std::filesystem::create_directories(empty);
                Agent fable(empty, "anthropic/claude-fable-5-1");
                ApiError limit(429, "anthropic returned HTTP 429: You've reached your Fable limit. Run /usage-credits to continue or switch models with /model.", 0, "rate_limit_error");
                expect(failure_text(fable, limit).find("\nfable-5.1 hit its usage limit; `:model opus-5.5` continues on the next tier (its on_limit)") != std::string::npos,
                       "a usage limit on the session's model says which :model continues, and switches nothing: " + failure_text(fable, limit));
                ApiError slow(429, "anthropic returned HTTP 429: slow down", 0, "rate_limit_error");
                expect(failure_text(fable, slow).find("usage limit") == std::string::npos, "a plain rate limit says nothing about it");
                std::filesystem::remove_all(empty);
            }
            Settings pin;
            pin.reviewer_model = "anthropic/claude-sonnet-5";
            expect(preset_lines(pin).find("tier 40, context 1000000, subagents on itself, reviewer sonnet-5") != std::string::npos, "the listing follows a reviewer_model pin");
            Settings sn;
            expect(apply_preset(sn, "llamacpp/current").empty() && sn.model == Settings{}.model, "a plain model name is not a preset and changes nothing");
            Settings ss;
            ss.presets.push_back({"qwen-4b-side", "llamacpp-2/Qwen3.5-4B-Q4_K_M", 4096, "same", -1});  // tier, limited and the lists default
            apply_preset(ss, "qwen-4b-side");
            bool side_window = false;
            for (const auto& p : ss.providers) side_window = side_window || (p.name == "llamacpp-2" && p.options.value("context_window", 0) == 4096);
            expect(ss.model == "llamacpp-2/Qwen3.5-4B-Q4_K_M" && ss.context_2 == 4096 && ss.context == Settings{}.context && side_window,
                   "a preset on the side server sets context_2 and that provider's window, not the main server's");
            unsetenv("MAIC_CONTEXT");
            unsetenv("MAIC_CONTEXT_2");
            std::vector<Provider> provs = default_providers();
            set_context(provs, 32768);
            set_context(provs, 4096, "llamacpp-2");
            int main_window = 0, side = 0;
            for (const auto& p : provs) {
                if (p.name == "llamacpp") main_window = p.options.value("context_window", 0);
                if (p.name == "llamacpp-2") side = p.options.value("context_window", 0);
            }
            expect(std::string(std::getenv("MAIC_CONTEXT")) == "32768" && std::string(std::getenv("MAIC_CONTEXT_2")) == "4096" && main_window == 32768 && side == 4096,
                   "set_context exports MAIC_CONTEXT for llamacpp and MAIC_CONTEXT_2 for llamacpp-2, and sizes each provider's readout");
            unsetenv("MAIC_CONTEXT");
            unsetenv("MAIC_CONTEXT_2");
            CompletionContext two_servers{{"llamacpp", "llamacpp-2", "comfyui"}, {"llamacpp", "llamacpp-2", "anthropic"}};
            auto gfree = complete_argument("gpu", "free l", two_servers);
            expect(gfree == std::vector<std::string>{"free llamacpp", "free llamacpp-2"}, ":gpu free completes both llama servers");
            expect(help_text("ctx2").find("*:ctx2*") == 0 && help_text("ctx2").find("llamacpp-2") != std::string::npos, ":h ctx2 is the side server's context page");
        }
        {
            // :open NAME folder / maic open --folder: a file place opens its parent, a directory itself.
            Settings so;
            fs::path ws = fs::temp_directory_path() / "maic-editor-open-test";
            fs::create_directories(ws);
            std::vector<ServiceDef> none;
            auto [c1, w1] = open_command("session", so, ws, none, ws / "t.jsonl", "", true);
            expect(c1.find("xdg-open '" + ws.string() + "'") == 0 && w1.find("holding t.jsonl") != std::string::npos, "--folder on a file place opens its parent");
            auto [c2, w2] = open_command("workspace", so, ws, none, std::nullopt, "", true);
            expect(c2.find("xdg-open '" + ws.string() + "'") == 0, "--folder on a directory place opens it");
        }
        expect(help_text("sess").find("several") != std::string::npos, "sess matches :session and sessions, so it lists both");
        expect(help_text("nope").find("no help") == 0, "an unknown topic says so");
        expect(help_text("f").find("*f*") == 0 && help_text(";") == help_text("f") && help_text(".").find("*.*") == 0 && help_text("m").find("*m*") == 0 &&
                   help_text("gq").find("*gq*") == 0 && help_text("gU") == help_text("gq") && help_text("J").find("*J*") == 0 && help_text("r").find("*r*") == 0 &&
                   help_text("R") == help_text("r") && help_text("shift").find("*>*") == 0,
               "the key topics f . m gq J r R and shift resolve");
        expect(help_text("macros").find("*macros*") == 0 && help_text("@") == help_text("macros") && help_text("q") == help_text("macros") &&
                   help_text("motions").find("*motions*") == 0 && help_text("%") == help_text("motions") && help_text("*") == help_text("motions") &&
                   help_text("diff").find("*diff*") == 0 && help_text("highlight").find("*highlight*") == 0 && help_text("enter_sends") == help_text("enter") &&
                   help_text("w").find("*:w*") == 0 && help_text("keys").find("macro") != std::string::npos,
               "the macros, motions, diff and highlight topics resolve and :h w is still the command");
        auto sets = complete_argument("set", "", CompletionContext{});
        expect(std::find(sets.begin(), sets.end(), "highlight") != sets.end() && std::find(sets.begin(), sets.end(), "enter_sends") != sets.end(), ":set completes highlight and enter_sends");
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

    section("conversation window H M L * #");
    {
        std::string r2;
        View v(&r2);
        for (int i = 1; i <= 30; ++i) v.append(Kind::User, "line " + std::to_string(i) + (i % 7 == 0 ? " seven" : ""));
        Settings s;
        v.set_timestamps(false);
        v.render(s, 80, 10);
        v.set_focused(true);
        auto at = [&](const std::string& key) {
            std::string msg = v.handle(Event::Character(key), 10);
            v.render(s, 80, 10);
            return v.status_hint();
        };
        // every user entry is a blank separator line plus its text: 59 lines, the window shows the last 10
        std::string total = at("L");
        expect(total == "59/59", "L goes to the bottom line of the window (" + total + ")");
        expect(at("H") == "50/59", "H goes to the top line of the window");
        expect(at("M") == "55/59", "M goes to the middle");
        v.handle(Event::Character("3"), 10);
        expect(at("H") == "52/59", "3H is the third line from the top");
        v.handle(Event::Character("2"), 10);
        expect(at("L") == "58/59", "2L is the second line from the bottom");
        // "line 28 seven" is four lines up from the last one (blank separators between entries); search for its last word
        at("G");
        for (int i = 0; i < 4; ++i) v.handle(Event::Character("k"), 10);
        v.handle(Event::Character("$"), 10);
        std::string msg = v.handle(Event::Character("*"), 10);
        expect(msg.find("match") == 0 && v.match_count() == 4, "* searches for the word under the cursor (" + msg + ", " + std::to_string(v.match_count()) + " matches)");
        expect(v.status_hint() == "13/59", "* lands on the next match, wrapping to the first (" + v.status_hint() + ")");
        msg = v.handle(Event::Character("#"), 10);
        expect(v.status_hint() == "55/59", "# goes to the previous match, wrapping (" + v.status_hint() + ")");
        v.handle(Event::Character("0"), 10);
        msg = v.handle(Event::Character("*"), 10);
        expect(msg.find("match") == 0 && v.match_count() > 4, "* on a word that is everywhere finds every line with it");
    }

    section("diff rendering");
    {
        expect(looks_like_diff("- old line\n+ new line\n"), "an edit preview is a diff");
        expect(looks_like_diff("exit code 0\ndiff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b"), "git diff output is a diff");
        expect(looks_like_diff("@@ -1,2 +1,2 @@\n a\n-b\n+c"), "a hunk header alone settles it");
        expect(!looks_like_diff("- item one\n- item two\n"), "a bulleted list is not a diff");
        expect(!looks_like_diff("+ only additions\n+ more\n"), "additions alone are not a diff");
        expect(looks_like_diff("new file, 2 lines:\n--- /dev/null\n+++ b/new\n+ a\n+ b"), "a new file with headers is");
        expect(diff_flags("+ added") == DiffAdd && diff_flags("-removed") == DiffDel && diff_flags("@@ -1 +1 @@") == DiffHunk &&
                   diff_flags("+++ b/x") == DiffHunk && diff_flags("--- a/x") == DiffHunk && diff_flags(" context") == MdNone && diff_flags("  … more") == MdNone,
               "line kinds: added, removed, headers, context");
        auto lines = diff_lines("- a\n+ b\n  … more");
        expect(lines.size() == 3 && lines[0][0].flags == DiffDel && lines[1][0].flags == DiffAdd && lines[2][0].flags == MdNone, "diff_lines tags every line");
        std::string r3;
        View v(&r3);
        v.set_collapse_default(false);
        v.append(Kind::Tool, "$ git diff");
        v.append(Kind::ToolOk, "exit code 0\n@@ -1 +1 @@\n-old\n+new");
        Settings s;  // a bare Settings has no styles (load_settings fills the defaults in), so name the ones looked at
        s.styles["tool_ok"] = Style{"gray_dark"};
        s.styles["diff_added"] = Style{"green"};
        s.styles["diff_removed"] = Style{"red"};
        s.styles["diff_hunk"] = Style{std::nullopt, std::nullopt, false, true};
        ftxui::Element e = v.render(s, 80, 10);
        ftxui::Screen screen(80, 10);
        ftxui::Render(screen, e);
        auto fg = [&](int y, int x) { return screen.PixelAt(x, y).foreground_color; };
        // the entries occupy the bottom rows: "▸ $ git diff", "  ⎿ exit code 0", "@@", "-old", "+new"
        expect(fg(8, 4) == parse_color("red") && fg(9, 4) == parse_color("green"), "removed lines are red and added lines green in a tool result");
        expect(fg(6, 4) == parse_color("gray_dark") && screen.PixelAt(4, 7).dim, "the result's own style stays on plain lines and the hunk header is dim");
        v.set_markdown(false);
        ftxui::Screen plain(80, 10);
        ftxui::Render(plain, v.render(s, 80, 10));
        expect(plain.PixelAt(4, 8).foreground_color == parse_color("gray_dark") && plain.PixelAt(4, 9).foreground_color == parse_color("gray_dark"), "with markdown off the diff is plain text");
    }

    section("msgpack");
    {
        using namespace maic::msgpack;
        Value m;
        m.kind = Value::Kind::Map;
        m.map.push_back({Value::str("k"), Value::boolean(true)});
        std::vector<Value> many;
        for (int i = 0; i < 20; ++i) many.push_back(Value::integer(i * 1000));
        Value v = Value::arr({Value::nil(), Value::integer(-1), Value::integer(-200), Value::integer(300), Value::integer(70000), Value::integer(-70000),
                              Value::integer(5000000000LL), Value::str(std::string(40, 'x')), Value::arr(std::move(many)), m});
        std::string bytes = encode(v);
        expect(bytes[0] == '\x9a' && bytes[1] == '\xc0' && bytes[2] == '\xff' && static_cast<unsigned char>(bytes[3]) == 0xd1, "fixarray, nil, negative fixint, int16");
        Value back;
        size_t pos = 0;
        bool ok = decode(bytes, pos, back);
        expect(ok && pos == bytes.size() && back.is_array() && back.array.size() == 10, "a round trip decodes the whole value");
        expect(back.array[1].i == -1 && back.array[2].i == -200 && back.array[3].i == 300 && back.array[4].i == 70000 && back.array[5].i == -70000 && back.array[6].i == 5000000000LL,
               "integers of every width survive");
        expect(back.array[7].is_str() && back.array[7].s.size() == 40 && back.array[8].array.size() == 20 && back.array[8].array[19].i == 19000, "str8 and array16");
        expect(back.array[9].kind == Value::Kind::Map && back.array[9].map.size() == 1 && back.array[9].map[0].first.s == "k" && back.array[9].map[0].second.b, "a map");
        pos = 0;
        Value partial;
        expect(!decode(bytes.substr(0, bytes.size() - 3), pos, partial) && pos == 0, "incomplete bytes: not yet, position untouched");
        std::string two = encode(Value::integer(1)) + encode(Value::str("ab"));
        pos = 0;
        Value a, b;
        expect(decode(two, pos, a) && decode(two, pos, b) && a.i == 1 && b.s == "ab" && pos == two.size(), "two values back to back");
        // nvim's Buffer handle: fixext 1, type 0, payload 7
        pos = 0;
        Value ext;
        expect(decode(std::string("\xd4\x00\x07", 3), pos, ext) && ext.kind == Value::Kind::Ext && ext.ext_type == 0 && ext.i == 7, "an ext handle decodes to its integer");
        pos = 0;
        Value f;
        expect(decode(encode([] { Value x; x.kind = Value::Kind::Float; x.f = 2.5; return x; }()), pos, f) && f.kind == Value::Kind::Float && f.f == 2.5, "float64");
        bool threw = false;
        try {
            pos = 0;
            Value bad;
            decode("\xc1", pos, bad);
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a byte that is not msgpack throws");
    }

    section("nvim highlighter");
    {
        expect(capture_flags("markup.heading.1") == HlHeading && capture_flags("markup.raw.block") == HlCode && capture_flags("keyword.function") == HlKeyword &&
                   capture_flags("string") == HlString && capture_flags("comment.documentation") == HlComment && capture_flags("markup.strong") == MdBold &&
                   capture_flags("markup.link.url") == MdLink && capture_flags("punctuation.special") == MdNone,
               "capture names map to the hl_* and md_* flags");
        fs::path dir = fs::temp_directory_path() / ("maic-editor-nvim-" + std::to_string(getpid()));
        fs::create_directories(dir);
        // A stand-in nvim: answers request 1 with [1, 1, nil, [[1, 1, 6, "keyword"]]] and then waits for the channel to close.
        fs::path fake = dir / "nvim";
        std::ofstream(fake) << "#!/bin/sh\nprintf '\\224\\001\\001\\300\\221\\224\\001\\001\\006\\247keyword'\ncat >/dev/null\n";
        fs::permissions(fake, fs::perms::owner_all);
        {
            NvimHighlighter hl(fake.string());
            auto lines = hl.highlight("x\n hello world", std::chrono::milliseconds(3000));
            expect(hl.alive() && lines.has_value(), "the fake nvim answers (" + hl.error() + ")");
            if (lines) {
                expect(lines->size() == 2 && (*lines)[1].size() == 3 && (*lines)[1][1].text == "hello" && (*lines)[1][1].flags == HlKeyword && (*lines)[1][0].flags == MdNone &&
                           (*lines)[1][2].text == " world",
                       "the capture becomes a keyword span on its row, the rest plain");
                auto again = hl.highlight("x\n hello world", std::chrono::milliseconds(0));
                expect(again.has_value() && again->size() == 2 && (*again)[1].size() == 3 && (*again)[1][1].flags == HlKeyword, "the same text is answered from the last reply");
                auto other = hl.highlight("changed", std::chrono::milliseconds(20));
                expect(!other.has_value() && hl.alive(), "a new text with no reply inside the budget is skipped, nvim stays");
            }
        }
        {
            NvimHighlighter missing((dir / "no-such-nvim").string());
            auto lines = missing.highlight("hello", std::chrono::milliseconds(500));
            expect(!lines.has_value() && !missing.alive() && missing.error().find("No such file") != std::string::npos, "a missing nvim: not alive, with the reason (" + missing.error() + ")");
        }
        fs::path failing = dir / "nvim-fail";
        std::ofstream(failing) << "#!/bin/sh\nprintf '\\224\\001\\001\\244boom\\300'\ncat >/dev/null\n";
        fs::permissions(failing, fs::perms::owner_all);
        {
            NvimHighlighter bad(failing.string());
            auto lines = bad.highlight("hello", std::chrono::milliseconds(3000));
            expect(!lines.has_value() && !bad.alive() && bad.error() == "nvim: boom", "an error reply turns the highlighter off with nvim's message (" + bad.error() + ")");
        }
        fs::path quitter = dir / "nvim-quit";
        std::ofstream(quitter) << "#!/bin/sh\nexit 3\n";
        fs::permissions(quitter, fs::perms::owner_all);
        {
            NvimHighlighter gone(quitter.string());
            auto lines = gone.highlight("hello", std::chrono::milliseconds(3000));
            expect(!lines.has_value() && !gone.alive() && gone.error() == "nvim exited", "an nvim that exits is reported (" + gone.error() + ")");
        }
        fs::remove_all(dir);
        bool have_nvim = false;
        if (const char* path = std::getenv("PATH")) {
            std::string p = path;
            for (size_t start = 0; start <= p.size() && !have_nvim;) {
                size_t colon = p.find(':', start);
                std::string d = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
                have_nvim = !d.empty() && access((d + "/nvim").c_str(), X_OK) == 0;
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
        }
        if (!have_nvim) {
            expect(true, "real nvim: skipped, nvim is not on PATH");
        } else {
            NvimHighlighter hl("nvim");
            std::string text = "# Title\n\n`code` and **bold**\n\n```lua\nlocal x = 'str' -- note\n```";
            std::optional<std::vector<StyledLine>> lines;
            for (int i = 0; i < 20 && !lines; ++i) lines = hl.highlight(text, std::chrono::milliseconds(1000));
            expect(hl.alive() && lines.has_value(), "real nvim answers (" + hl.error() + ")");
            if (lines) {
                auto has = [&](size_t row, unsigned flag, const std::string& piece) {
                    if (row >= lines->size()) return false;
                    for (const auto& sp : (*lines)[row]) if ((sp.flags & flag) && sp.text.find(piece) != std::string::npos) return true;
                    return false;
                };
                expect(lines->size() == 7, "one line per source line");
                expect(has(0, HlHeading, "Title"), "the heading is a heading");
                expect(has(2, HlCode, "code") && has(2, MdBold, "bold"), "inline code and bold from markdown_inline");
                expect(has(5, HlKeyword, "local") && has(5, HlString, "str") && has(5, HlComment, "note"), "a fenced lua block gets keyword, string and comment");
            }
        }
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
