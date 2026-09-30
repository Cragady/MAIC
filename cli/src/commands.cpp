#include "commands.hpp"

#include <algorithm>
#include <cctype>

namespace maic {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "Ctrl-W", "C-w", "<C-w>", "^W", ":w", "Alt+Enter", "M-CR" all reduce to a comparable key.
std::string normalize(std::string s) {
    s = lower(s);
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (const char* junk : {"<", ">"}) {
        for (size_t p; (p = s.find(junk)) != std::string::npos;) s.erase(p, 1);
    }
    for (const char* pre : {"ctrl-", "ctrl+", "control-", "c-", "^"}) {
        if (s.rfind(pre, 0) == 0) s = "ctrl-" + s.substr(std::char_traits<char>::length(pre));
    }
    for (const char* pre : {"alt-", "alt+", "meta-", "m-"}) {
        if (s.rfind(pre, 0) == 0) s = "alt-" + s.substr(std::char_traits<char>::length(pre));
    }
    if (s == "alt-cr" || s == "alt-return") s = "alt-enter";
    if (s == "cr" || s == "return") s = "enter";
    if (s == "esc") s = "escape";
    return s;
}

struct Topic {
    std::string name;
    std::vector<std::string> aliases;
    std::string summary;
    std::string text;
};

const std::vector<Topic>& topics() {
    static const std::vector<Topic> t = {
        {"modes", {"mode-list", "manual", "auto-read", "edit", "auto", "plan"}, "what the agent may do without asking",
         "*modes*\n"
         "Shift-Tab cycles them; `:mode NAME` sets one; `--mode` on the command line; `mode` in settings.\n\n"
         "- **manual** (default): asks before every edit and every command.\n"
         "- **auto-read**: reads anywhere and read-only commands (ls, cat, grep, git log, ...) run on their own, in a sandbox where even the workspace is read-only; edits and other commands ask.\n"
         "- **edit**: edits inside the workspace apply on their own; commands ask.\n"
         "- **auto**: edits and sandboxed commands inside the workspace run on their own; writes outside it ask.\n"
         "- **plan**: read-only. Reads and read-only commands only; the model proposes a plan.\n\n"
         "In every mode: secrets are never read, system paths are never written, startup files and MAIC's own harness are always asked about, dangerous commands trip the harness, and a request from another origin is always asked. See `:h harness`."},
        {"harness", {"tripwire", "sandbox", "trip", "lock", "dumb", "smart", "reviewer"}, "what protects the machine; :harness smart|dumb",
         "*harness* *:harness* *--harness*\n"
         "Every tool call is checked before it runs: tripwire, then policy for the mode, then the reviewer, then approval, then the sandbox.\n\n"
         "- **reviewer** (the **smart** harness, the default): before any command or write that the rules would let through *without asking* (auto and edit modes), a model reads the last few things you said, the agent's last words and the action, and answers ALLOW, ASK or DENY. ASK becomes an approval prompt, DENY refuses with the reason, and a reviewer that cannot answer means ASK. Reads are never reviewed; what you approved yourself is not reviewed either. `reviewer_model` in settings picks the model (default: the session's).\n"
         "- **dumb harness**: `:harness dumb`, `--harness dumb`, or `harness = \"dumb\"` in settings turns the reviewer off; the rule list alone decides and nothing reads the conversation. Entering **auto** under it shows a warning once per session and asks you to confirm; `dumb_auto_ok = true` (or `--accept-dumb-auto`) skips that. Headless runs refuse dumb + auto without one of those. The status strip shows DUMB HARNESS.\n"
         "- **tripwire**: a root-owned lock. `:trip REASON` (or the [t] answer at an approval, or a dangerous command) sets it instantly with no password. While tripped nothing runs, but the session survives. `:unlock` resets it and asks for your sudo password.\n"
         "- **sandbox**: every model-run command executes in bubblewrap: only the workspace writable, secrets hidden, no network, no sudo, a timeout.\n"
         "- **approval**: y / n / N (no, and type a sentence the model gets as the reason) / a (always this file or program, this session) / t (trip). Edits show the lines that would change.\n"
         "- **repeated calls**: the same call three times in a row is refused; five times trips the lock. Three denials by you in one turn end the turn.\n"
         "- **undo points**: every file the agent changes is saved first; `:undo` restores. See `:h undo`.\n\n"
         "Details and the planned layers: docs/harness.md."},
        {"sessions", {"session", "resume", "transcript", "transcripts"}, "transcripts, -c, -r, forking",
         "*sessions*\n"
         "Every session is a JSONL file under ~/.local/state/maic/sessions (0600), in a home: `general/` by default, `projects/<encoded workspace>/` when settings say `\"sessions_home\": \"project\"`, or any name. `:session` shows this one and its home; `maic sessions` lists them with where each was started and last opened; `maic sessions rehome ID project|general|NAME` moves one.\n\n"
         "- `maic -c` continues the newest session from the current directory; `maic -r` picks from a list; `maic -r ID` (a unique prefix is enough); `maic -r PATH` resumes any transcript file by path, including a temporary one under $XDG_RUNTIME_DIR from `--no-record` (those are never listed, so `-c` cannot find them).\n"
         "- Interactive resumes append to the same file. `--no-append` writes a new file that only points at the old one and the number of records loaded, which is also how a session forks. `--no-record` (or `\"record\": false` in settings, or `maic -p` without `--record`) keeps the transcript in the runtime directory instead, where it disappears at logout; it is never listed.\n"
         "- `maic -r ID --fork-at N` (also with `-c`, and with `-p`) continues from the first N records of that file only, in a new file that points at them; the old file is never changed. `maic sessions path ID` finds the file to count records in.\n"
         "- `maic sessions import FILE` turns a claude.ai export (JSON) or a Claude Code transcript (JSONL) into a session here and prints its id; `--as` names the format when detection guesses wrong, `--home` picks where it goes, `--conversation UUID` picks one out of a full export.\n"
         "- `maic sessions redact ID` writes `./<id>.redacted.jsonl` with credential material replaced by `[REDACTED:kind]` and reports counts per kind; `-o FILE` or `--in-place` choose where. Record types and the redaction kinds: docs/sessions.md.\n"
         "- A session that ended mid tool call resumes from the last complete step. The model is told it resumed, with the current mode and instructions."},
        {"headless", {"-p", "print", "cli", "command-line", "context", "-C", "--context", "interactive", "-i"}, "maic -p, stdin, --context files, --interactive",
         "*headless* *-p* *--context* *--interactive*\n"
         "`maic -p \"prompt\"` runs one turn without the UI: the reply streams to stdout, tool activity to stderr. `maic -p -` takes the prompt from stdin (`cat dialog.txt | maic -p -`). `--json` prints events as JSON lines. Approvals are asked on the terminal when there is one, otherwise denied; `--mode auto-read` is the usual choice for scripts.\n\n"
         "**Context.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several; `-C -` reads stdin (then the prompt must be an argument). Combine with `-c` / `-r` to put a file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maic`.\n\n"
         "**Transcripts.** A headless run writes its transcript to the runtime directory ($XDG_RUNTIME_DIR/maic/sessions, gone at logout) unless `--record` (with `-c`/`-r`: a new file that points at the old one, a fork) or `--append` (writes into the old file). `maic --no-record` does the same for an interactive session; `\"record\": false` in settings makes it the default.\n\n"
         "**--interactive** (`-i`) with `-p` opens an interactive session that starts with the prompt already sent. It follows interactive rules whatever the order of the flags: a transcript is always kept, `-c`/`-r` continue in the same file unless `--no-append`; `--record` is redundant and `--json` is ignored.\n\n"
         "Short flags cluster: `maic -pi -` is `-p -i -`; a flag that takes a value (`-m`, `-C`) goes last in a cluster. `maic help TOPIC` prints these pages outside a session."},
        {"queue", {"queued", "mid-turn", "interrupt"}, "sending while the agent works",
         "*queue*\n"
         "Sending while the agent is busy queues the message; it reaches the model at its next step in the current turn. `:w now` delivers it immediately: the current output is abandoned and the model is asked again with your message included. Messages still queued when a turn ends start the next turn. Ctrl-C interrupts the turn instead."},
        {"providers", {"provider", "models", "remote", "ollama", "anthropic", "deepseek", "openrouter"}, "local and remote models",
         "*providers*\n"
         "Models are `provider/model`: `qwen3.5:9b` (Ollama, local), `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, `openrouter/...`, or any OpenAI-compatible server added in settings. `:model` alone lists providers; `:models` lists what Ollama has.\n\n"
         "A remote provider receives your prompts, every file the agent reads and every command's output; MAIC says so when you switch and shows REMOTE in the status strip. Keys come from an environment variable or a command, never from the settings file. See docs/settings.md."},
        {"server", {"remote", "phone", "token", "tls", "maic-server"}, "remote access from a phone: the server, tokens, TLS",
         "*server* *maic-server*\n"
         "`maic server start [--listen ADDR:PORT] [--model M] [--mode MODE]` serves agent sessions over HTTP with server-sent events, and a one-file web client at `/` for a phone. Loopback (127.0.0.1:7373) by default; any other address turns TLS on, with a self-signed certificate made on first use (its fingerprint is printed) or the pair set in `server.cert` / `server.key`.\n\n"
         "Every request needs a per-device bearer token: `maic server token new NAME` prints one once and keeps only its hash; `token list` and `token revoke NAME` manage them. Failed attempts are rate limited per source and every request is written to `~/.local/state/maic/server/audit.log`. `maic server status` shows the configuration and whether a server answers.\n\n"
         "Every tool call from a remote client is asked about, whatever the mode; approvals are answered from the client. The tripwire can be tripped from a client and never reset: there is no route for it. Sessions are recorded like any other (kind server) and `maic -r ID` continues one at the terminal. Workspaces are limited to `server.workspaces` in settings. See docs/remote.md."},
        {"vendor", {"vendored", "install-services", "artifacts-tree"}, "services MAIC installs for itself, and the artifact tree",
         "*vendor*\n"
         "`maic vendor` lists the services MAIC can install at pinned versions (ComfyUI as a submodule at a release tag, its Ollama custom node at a pinned commit, Ollama as a checksum-verified release). `maic vendor add NAME` fetches and installs one the way MAIC wants it (own Python, telemetry off, models on the external drive); `maic vendor adopt NAME PATH` uses an install you already have; `maic vendor unlink NAME` stops using it. Everything lives under ~/.local/state/maic/vendor/, workflows under ~/.local/state/maic/workflows/, and `maic artifacts` shows where each thing really is. See docs/vendor.md."},
        {"settings", {"config", "styles", "style", "settings.lua", "settings.json"}, "the settings file",
         "*settings*\n"
         "Lua files returning a table (JSON works too). Layered: ~/.config/maic/settings.lua, then `.maic/settings.lua` and `.maic/settings.local.lua` in each directory from under $HOME down to the workspace (nearest wins; settings.lua is for the project, settings.local.lua is personal). A file is code: `os.getenv`, `maic.hostname`, `maic.home` for per-machine choices. Keys: model, mode, think, markdown, mouse, record, compact_at, sessions_home (auto/general/project/name), models_dir, leader, instruction_files, providers, style. `maic settings init` writes the global one, `:init` scaffolds a project's, `:settings` shows what is in effect. See docs/settings.md."},
        {"tools", {"tool", "lua-tools", "glob", "question", "todo-tool", "user-tools"}, "the model's tools, and writing your own in Lua",
         "*tools*\n"
         "Built in: `read_file`, `list_dir`, `glob` (files by name pattern), `search_files` (grep -E), `write_file`, `edit_file`, `run_shell` (bubblewrap sandbox), "
         "`question` (asks you something, with options; a number picks, or type an answer, Esc gives none) and `todo` (the model's plan; `:todo` shows it, the status strip counts it). "
         "Every one goes through the harness. `:tools` lists them with any tools of your own.\n\n"
         "**Your own tools** are Lua files: `.maic/tools/<name>.lua` in the workspace or `~/.config/maic/tools/<name>.lua`, loaded when a session starts. A file returns a table: "
         "`name`, `description`, `parameters` (a JSON schema as a Lua table) and `run = function(args) ... end` returning a string or a table. The model calls it like any other tool. "
         "Each call runs in its own LuaJIT state with only the base, string, table, math and bit libraries: no io, os, require or load. Inside, `maic.read(path)`, `maic.write(path, text)`, "
         "`maic.list(path)`, `maic.search(pattern, path)` and `maic.shell(cmd, opts)` each go through the harness exactly as the built-in tool would (policy, your approval, the sandbox); a denial "
         "is a Lua error carrying the reason, so the tool fails and the model sees why. `maic.json_encode` / `maic.json_decode` convert. A tool is stopped after 60 s or on Ctrl-C, and its output is capped at 64 KB. "
         "A file that fails to load is skipped with a notice. `maic tools` lists them outside a session. Format and a complete example: docs/tools.md and tools/examples/word-count.lua."},
        {"instructions", {"maic.md", "agents.md", "claude.md"}, "standing instructions the model always sees",
         "*instructions*\n"
         "~/.config/maic/MAIC.md, then every MAIC.md or AGENTS.md from under $HOME down to the workspace, re-read at the start of every turn (32 KB each); an AGENTS.md deeper in the tree is attached the first time a file under it is read. `:instructions` shows what is in effect; `:instructions off`, `--no-instructions` or `load_instructions = false` loads none, and `:system` / `--system` places operator text ahead of all of them (see `:h system`)."},
        {"keys", {"keybindings", "bindings", "motions", "vim"}, "the key map",
         "*keys*\n"
         "The input is a small vim and starts in normal mode.\n\n"
         "- **insert**: `i a I A o O` enter it (a count repeats what you type); Enter = new line; Esc = normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register, Ctrl-R {reg} a named one; Ctrl-O runs one normal-mode command; ↑ ↓ or Ctrl-P / Ctrl-N prompt history.\n"
         "- **normal**: `h j k l w b e ge 0 ^ $ gg G` move (Enter = down a line); `f{c} F{c} t{c} T{c}` to a character on the line, `;` and `,` repeat; `}` `{` paragraphs, `)` `(` sentences; `x X D C S s J r R ~`; `d c y > < gq gu gU g~` + motion, doubled for the line (`dd`, `>>`, `gqq`, `gUU`); text objects `iw aw ip ap is as i\" i( i[ i{ i<`; `v V`; `p P`; `.` repeats the last change; `m{a-z}` marks, `'a` / `` `a `` jump; `\"a`-`\"z` registers; `u` undo, Ctrl-R redo; counts (`3w`, `2d3w`, `5.`); `:` commands; `/` searches the conversation.\n"
         "- **send**: Alt+Enter or `:w` from any mode. `:e` or Ctrl-X Ctrl-E edits the input in nvim.\n"
         "- **conversation window**: Ctrl-W k enters it, Ctrl-W j (Esc, i, Enter) returns; motions including `f t ; ,`, `v V`, `y` yanks to the clipboard, `yy`, `/ n N`, `}` `{` between messages.\n"
         "- **anywhere**: Shift-Tab cycles modes; Ctrl-C interrupts, then clears, then quits; the scroll wheel scrolls.\n\n"
         "`:h KEY` works for single keys too: `:h u`, `:h f`, `:h .`, `:h m`, `:h gq`, `:h J`, `:h r`, `:h Ctrl-W`, `:h Alt+Enter`."},
        {"conversation", {"window", "ctrl-w", "focus", "yank", "clipboard", "search", "/"}, "the conversation window as a vim buffer",
         "*conversation* *Ctrl-W*\n"
         "Ctrl-W k moves the cursor into the conversation window; Ctrl-W j, Esc, `i` or Enter bring it back (Ctrl-W in insert mode deletes a word, so press Esc first unless the input is empty). Inside: `j k h l w b e 0 $ gg G`, Ctrl-D/U/F/B; `}` / `{` next / previous message, `]]` / `[[` next / previous message of yours; `v` / `V` select, `o` swaps the ends; `y` yanks the selection to the register and the system clipboard (wl-copy, xclip, and the terminal through OSC 52); `yy` a line; `/pattern` then `n` / `N` search, smart case. Ctrl-Shift-C in your terminal still copies mouse selections; with the scroll wheel on, select with Shift+drag."},
        {"alt-enter", {"send"}, "sends the input", "*Alt+Enter*\nSends the input from any mode; the same as `:w`. Enter is a new line. nvim has no default Alt mappings, so nothing is lost."},
        {"enter", {}, "a new line", "*Enter*\nInsert mode: a new line. Normal mode: down a line. To send, use Alt+Enter or `:w`."},
        {"escape", {}, "back to normal mode", "*Esc*\nInsert or visual mode to normal mode; in the command line, cancels; in the conversation window, back to the input."},
        {"u", {"redo"}, "undo", "*u* *Ctrl-R*\n`u` undoes the last change in the input, `Ctrl-R` redoes (for the agent's file changes see `:h :undo`). Two hundred levels. An insert session counts as one step, and so does a change operator (`cw`, `cc`, `C`, `S`) together with what you typed after it."},
        {"ctrl-r", {"redo"}, "redo", "*Ctrl-R*\nRedo. See `:h u`."},
        {"ctrl-c", {}, "interrupt, clear, quit", "*Ctrl-C*\nWhile the agent works: interrupts the turn. While a `!command` runs: stops it. Otherwise: clears the input; pressed twice on an empty input: quits (or `:q`)."},
        {"shift-tab", {"tab"}, "cycle the mode", "*Shift-Tab*\nCycles manual → auto-read → edit → auto → plan. See `:h modes`."},
        {"ctrl-x", {"ctrl-x ctrl-e", "nvim", "editor"}, "edit the input in nvim", "*Ctrl-X Ctrl-E*\nOpens the input in $VISUAL, $EDITOR or nvim as a markdown file and loads it back when you quit. Same as `:e`."},
        {"v", {"visual", "visual-mode"}, "visual selection", "*v* *V*\n`v` selects by character, `V` by line, in the input or the conversation window. Then `y` yanks, `d` deletes (input only), `c` changes, `o` swaps the ends, Esc leaves."},
        {"i", {"insert", "a", "o", "ctrl-o", "s"}, "insert mode", "*i* *a* *I* *A* *o* *O* *s* *Ctrl-O*\n`i` inserts before the cursor, `a` after, `I` at the first non-blank, `A` at the line end, `o` opens a line below, `O` above; `s` deletes the character first (`3s` three), `S` the line. A count repeats what you typed: `3ix<Esc>` gives `xxx`, `2ofoo<Esc>` two lines. In insert mode Ctrl-O runs one normal-mode command and comes back (`Ctrl-O $`, `Ctrl-O dw`), Ctrl-R then a register name pastes it. Esc returns to normal mode."},
        {"p", {"paste", "register", "reg", "registers", "ctrl-y"}, "paste; named registers", "*p* *P* *Ctrl-Y* *Ctrl-R* *\"+p* *\"a*\n`p` pastes the register after the cursor, `P` before, `3p` three times; lines (from `dd`, `yy`, `yj`, `dap`) go on their own line below or above. In insert mode Ctrl-Y pastes the register and Ctrl-R {reg} a named one (`Ctrl-R a`, `Ctrl-R \"`, `Ctrl-R +`). `\"+p` (or `\"*p`, or Space then `p`) pastes the system clipboard (wl-paste, xclip or xsel).\n"
         "Registers: `\"a` to `\"z` before a yank, delete or paste use that register (`\"ayy`, `\"ap`, `\"bdiw`); a capital letter appends (`\"Ayw`). The unnamed register (what plain `p` uses) always holds the last yank or delete, from the input or the conversation window. `:reg` shows them all."},
        {"y", {"yank", "leader", "space", "text-objects", "iw", "aw", "ip", "ap", "is", "as"}, "yank: motions, text objects, the clipboard", "*y* *Y* *\"+y* *<leader>y* *text-objects* *ip* *is*\n"
         "`y` + motion yanks (`yw`, `y$`, `yy`, `Y`, `yj`, `y}`); `d`, `c`, `>`, `<`, `gq`, `gu`, `gU` and `g~` take the same motions. Text objects work after an operator and in visual mode: `iw` `aw` (word), `iW` `aW`, `is` `as` (sentence), `ip` `ap` (paragraph: the lines up to an empty line, `ap` with the empty lines after it), `i\"` `a\"`, `i'` `a'`, `` i` ``, `i(` `a(` (also `ib`), `i[` `a[`, `i{` `a{` (also `iB`), `i<` `a<`. So `ciw`, `dap`, `gqip`, `di\"`, `ya(`, `vip`.\n"
         "The system clipboard: `\"+y` (or `\"*y`) before any yank, or the leader (Space by default, `leader` in settings) then `y`: `<leader>y` yanks the line in normal mode or the selection in visual mode. In the conversation window every yank already reaches the clipboard, and `yiw`, `yw`, `y$`, `Y`, `yy` work there too."},
        {"f", {"t", "F", "T", ";", ",", "find-char"}, "f t F T to a character, ; and , repeat", "*f* *F* *t* *T* *;* *,*\n"
         "`f{c}` moves to the next `c` on the line, `F{c}` to the previous; `t{c}` and `T{c}` stop one short of it. `;` repeats the last one, `,` repeats it the other way; a count picks the nth match (`3fa`, `2;`). As operator targets they take the character with `f` and `t` (`df)`, `ct,`, `yf.`) and stop before the cursor with `F` and `T` (`dF(`). Work in the conversation window too."},
        {".", {"dot", "repeat"}, "repeat the last change", "*.*\n"
         "`.` repeats the last change: an operator with its motion or text object (`dw`, `ciw` and the text typed after it, `>>`, `gUiw`), `x`, `p`, `J`, `r`, `~`, or a whole insert session (`A!<Esc>` then `.` appends `!` to another line). Its count is kept (`2x` then `.` deletes two); a count on `.` replaces it (`5.`). `u`, Ctrl-R, motions, yanks and the prompt history are not changes."},
        {"m", {"mark", "marks", "'", "`"}, "marks", "*m* *'* *`*\n"
         "`m{a-z}` sets a mark at the cursor. `'a` jumps to the first non-blank of its line, `` `a `` to the exact position; `''` and ``` `` ``` go back to where the last jump (`G`, `gg`, `}`, `)`, a mark) started. Marks are motions too: `d'a` deletes the lines between here and the mark, `` y`a `` yanks up to it exactly. Marks follow the text when you edit before them and are cleared when the input is sent."},
        {"gq", {"format", "textwidth", "gu", "gU", "g~", "~", "case", "wrap"}, "gq re-wraps, gu gU g~ change case", "*gq* *gu* *gU* *g~* *~*\n"
         "`gq{motion}` re-wraps the lines the motion covers at 80 columns (`textwidth`): `gqq` or `gqgq` the line, `gqip` the paragraph, `gqj` two lines, `gq` in visual mode the selection. Words are re-flowed, the first line's indent is kept, and empty lines stay as paragraph breaks. The cursor lands on the last formatted line.\n"
         "`gu{motion}`, `gU{motion}` and `g~{motion}` lowercase, uppercase and toggle (`gUiw`, `guu`, `gUU`, `g~~`, `gugu`); `~` alone toggles the character under the cursor and moves right (`5~`). In visual mode `u`, `U` and `~` do the same to the selection."},
        {"J", {"join"}, "join lines", "*J*\n"
         "`J` joins the next line onto this one: its leading white space goes and one space is put between them, except when the line already ends in white space, the next line is empty, or it starts with `)`. `3J` joins three lines; in visual mode `J` joins the selected lines. The cursor sits on the join."},
        {"r", {"R", "replace", "replace-mode"}, "r replaces a character, R enters replace mode", "*r* *R*\n"
         "`r{c}` replaces the character under the cursor with `c` and stays put; `3rx` replaces three, and fails when the line is shorter; `r<Enter>` breaks the line there. In visual mode `r{c}` replaces every selected character.\n"
         "`R` enters replace mode: what you type overwrites the text (Enter still breaks the line), Backspace puts the original character back, Esc returns to normal mode; the session undoes as one step and `3Rab<Esc>` writes `ababab`."},
        {">", {"<", "shift", "shiftwidth", "indent"}, "> and < shift lines", "*>* *<*\n"
         "`>{motion}` indents the lines the motion covers by four spaces (`shiftwidth`), `<{motion}` removes up to four; `>>` and `<<` do the line, `3>>` three lines, `>ip` the paragraph. In visual mode `>` and `<` shift the selection, and a count shifts that many times (`2>`). Empty lines are left alone."},
        {"!", {"shell", "bang"}, "run a command in your shell", "*!* *:!*\n`!cmd` as a message, or `:!cmd`, runs cmd in your own shell (not the sandbox) in the workspace. The output shows in the conversation and is handed to the model as context. Ctrl-C stops it."},
    };
    return t;
}

}  // namespace

const std::vector<CommandInfo>& commands() {
    static const std::vector<CommandInfo> c = {
        {"w", {"write", "send"}, "[now]", "send the input (also Alt+Enter)",
         "*:w* *:write* *:send*\n`:w` sends the input, the same as Alt+Enter. `:w now` sends even while the agent is working: the current output is abandoned and the model is asked again with your message included. Without `now`, a message sent while the agent is busy waits for its next step. See `:h queue`."},
        {"ww", {}, "", "send now, even mid-turn (= :w now)",
         "*:ww*\nThe same as `:w now`: sends immediately even while the agent is working. See `:h w`."},
        {"e", {"edit", "nvim"}, "", "edit the input in nvim",
         "*:e* *:edit* *:nvim*\nOpens the input in $VISUAL, $EDITOR or nvim as a markdown file; when you quit, the file becomes the input (one undo step). A non-zero exit leaves the input unchanged. Also Ctrl-X Ctrl-E."},
        {"h", {"help", "topics"}, "[topic]", "this help, or :h TOPIC",
         "*:h* *:help* *maic help*\n`:h` alone lists every topic. `:h TOPIC` shows one: a command (`:h w`), a key (`:h u`, `:h Ctrl-W`, `:h Alt+Enter`) or a concept (`:h modes`, `:h harness`, `:h sessions`). A unique prefix is enough; several matches give a list.\n\nOutside a session `maic help` prints the command summary and `maic help TOPIC` one of these pages, both on stdout so they pipe (`maic help lua | less`, `maic help | grep vendor`). `maic help topics` prints the index."},
        {"harness", {}, "[smart|dumb]", "the reviewer on (smart) or the rule list alone (dumb)",
         "*:harness*\n`:harness` shows which is in force; `:harness smart` turns the model reviewer on, `:harness dumb` off. Switching to dumb while in auto mode drops to edit until you confirm auto again. See `:h harness`."},
        {"mode", {}, "NAME", "set the agent mode",
         "*:mode*\n`:mode manual|auto-read|edit|auto|plan`. Shift-Tab cycles them. See `:h modes`."},
        {"model", {}, "[NAME]", "switch model, or list providers",
         "*:model*\n`:model NAME` switches (when the agent is idle): `qwen3.5:9b`, `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, ... `:model` alone lists the providers. Switching to a remote provider prints what will leave this machine. See `:h providers`."},
        {"models", {}, "", "models the Ollama server has", "*:models*\nLists the models on the current Ollama provider (`ollama list`)."},
        {"think", {}, "on|off", "let the model reason first", "*:think*\n`:think on` asks the model to reason before answering: slower, better on hard problems. Anthropic models then use the provider's `think_effort`."},
        {"set", {}, "markdown|mouse on|off", "rendering and mouse toggles",
         "*:set*\n`:set markdown off` shows the conversation as raw text; `on` renders it. `:set mouse off` stops the scroll wheel and gives the terminal its normal mouse selection back. `:set tooldetails on` shows tool output in full instead of an 8-line preview (in the conversation window `za` folds or unfolds one result, `zR` unfolds all, `zM` folds all). markdown and mouse persist through settings.lua."},
        {"status", {}, "", "harness, services, model, session",
         "*:status*\nThe harness state, every service with where it runs (host process, pid, url) and a quick action, the model and whether it is remote, this session's file, the mode, queued messages, the user-defined tools and the model's todo list."},
        {"todo", {"plan"}, "", "the model's plan (the todo tool)",
         "*:todo*\nShows the list the model keeps with its `todo` tool during multi-step work: `[x]` done, `[ ]` not yet. The status strip shows `todo n/m done` while there is one; `:clear` drops it. See `:h tools`."},
        {"tools", {}, "", "the model's tools, built in and yours",
         "*:tools* *maic tools*\nLists the built-in tools and every user-defined Lua tool with its file and description, plus files that were skipped and why. Outside a session `maic tools` does the same. Writing one: `:h tools` (the topic) and docs/tools.md."},
        {"up", {}, "SERVICE", "start a service", "*:up*\n`:up ollama` starts a service MAIC manages (ollama, comfyui). Refused while the harness is tripped."},
        {"down", {}, "SERVICE", "stop a service MAIC started", "*:down*\n`:down ollama` stops it. MAIC only stops what it started."},
        {"init", {}, "", "scaffold MAIC.md and .maic/settings.lua, then draft the MAIC.md",
         "*:init*\nCreates `.maic/settings.lua` and a `MAIC.md` placeholder in the workspace, then asks the agent to look over the project and write the MAIC.md (it will ask before writing in manual mode). A project with a MAIC.md keeps its transcripts under sessions/projects/. `maic init` does the scaffolding only."},
        {"settings", {}, "", "which settings files are in effect",
         "*:settings*\nLists the settings files that were read, nearest last: the global file, then `.maic/settings.lua` and `.maic/settings.local.lua` (or their .json fallbacks) from just under $HOME down to the workspace. Shows where this session's transcript home resolved to. See `:h settings`."},
        {"ban", {"bans", "banned", "logit_bias", "logit-bias", "pattern", "patterns", "regex"}, "[add TEXT|pattern REGEX|token ID|remove N|clear]", "strings, regex patterns and tokens the model must not produce",
         "*:ban* *--ban* *--ban-pattern* *bans*\n"
         "**Files**: wherever a ban is given, `@path` stands for a file with one entry per line (`#` lines and blank lines skipped, `~` expands): `--ban @~/bans/phrases.txt`, `--ban-pattern @~/bans/tics.re`, `:ban add @file`, `:ban pattern @file`, `:ban token @file`, and `strings = { \"@~/bans/phrases.txt\" }` in settings.\n\n"
         "**Regex bans** (`:ban pattern REGEX`, `--ban-pattern`, `bans.patterns` in settings; POSIX extended, so `(as an ai|i cannot|certainly!)` with `:ban case off`) are matched over the streamed reply the same way as string bans: the reply is cut before the match reaches the screen, the model is told what it started and asked to continue, and after `retries` attempts the match is replaced. A regex cannot say how much more text might complete a match, so the last `window` characters (64) are held back until more text arrives; keep the window longer than any phrase you ban. One regex holding every tic a model is known for is the intended use: see docs/bans.md for a starter set. This is done by MAIC after the fact, not by constrained decoding: grammar-guided decoding on llama.cpp or vLLM can say what a reply must look like, not what it must not contain.\n\n"
         "**String bans** work with every provider: the reply streams through a filter that holds back a short tail and cuts the call the moment a banned phrase would appear, before it reaches the screen; the clean part is kept, the model is told which phrase was banned and asked to continue; after `retries` attempts (3) the phrase is replaced by `replacement` (\"[banned]\") instead. `:ban add TEXT`, `--ban TEXT` (repeatable), or `bans = { strings = { ... } }` in settings; layers add up. `:ban case off` matches regardless of case.\n\n"
         "**Token bans** map to `logit_bias` (the token's probability goes to minus infinity, so the model takes another path) on OpenAI-compatible providers: llama.cpp server, vLLM, LM Studio and the like. `:ban token 1234` bans an id; `:ban token TEXT` bans text, which llama.cpp-style servers accept in logit_bias and which is a string ban everywhere else. Ollama's own API and Anthropic have no logit bias, so ids are ignored there with one notice.\n\n"
         "**Samplers**: see `:h sampling`."},
        {"sampling", {"sampler", "xtc", "temperature", "top_k", "min_p"}, "[KEY VALUE|xtc P [T]|unset KEY|reset]", "sampler settings: temperature, top_k, min_p, XTC, ...",
         "*:sampling* *sampling* *xtc* *--xtc* *--sampling*\n"
         "On the command line: `--xtc 0.5` or `--xtc 0.5,0.1` (probability, threshold) and `--sampling KEY=VALUE` (repeatable) set them for one run, over the settings. In a session: `:sampling` shows what is sent with every request; `:sampling temperature 0.7`, `:sampling min_p 0.05`, `:sampling seed 7` set a key for this session (over `sampling = { ... }` in settings and `providers.<name>.options.sampling`, which wins over the global table); `:sampling unset KEY`, `:sampling reset`.\n\n"
         "**XTC** (exclude top choices) is a sampler that, with probability P, drops every token above threshold T except the least likely of them, so the model is pushed off its most predictable path, which is where refusals and stock phrases live. `:sampling xtc 0.5 0.1` (or `sampling = { xtc_probability = 0.5, xtc_threshold = 0.1 }`). It exists in llama.cpp's server, koboldcpp, text-generation-webui and other llama.cpp-based OpenAI-compatible servers, where MAIC sends it as `xtc_probability` / `xtc_threshold`. Ollama's API has no XTC (its options are temperature, top_k, top_p, min_p, seed, repeat_penalty, num_predict), and Anthropic's current models take no sampling parameters, so nothing is sent there; the keys are kept for when you switch provider. Bans and XTC combine: XTC changes what the model is likely to say, bans catch what it says anyway."},
        {"system", {"system-prompt", "operator"}, "[TEXT|@FILE]", "operator instructions placed first in the system prompt",
         "*:system* *--system* *system_prompt*\n"
         "Text that leads every system prompt, before MAIC's own briefing and before any instruction file, marked as operator instructions that take precedence, and repeated at the very end of the prompt (small models drop a short rule buried under the briefing and keep one that closes it): the way to front-load behaviour. Set it with `--system TEXT` or `--system @~/prompts/reviewer.md` on the command line, `system_prompt = \"...\"` or `\"@path\"` in settings, or `:system TEXT` / `:system @file` in a session (idle only; it applies from the next turn and is appended to a resumed conversation). `:system` alone shows it. Independent of instruction files: combine with `--no-instructions` to run on the operator text alone."},
        {"instructions", {"no-instructions", "load_instructions"}, "[on|off]", "the MAIC.md / AGENTS.md files in effect, or switch them off",
         "*:instructions* *--no-instructions*\nLists the instruction files the model sees, re-read every turn. `:instructions off` stops loading them (global, project and nested) for the next turns; `on` brings them back. `--no-instructions` on the command line or `load_instructions = false` in settings starts that way. Independent of `:system`. See `:h instructions`."},
        {"session", {}, "", "where this transcript is", "*:session*\nThis session's file and the sessions directory. See `:h sessions`."},
        {"artifacts", {}, "", "where everything is kept, with sizes", "*:artifacts*\nEvery place MAIC and its services leave things (transcripts, service logs, ComfyUI outputs, ...) with sizes. Clean with `maic artifacts clean OWNER/NAME [--older-than DAYS]`."},
        {"reg", {"register", "registers"}, "", "show the registers", "*:reg*\nShows the unnamed register and every named register `\"a`..`\"z` that holds something. See `:h p`."},
        {"undo", {}, "[N]", "restore the file(s) the agent changed last",
         "*:undo*\nEvery write_file / edit_file saves the file's previous content first. `:undo` restores the newest one (`:undo 3` the newest three); a file that did not exist is removed. The model is told what was undone; the transcript records it. Points live for the session."},
        {"copy", {}, "", "copy the last reply to the clipboard", "*:copy*\nCopies the newest assistant reply to the register and the system clipboard."},
        {"export", {}, "[FILE]", "write the transcript as markdown",
         "*:export* *maic sessions export*\n`:export` writes this session as markdown (## User / ## Assistant, tool calls in fenced blocks) to `<session id>.md` in the workspace, or to FILE. Outside a session: `maic sessions export ID [FILE]` (stdout without FILE)."},
        {"stash", {"pop"}, "", "park the input draft; :pop brings it back",
         "*:stash* *:pop*\n`:stash` saves the input draft to ~/.local/state/maic/prompt-stash.jsonl and clears the input, so you can ask something else first; `:pop` restores the newest one. Survives restarts; `:q` with a draft stashes it automatically. Sent prompts are also kept in prompt-history.jsonl, so ↑ / Ctrl-P in the input reach earlier sessions' prompts."},
        {"rename", {"title"}, "TITLE", "title this session",
         "*:rename*\nSets the title `maic sessions` and `:export` show. With `title_model` in settings (for example `title_model = \"qwen3.5:4b\"`) a title is generated after the first turn; a remote title model is never used for a local session."},
        {"budget", {}, "[N|off]", "token budget for this session",
         "*:budget*\n`:budget` shows tokens used; `:budget 200000` stops the agent once input plus output over the session reaches that; `:budget off` removes it. `budget_tokens` in settings sets a default."},
        {"lua", {"luafile", "luajit", "repl", "chat"}, "[CODE]", "run Lua (LuaJIT) here, or enter Lua mode; :chat returns",
         "*:lua* *:luafile* *:chat* *maic lua*\n"
         "`:lua CODE` runs Lua in the workspace with LuaJIT (vendored, pinned to the revision Neovim uses); an expression shows its value, `=expr` forces that. `:luafile PATH` runs a file. `:lua` with nothing after it enters **Lua mode**: the input box becomes a REPL (prompt `lua❯`), every send runs in Lua, and `:chat` (or `:lua` again) returns to the model. Globals persist for the session. Output shows in the conversation and is handed to the model as context, like `!cmd`.\n\n"
         "Outside a session: `maic lua` is a REPL (`=expr`, multi-line continuation, Ctrl-D leaves), `maic lua FILE [args]` runs a file (`arg` holds the arguments), `maic lua -e CODE` a snippet.\n\n"
         "The `maic` table: `maic.workspace`, `maic.version`, `maic.read(path)`, `maic.write(path, text)`, `maic.shell(cmd)` (returns output and exit code), `maic.notice(text)`. The standard library is available: this runs as you, like your shell, and is never given to the model."},
        {"compact", {}, "[prune|head|all]", "free context: old tool results first, then the oldest turns",
         "*:compact*\n"
         "Frees context without losing the thread. `:compact` (and the automatic compaction at `compact_at`, 75% of the window by default) does it in this order:\n"
         "1. **prune**: every tool result except the most recent few (`compact_keep_results`, 4) is replaced by a one-line stub saying what it was; the dialog stays word for word. Tool output is what fills a context; dialog is cheap.\n"
         "2. **head**: only if pruning was not enough, the oldest half of the turns is summarised into a handover note (objective, details, work state, next move, files) and the rest stays verbatim. Each time this happens the compaction point moves forward, so the recent conversation is always intact.\n\n"
         "`:compact prune`, `:compact head` run one stage; `:compact all` is the traditional whole-conversation summary. The session file keeps everything that was said: compaction is recorded, never edited into the past. After any compaction, assistant turns replay as plain text (provider thinking blocks are dropped)."},
        {"clear", {}, "", "start a new conversation", "*:clear*\nForgets the conversation (the session file keeps everything). Waits until the agent is idle."},
        {"trip", {}, "[reason]", "trip the harness now", "*:trip*\nSets the tripwire immediately with no password; nothing runs until `:unlock`. See `:h harness`."},
        {"unlock", {}, "", "reset the harness (sudo password)", "*:unlock*\nResets the tripwire without leaving the session; asks for your sudo password every time."},
        {"!", {}, "cmd", "run cmd in your shell", "*:!*\nSee `:h !`."},
        {"q", {"quit", "exit"}, "", "quit (an unsent draft is stashed)",
         "*:q* *:quit*\nQuits. A running turn is interrupted. The session file is complete at every moment, and an unsent draft in the input is stashed (`:pop` in the next session brings it back), so nothing is lost. On exit the transcript path and its `maic -r` command are printed."},
        {"wq", {}, "", "send, then quit when the reply is in",
         "*:wq*\nSends the input like `:w` and quits once the reply has arrived, as vim's write-and-quit would. Ctrl-C while waiting keeps the session open. With an empty input it is `:q`."},
    };
    return c;
}

std::vector<const CommandInfo*> match_commands(const std::string& word) {
    std::string w = lower(word);
    std::vector<const CommandInfo*> exact, prefix;
    for (const auto& c : commands()) {
        bool is_exact = c.name == w, is_prefix = c.name.rfind(w, 0) == 0;
        for (const auto& a : c.aliases) {
            is_exact = is_exact || a == w;
            is_prefix = is_prefix || a.rfind(w, 0) == 0;
        }
        if (is_exact) exact.push_back(&c);
        else if (is_prefix) prefix.push_back(&c);
    }
    exact.insert(exact.end(), prefix.begin(), prefix.end());
    return exact;
}

std::vector<std::string> complete_argument(const std::string& command, const std::string& partial, const CompletionContext& ctx) {
    std::vector<std::string> candidates;
    std::string cmd = lower(command);
    if (cmd == "mode") candidates = {"manual", "auto-read", "edit", "auto", "plan"};
    else if (cmd == "set") candidates = {"markdown", "mouse", "tooldetails", "timestamps"};
    else if (cmd == "budget") candidates = {"off"};
    else if (cmd == "instructions") candidates = {"on", "off"};
    else if (cmd == "ban") candidates = {"add", "token", "remove", "tokens", "clear", "retries", "case", "list"};
    else if (cmd == "harness") candidates = {"smart", "dumb"};
    else if (cmd == "sampling") candidates = {"xtc", "temperature", "top_k", "top_p", "min_p", "seed", "repeat_penalty", "unset", "reset"};
    else if (cmd == "compact") candidates = {"prune", "head", "all"};
    else if (cmd == "think") candidates = {"on", "off"};
    else if (cmd == "w" || cmd == "write" || cmd == "send") candidates = {"now"};
    else if (cmd == "up" || cmd == "down") candidates = ctx.services;
    else if (cmd == "model") {
        candidates = ctx.models;
        for (const auto& p : ctx.providers) candidates.push_back(p + "/");
    } else if (cmd == "h" || cmd == "help") {
        for (const auto& c : commands()) candidates.push_back(c.name);
        for (const auto& t : topics()) candidates.push_back(t.name);
    }
    std::vector<std::string> out;
    std::string p = lower(partial);
    for (const auto& c : candidates) {
        if (lower(c).rfind(p, 0) == 0) out.push_back(c);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string help_text(const std::string& topic_in) {
    std::string topic = normalize(topic_in);
    while (!topic.empty() && topic.back() == ' ') topic.pop_back();
    if (topic.empty() || topic == "topics") {
        std::string out = "*help* Type `:h TOPIC` (or `maic help TOPIC`) for one of these; a unique prefix is enough:\n\n**commands**\n";
        for (const auto& c : commands()) out += "  :" + c.name + (c.args.empty() ? "" : " " + c.args) + "  " + c.summary + "\n";
        out += "\n**topics and keys**\n";
        for (const auto& t : topics()) out += "  " + t.name + "  " + t.summary + "\n";
        return out;
    }
    // Exact matches first, then prefixes, across commands and topics.
    std::vector<std::pair<std::string, const std::string*>> exact, prefix;
    auto consider = [&](const std::string& name, const std::vector<std::string>& aliases, const std::string& text, const std::string& shown) {
        std::vector<std::string> names = aliases;
        names.push_back(name);
        for (const auto& n : names) {
            std::string nn = normalize(n);
            if (nn == topic) {
                exact.push_back({shown, &text});
                return;
            }
        }
        for (const auto& n : names) {
            if (normalize(n).rfind(topic, 0) == 0) {
                prefix.push_back({shown, &text});
                return;
            }
        }
    };
    for (const auto& c : commands()) consider(c.name, c.aliases, c.help, ":" + c.name);
    for (const auto& t : topics()) consider(t.name, t.aliases, t.text, t.name);
    if (!exact.empty()) return *exact.back().second;  // a topic and a command of the same name: the topic's page
    if (prefix.size() == 1) return *prefix.front().second;
    // Several matches that are one name (a `:harness` command and a `harness` topic): the topic's page.
    bool one_name = true;
    auto bare = [](std::string n) { return n.rfind(":", 0) == 0 ? n.substr(1) : n; };
    for (const auto& [name, text] : prefix) one_name = one_name && bare(name) == bare(prefix.front().first);
    if (!prefix.empty() && one_name) return *prefix.back().second;
    if (prefix.empty()) return "no help for '" + topic_in + "'. `:h` lists the topics.";
    std::string out = "'" + topic_in + "' matches several topics:\n";
    for (const auto& [name, text] : prefix) out += "  " + name + "\n";
    return out;
}

}  // namespace maic
