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
        {"harness", {"tripwire", "sandbox", "trip", "lock"}, "what protects the machine",
         "*harness*\n"
         "Every tool call is checked before it runs: tripwire, then policy for the mode, then approval, then the sandbox.\n\n"
         "- **tripwire**: a root-owned lock. `:trip REASON` (or the [t] answer at an approval, or a dangerous command) sets it instantly with no password. While tripped nothing runs, but the session survives. `:unlock` resets it and asks for your sudo password.\n"
         "- **sandbox**: every model-run command executes in bubblewrap: only the workspace writable, secrets hidden, no network, no sudo, a timeout.\n"
         "- **approval**: y / n / a (always this file or program, this session) / t (trip).\n\n"
         "Details and the planned layers: docs/harness.md."},
        {"sessions", {"session", "resume", "transcript", "transcripts"}, "transcripts, -c, -r, forking",
         "*sessions*\n"
         "Every session is a JSONL file under ~/.local/state/maic/sessions (0600), in a home: `general/` by default, `projects/<encoded workspace>/` when settings say `\"sessions_home\": \"project\"`, or any name. `:session` shows this one and its home; `maic sessions` lists them with where each was started and last opened; `maic sessions rehome ID project|general|NAME` moves one.\n\n"
         "- `maic -c` continues the newest session from the current directory; `maic -r` picks from a list; `maic -r ID` (a unique prefix is enough).\n"
         "- Interactive resumes append to the same file. `--no-append` writes a new file that only points at the old one and the number of records loaded, which is also how a session forks. `maic -p` records nothing unless `--record` (a fork when resuming) or `--append`.\n"
         "- A session that ended mid tool call resumes from the last complete step. The model is told it resumed, with the current mode and instructions."},
        {"headless", {"-p", "print", "cli", "command-line", "context", "-C", "--context", "interactive", "-i"}, "maic -p, stdin, --context files, --interactive",
         "*headless* *-p* *--context* *--interactive*\n"
         "`maic -p \"prompt\"` runs one turn without the UI: the reply streams to stdout, tool activity to stderr. `maic -p -` takes the prompt from stdin (`cat dialog.txt | maic -p -`). `--json` prints events as JSON lines. Approvals are asked on the terminal when there is one, otherwise denied; `--mode auto-read` is the usual choice for scripts.\n\n"
         "**Context.** `--context FILE` (`-C`) attaches a text file to the conversation before the prompt, labelled with its path; repeat it for several; `-C -` reads stdin (then the prompt must be an argument). Combine with `-c` / `-r` to put a file in front of an old conversation. Binary files are refused. The same flag works for the interactive `maic`.\n\n"
         "**Transcripts.** A headless run keeps no transcript unless `--record` (with `-c`/`-r`: a new file that points at the old one, a fork) or `--append` (writes into the old file).\n\n"
         "**--interactive** (`-i`) with `-p` opens an interactive session that starts with the prompt already sent. It follows interactive rules whatever the order of the flags: a transcript is always kept, `-c`/`-r` continue in the same file unless `--no-append`; `--record` is redundant and `--json` is ignored.\n\n"
         "`maic help TOPIC` prints these pages outside a session."},
        {"queue", {"queued", "mid-turn", "interrupt"}, "sending while the agent works",
         "*queue*\n"
         "Sending while the agent is busy queues the message; it reaches the model at its next step in the current turn. `:w now` delivers it immediately: the current output is abandoned and the model is asked again with your message included. Messages still queued when a turn ends start the next turn. Ctrl-C interrupts the turn instead."},
        {"providers", {"provider", "models", "remote", "ollama", "anthropic", "deepseek", "openrouter"}, "local and remote models",
         "*providers*\n"
         "Models are `provider/model`: `qwen3.5:9b` (Ollama, local), `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, `openrouter/...`, or any OpenAI-compatible server added in settings. `:model` alone lists providers; `:models` lists what Ollama has.\n\n"
         "A remote provider receives your prompts, every file the agent reads and every command's output; MAIC says so when you switch and shows REMOTE in the status strip. Keys come from an environment variable or a command, never from the settings file. See docs/settings.md."},
        {"settings", {"config", "styles", "style", "settings.json"}, "the settings file",
         "*settings*\n"
         "Layered: ~/.config/maic/settings.json, then `.maic/settings.json` and `.maic/settings.local.json` in each directory from under $HOME down to the workspace (nearest wins; settings.json is for the project, settings.local.json is personal). Keys: model, mode, think, markdown, mouse, sessions_home (auto/general/project/name), instruction_files, providers, style. `maic settings init` writes the global one, `:init` scaffolds a project's, `:settings` shows what is in effect. See docs/settings.md."},
        {"instructions", {"maic.md", "agents.md", "claude.md"}, "standing instructions the model always sees",
         "*instructions*\n"
         "~/.config/maic/MAIC.md, then every MAIC.md or AGENTS.md from under $HOME down to the workspace, re-read at the start of every turn (32 KB each). `:instructions` shows what is in effect."},
        {"keys", {"keybindings", "bindings", "motions", "vim"}, "the key map",
         "*keys*\n"
         "The input is a small vim and starts in normal mode.\n\n"
         "- **insert**: `i a I A o O` enter it; Enter = new line; Esc = normal; Ctrl-W / Ctrl-U delete word / line; Ctrl-Y pastes the register; ↑ ↓ or Ctrl-P / Ctrl-N prompt history.\n"
         "- **normal**: `h j k l w b e 0 ^ $` move (Enter = down a line); `x X D C S`; `d c y` + motion, `dd cc yy`; `v V`; `p P`; `u` undo, Ctrl-R redo; counts (`3w`); `:` commands; `/` searches the conversation.\n"
         "- **send**: Alt+Enter or `:w` from any mode. `:e` or Ctrl-X Ctrl-E edits the input in nvim.\n"
         "- **conversation window**: Ctrl-W k enters it, Ctrl-W j (Esc, i, Enter) returns; motions, `v V`, `y` yanks to the clipboard, `yy`, `/ n N`.\n"
         "- **anywhere**: Shift-Tab cycles modes; Ctrl-C interrupts, then clears, then quits; the scroll wheel scrolls.\n\n"
         "`:h KEY` works for single keys too: `:h u`, `:h Ctrl-W`, `:h Alt+Enter`."},
        {"conversation", {"window", "ctrl-w", "focus", "yank", "clipboard", "search", "/"}, "the conversation window as a vim buffer",
         "*conversation* *Ctrl-W*\n"
         "Ctrl-W k moves the cursor into the conversation window; Ctrl-W j, Esc, `i` or Enter bring it back (Ctrl-W in insert mode deletes a word, so press Esc first unless the input is empty). Inside: `j k h l w b e 0 $ gg G`, Ctrl-D/U/F/B; `v` / `V` select, `o` swaps the ends; `y` yanks the selection to the register and the system clipboard (wl-copy, xclip, and the terminal through OSC 52); `yy` a line; `/pattern` then `n` / `N` search, smart case. Ctrl-Shift-C in your terminal still copies mouse selections; with the scroll wheel on, select with Shift+drag."},
        {"alt-enter", {"send"}, "sends the input", "*Alt+Enter*\nSends the input from any mode; the same as `:w`. Enter is a new line. nvim has no default Alt mappings, so nothing is lost."},
        {"enter", {}, "a new line", "*Enter*\nInsert mode: a new line. Normal mode: down a line. To send, use Alt+Enter or `:w`."},
        {"escape", {}, "back to normal mode", "*Esc*\nInsert or visual mode to normal mode; in the command line, cancels; in the conversation window, back to the input."},
        {"u", {"undo"}, "undo", "*u* *Ctrl-R*\n`u` undoes the last change, `Ctrl-R` redoes. Two hundred levels. An insert session counts as one step, and so does a change operator (`cw`, `cc`, `C`, `S`) together with what you typed after it."},
        {"ctrl-r", {"redo"}, "redo", "*Ctrl-R*\nRedo. See `:h u`."},
        {"ctrl-c", {}, "interrupt, clear, quit", "*Ctrl-C*\nWhile the agent works: interrupts the turn. While a `!command` runs: stops it. Otherwise: clears the input; pressed twice on an empty input: quits (or `:q`)."},
        {"shift-tab", {"tab"}, "cycle the mode", "*Shift-Tab*\nCycles manual → auto-read → edit → auto → plan. See `:h modes`."},
        {"ctrl-x", {"ctrl-x ctrl-e", "nvim", "editor"}, "edit the input in nvim", "*Ctrl-X Ctrl-E*\nOpens the input in $VISUAL, $EDITOR or nvim as a markdown file and loads it back when you quit. Same as `:e`."},
        {"v", {"visual", "visual-mode"}, "visual selection", "*v* *V*\n`v` selects by character, `V` by line, in the input or the conversation window. Then `y` yanks, `d` deletes (input only), `c` changes, `o` swaps the ends, Esc leaves."},
        {"i", {"insert", "a", "o"}, "insert mode", "*i* *a* *I* *A* *o* *O*\n`i` inserts before the cursor, `a` after, `I` at the line start, `A` at the line end, `o` opens a line below, `O` above. Esc returns to normal mode."},
        {"p", {"paste", "register", "reg"}, "paste the register or the clipboard", "*p* *P* *Ctrl-Y* *\"+p*\n`p` pastes the register after the cursor, `P` before; in insert mode Ctrl-Y pastes it. `\"+p` (or `\"*p`, or Space then `p`) pastes the system clipboard (wl-paste, xclip or xsel). The register holds the last yank or delete from the input or the conversation window. `:reg` shows it."},
        {"y", {"yank", "leader", "space", "text-objects", "iw", "aw"}, "yank: motions, text objects, the clipboard", "*y* *Y* *\"+y* *<leader>y* *text-objects*\n"
         "`y` + motion yanks (`yw`, `y$`, `yy`, `Y`); `d` and `c` take the same motions. Text objects work after `d`, `c`, `y` and in visual mode: `iw` `aw` (word), `iW` `aW`, `i\"` `a\"`, `i'` `a'`, `` i` ``, `i(` `a(` (also `ib`), `i[` `a[`, `i{` `a{` (also `iB`), `i<` `a<`. So `ciw`, `di\"`, `ya(`, `viw`.\n"
         "The system clipboard: `\"+y` (or `\"*y`) before any yank, or the leader (Space by default, `leader` in settings) then `y`: `<leader>y` yanks the line in normal mode or the selection in visual mode. In the conversation window every yank already reaches the clipboard, and `yiw`, `yw`, `y$`, `Y`, `yy` work there too."},
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
        {"h", {"help"}, "[topic]", "this help, or :h TOPIC",
         "*:h* *:help*\n`:h` alone lists every topic. `:h TOPIC` shows one: a command (`:h w`), a key (`:h u`, `:h Ctrl-W`, `:h Alt+Enter`) or a concept (`:h modes`, `:h harness`, `:h sessions`). A unique prefix is enough; several matches give a list."},
        {"mode", {}, "NAME", "set the agent mode",
         "*:mode*\n`:mode manual|auto-read|edit|auto|plan`. Shift-Tab cycles them. See `:h modes`."},
        {"model", {}, "[NAME]", "switch model, or list providers",
         "*:model*\n`:model NAME` switches (when the agent is idle): `qwen3.5:9b`, `anthropic/claude-opus-5-5`, `deepseek/deepseek-chat`, ... `:model` alone lists the providers. Switching to a remote provider prints what will leave this machine. See `:h providers`."},
        {"models", {}, "", "models the Ollama server has", "*:models*\nLists the models on the current Ollama provider (`ollama list`)."},
        {"think", {}, "on|off", "let the model reason first", "*:think*\n`:think on` asks the model to reason before answering: slower, better on hard problems. Anthropic models then use the provider's `think_effort`."},
        {"set", {}, "markdown|mouse on|off", "rendering and mouse toggles",
         "*:set*\n`:set markdown off` shows the conversation as raw text; `on` renders it. `:set mouse off` stops the scroll wheel and gives the terminal its normal mouse selection back. `:set tooldetails on` shows tool output in full instead of an 8-line preview (in the conversation window `za` folds or unfolds one result, `zR` unfolds all, `zM` folds all). markdown and mouse persist through settings.json."},
        {"status", {}, "", "harness, services, model, session",
         "*:status*\nThe harness state, every service with where it runs (host process, pid, url) and a quick action, the model and whether it is remote, this session's file, the mode, and queued messages."},
        {"up", {}, "SERVICE", "start a service", "*:up*\n`:up ollama` starts a service MAIC manages (ollama, comfyui). Refused while the harness is tripped."},
        {"down", {}, "SERVICE", "stop a service MAIC started", "*:down*\n`:down ollama` stops it. MAIC only stops what it started."},
        {"init", {}, "", "scaffold MAIC.md and .maic/settings.json, then draft the MAIC.md",
         "*:init*\nCreates `.maic/settings.json` and a `MAIC.md` placeholder in the workspace, then asks the agent to look over the project and write the MAIC.md (it will ask before writing in manual mode). A project with a MAIC.md keeps its transcripts under sessions/projects/. `maic init` does the scaffolding only."},
        {"settings", {}, "", "which settings files are in effect",
         "*:settings*\nLists the settings files that were read, nearest last: the global file, then `.maic/settings.json` and `.maic/settings.local.json` from just under $HOME down to the workspace. Shows where this session's transcript home resolved to. See `:h settings`."},
        {"instructions", {}, "", "the MAIC.md / AGENTS.md files in effect", "*:instructions*\nLists the instruction files the model sees, re-read every turn. See `:h instructions`."},
        {"session", {}, "", "where this transcript is", "*:session*\nThis session's file and the sessions directory. See `:h sessions`."},
        {"artifacts", {}, "", "where everything is kept, with sizes", "*:artifacts*\nEvery place MAIC and its services leave things (transcripts, service logs, ComfyUI outputs, ...) with sizes. Clean with `maic artifacts clean OWNER/NAME [--older-than DAYS]`."},
        {"reg", {"register"}, "", "show the yank register", "*:reg*\nShows the register. See `:h p`."},
        {"clear", {}, "", "start a new conversation", "*:clear*\nForgets the conversation (the session file keeps everything). Waits until the agent is idle."},
        {"trip", {}, "[reason]", "trip the harness now", "*:trip*\nSets the tripwire immediately with no password; nothing runs until `:unlock`. See `:h harness`."},
        {"unlock", {}, "", "reset the harness (sudo password)", "*:unlock*\nResets the tripwire without leaving the session; asks for your sudo password every time."},
        {"!", {}, "cmd", "run cmd in your shell", "*:!*\nSee `:h !`."},
        {"q", {"quit", "exit", "wq"}, "", "quit", "*:q* *:quit*\nQuits. A running turn is interrupted. The session file is complete at every moment, so nothing is lost."},
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
    else if (cmd == "set") candidates = {"markdown", "mouse", "tooldetails"};
    else if (cmd == "think") candidates = {"on", "off"};
    else if (cmd == "w" || cmd == "write" || cmd == "send") candidates = {"now"};
    else if (cmd == "up" || cmd == "down") candidates = ctx.services;
    else if (cmd == "model") {
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
    if (topic.empty()) {
        std::string out = "*help* Type `:h TOPIC` for one of these (a unique prefix is enough):\n\n**commands**\n";
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
    if (!exact.empty()) return *exact.front().second;
    if (prefix.size() == 1) return *prefix.front().second;
    if (prefix.empty()) return "no help for '" + topic_in + "'. `:h` lists the topics.";
    std::string out = "'" + topic_in + "' matches several topics:\n";
    for (const auto& [name, text] : prefix) out += "  " + name + "\n";
    return out;
}

}  // namespace maic
