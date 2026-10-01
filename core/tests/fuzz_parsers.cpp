// A seeded mutation fuzzer, no libFuzzer: corpora of SSE lines (OpenAI and Anthropic shapes), session JSONL
// records, markdown snippets and ban-filter inputs are mutated at random and fed to the real parsers for
// FUZZ_SECONDS (default 2). Nothing may crash; the ban filter and the markdown renderer must also keep their
// invariants. The SSE bodies go through a local httplib server so the whole path runs: LineSplitter, the
// event parsers, usage, tool-call assembly. FUZZ_SEED repeats a run; the seed is printed either way.
#include "check.hpp"

#include "maic/bans.hpp"
#include "maic/import.hpp"
#include "maic/llm.hpp"
#include "maic/markdown.hpp"
#include "maic/session.hpp"

#include "maic/http.hpp"

#include <regex.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace maic;
namespace fs = std::filesystem;

namespace {

std::mt19937 rng;

// Mutated inputs are rarely valid UTF-8: a message shows them as JSON with the bad bytes replaced.
std::string show(const std::string& s) {
    return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Thousands of checks per second: only a failure is worth a line.
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::cout << "  FAIL  " << what << "\n";
    ++failures();
}

size_t pick(size_t n) {
    return std::uniform_int_distribution<size_t>(0, n - 1)(rng);
}

template <typename T>
const T& one_of(const std::vector<T>& v) {
    return v[pick(v.size())];
}

// A few random edits: bytes flipped, dropped, duplicated, inserted, a slice from another corpus entry spliced in.
std::string mutate(std::string s, const std::vector<std::string>& corpus) {
    int edits = 1 + static_cast<int>(pick(4));
    for (int e = 0; e < edits && !s.empty(); ++e) {
        size_t at = pick(s.size());
        switch (pick(6)) {
            case 0: s[at] = static_cast<char>(pick(256)); break;
            case 1: s.erase(at, 1 + pick(4)); break;
            case 2: s.insert(at, s.substr(at, 1 + pick(8))); break;
            case 3: s.insert(at, 1, static_cast<char>(pick(256))); break;
            case 4: s.insert(at, one_of(std::vector<std::string>{"\"", "{", "}", "[", "]", ",", ":", "\n", "\r\n", "data: ", "\\u00", "\xff", "\xc3", "**", "`", "](", "#", "> "})); break;
            default: {
                const std::string& other = one_of(corpus);
                if (!other.empty()) {
                    size_t from = pick(other.size());
                    s.insert(at, other.substr(from, 1 + pick(32)));
                }
            }
        }
    }
    return s;
}

const std::vector<std::string> openai_lines = {
    R"(data: {"id":"c1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant","content":""}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"content":"Hello, "}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"content":"world"}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"reasoning_content":"thinking about it"}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"reasoning":"more thinking"}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"read_file","arguments":""}}]}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\": \"a.txt\""}}]}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"}"}}]}}]})",
    R"(data: {"choices":[{"index":1,"delta":{"tool_calls":[{"index":1,"id":"call_2","function":{"name":"run_shell","arguments":"{\"command\":\"ls\"}"}}]}}]})",
    R"(data: {"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
    R"(data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":120,"completion_tokens":30}})",
    R"(data: {"choices":[{"index":0,"delta":{},"finish_reason":"length"}]})",
    R"(data: {"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":2,"total_tokens":7}})",
    R"(data: {"error":{"message":"request exceeds the available context size","type":"invalid_request_error","code":400}})",
    R"(data: {"error":"plain string error"})",
    R"(data: {"choices":[{"index":0,"delta":{"content":null}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"content":123}}]})",
    R"(data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":"x","function":"notanobject"}]}}]})",
    R"(data: {"choices":"notanarray"})",
    R"(data: [DONE])",
    R"(: keepalive comment)",
    R"(event: message)",
    R"(data:)",
    R"()",
};

const std::vector<std::string> anthropic_lines = {
    R"(event: message_start)",
    R"(data: {"type":"message_start","message":{"id":"m1","model":"claude","usage":{"input_tokens":25,"cache_read_input_tokens":10,"cache_creation_input_tokens":0}}})",
    R"(data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
    R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})",
    R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" there"}})",
    R"(data: {"type":"content_block_stop","index":0})",
    R"(data: {"type":"content_block_start","index":1,"content_block":{"type":"thinking","thinking":""}})",
    R"(data: {"type":"content_block_delta","index":1,"delta":{"type":"thinking_delta","thinking":"hmm"}})",
    R"(data: {"type":"content_block_delta","index":1,"delta":{"type":"signature_delta","signature":"abc=="}})",
    R"(data: {"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"read_file","input":{}}})",
    R"(data: {"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})",
    R"(data: {"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":" \"a.txt\"}"}})",
    R"(data: {"type":"content_block_stop","index":2})",
    R"(data: {"type":"message_delta","delta":{"stop_reason":"tool_use","stop_sequence":null},"usage":{"output_tokens":40}})",
    R"(data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":12}})",
    R"(data: {"type":"message_delta","delta":{"stop_reason":"max_tokens"}})",
    R"(data: {"type":"message_delta","delta":{"stop_reason":"refusal"}})",
    R"(data: {"type":"message_stop"})",
    R"(data: {"type":"ping"})",
    R"(data: {"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})",
    R"(data: {"type":"content_block_delta","index":"zero","delta":{"type":"text_delta","text":5}})",
    R"(data: {"type":"content_block_start","index":-1,"content_block":"text"})",
    R"(data: {"type":"message_start","message":"nope"})",
    R"(data: not json at all)",
};

// Session records, as session.cpp writes them, plus a Claude Code transcript line for the importer.
const std::vector<std::string> jsonl_lines = {
    R"({"type":"start","time":"2026-10-01T04:00:00+0000","workspace":"/tmp/ws","host":"box","pid":123,"model":"llamacpp/current","mode":"manual"})",
    R"({"type":"start","parent":"20261001-040000-tui-1","profile":"scout"})",
    R"({"type":"start","parent":"20261001-040000-tui-1","agent":"explore","model_reason":"fable-5.1 is limited"})",
    R"({"type":"user","text":"hello there","provider":"llamacpp","model":"llamacpp/current","remote":false,"mode":"auto","origin":"local"})",
    R"({"type":"assistant","text":"hi"})",
    R"({"type":"msg","role":"system","content":"sys"})",
    R"({"type":"msg","role":"user","content":"turn","images":[{"name":"a.png","media":"image/png","data":"iVBOR"}]})",
    R"({"type":"msg","role":"assistant","content":"","tool_calls":[{"id":"c1","name":"read_file","arguments":{"path":"f"}}]})",
    R"({"type":"msg","role":"tool","content":"result","tool_name":"read_file","tool_call_id":"c1","is_error":false})",
    R"({"type":"msg","role":"assistant","content":"done","raw_kind":"anthropic","raw":[{"type":"text","text":"done"}]})",
    R"({"type":"msg","role":"assistant","raw_kind":"openai","raw":{"reasoning_content":"r"}})",
    R"({"type":"tool","tool":"run_shell","arguments":{"command":"ls"},"result":"a\nb","ok":true})",
    R"({"type":"usage","input":100,"output":20,"context":16384})",
    R"({"type":"compact","stage":"prune","summary":"pruned 3"})",
    R"({"type":"reset"})",
    R"({"type":"clear"})",
    R"({"type":"title","text":"a title"})",
    R"({"type":"context","text":"attached ctx.txt"})",
    R"({"type":"resumed_from","id":"20261001-035959-tui-7","path":"/nonexistent/20261001-035959-tui-7.jsonl","records":12})",
    R"({"type":"imported_from","source":"x.json","format":"claude-code"})",
    R"({"type":"msg","role":12,"content":{"a":1}})",
    R"({"type":"msg","role":"assistant","tool_calls":"nope"})",
    R"({"type":"usage","input":"many"})",
    R"({"type":"resumed_from","records":"x"})",
    R"({"type":"user","text":42})",
    R"([1,2,3])",
    R"("just a string")",
    R"({"type":"user","message":{"role":"user","content":"claude code style"},"uuid":"u1","parentUuid":null,"cwd":"/tmp","sessionId":"s1","timestamp":"2026-10-01T00:00:00Z"})",
    R"({"type":"assistant","message":{"role":"assistant","model":"claude","content":[{"type":"text","text":"hi"},{"type":"tool_use","id":"t1","name":"Read","input":{"file_path":"/tmp/a"}}]},"uuid":"u2","parentUuid":"u1"})",
    R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"file text"}]},"uuid":"u3","parentUuid":"u2"})",
    R"({"type":"summary","summary":"a summary","leafUuid":"u3"})",
};

const std::vector<std::string> markdown_snippets = {
    "# Heading\n\nSome **bold** and *italic* and `code` and a [link](http://x.y/z).\n",
    "## Sub\n- item one\n- item **two**\n  - nested\n1. first\n2) second\n",
    "```cpp\nint main() { return 0; }\n```\n",
    "~~~\nfenced with tildes\n~~~",
    "> a quote with `code`\n>another\n>\n",
    "---\n***\n___\n-- not a rule\n",
    "unclosed **bold and `code\n__under__ _it_ *x*",
    "plain text with a URL http://example.com/path?q=1 and ends",
    "émoji 😀 and wide 漢字 characters in a line that is long enough to wrap around the width limit set",
    "a\n\n\n\nb\n",
    "#no space heading\n####### seven\n# \n",
    "   indented   text   with   spaces   \n\t tab",
    "",
    "\n",
    "**",
    "`",
    "[]()",
    "- \n* \n+ \n",
    "10. ten\n0. zero\n1.no\n",
};

// The ban vocabulary: words the inputs are built from, bans drawn from them, patterns over them. The
// replacement uses characters outside the vocabulary so it can never complete a ban itself.
const std::vector<std::string> words = {"pelican", "heron", "the", "river", "wide", "pel", "ican", "PELICAN", "Heron", "x", " ", "\n", "pelipelican", "hero"};
const std::vector<std::string> patterns = {"pel+ican", "he(r|ar)on", "ri[vw]er", "p.*n", "(the )+", "[[:space:]]wide", "wi?de$", "^the", "(", "[a-", "*bad", "a{3,2}", ""};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& text, const std::string& ban, bool icase) {
    return icase ? lower(text).find(lower(ban)) != std::string::npos : text.find(ban) != std::string::npos;
}

std::string random_body(const std::vector<std::string>& lines);

// An httplib server answering every POST with a fresh random SSE body for the path's provider.
struct ReplayServer {
    httplib::Server srv;
    std::thread thread;
    std::mutex mu;
    int port = 0;
    ReplayServer() {
        port = srv.bind_to_any_port("127.0.0.1");
        auto serve = [this](const httplib::Request& req, httplib::Response& res) {
            std::string body;
            {
                std::lock_guard lock(mu);  // the one rng
                body = random_body(req.path == "/v1/messages" ? anthropic_lines : openai_lines);
            }
            res.set_content(body, "text/event-stream");
        };
        srv.Post("/v1/chat/completions", serve);
        srv.Post("/v1/messages", serve);
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~ReplayServer() {
        srv.stop();
        thread.join();
    }
};

std::string random_body(const std::vector<std::string>& lines) {
    std::string body;
    int n = 1 + static_cast<int>(pick(12));
    for (int i = 0; i < n; ++i) {
        std::string line = one_of(lines);
        if (pick(3) == 0) line = mutate(line, lines);
        body += line + (pick(8) == 0 ? "\n" : "\n\n");
    }
    if (pick(4) == 0) body = mutate(body, lines);
    return body;
}

// One chat call against the replay server; the body is drawn there. Every call ends with up to 100 ms of
// watcher wake-up in stream_post, so the target runs eight of these at a time.
int fuzz_sse(ReplayServer& server, bool anthropic, double seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
    Provider p{"fuzz", anthropic ? "anthropic" : "openai", "http://127.0.0.1:" + std::to_string(server.port) + (anthropic ? "" : "/v1"), "", "", {{"context_window", 4096}}};
    setenv("FUZZ_KEY", "k", 1);
    p.api_key_env = "FUZZ_KEY";
    std::vector<Message> with_call = {{"system", "sys"}, {"user", "hi"}, {"assistant", "", {{"c1", "read_file", {{"path", "f"}}}}}, {"tool", "r", {}, "read_file", "c1"}};
    std::vector<Message> plain = {{"system", "sys"}, {"user", "hi"}};
    std::atomic<int> iterations{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 8; ++w) {
        workers.emplace_back([&, w] {
            while (std::chrono::steady_clock::now() < end) {
                ChatOptions options{"m", w % 2 == 0};
                options.retries = 0;
                std::atomic<bool> cancel{false};
                size_t streamed = 0;
                try {
                    Message reply = chat(p, options, w % 3 == 0 ? with_call : plain, nlohmann::json::array(), [&](std::string_view d, bool) { streamed += d.size(); }, cancel);
                    (void)reply.content.size();
                    for (const auto& c : reply.tool_calls) (void)c.arguments.dump();
                } catch (const std::exception&) {
                    // a malformed stream may be an error; it may not be a crash
                }
                ++iterations;
            }
        });
    }
    for (auto& t : workers) t.join();
    return iterations;
}

int fuzz_jsonl(const fs::path& dir) {
    fs::path file = dir / "fuzz.jsonl";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        int n = 1 + static_cast<int>(pick(10));
        for (int i = 0; i < n; ++i) {
            std::string line = one_of(jsonl_lines);
            if (pick(2) == 0) line = mutate(line, jsonl_lines);
            out << line << "\n";
        }
    }
    try {
        LoadedSession s = load_session(file);
        (void)s.messages.size();
    } catch (const std::exception&) {
    }
    try {
        auto info = find_session(file.string());  // a path: the listing fields are read from the file itself
        if (info) (void)info->first_prompt.size();
    } catch (const std::exception&) {
    }
    try {
        ImportedSession imp = read_import(file, "claude-code");
        (void)imp.records.size();
    } catch (const std::exception&) {
    }
    return 1;
}

int fuzz_markdown() {
    std::string text = one_of(markdown_snippets);
    if (pick(3) != 0) text = mutate(text, markdown_snippets);
    auto lines = markdown_lines(text);
    std::string back;
    for (size_t i = 0; i < lines.size(); ++i) back += (i ? "\n" : "") + line_text(lines[i]);
    check(back == text, "markdown keeps every byte: " + show(text));
    check(!lines.empty(), "markdown_lines never returns no lines");
    auto plain = plain_lines(text);
    check(!plain.empty(), "plain_lines never returns no lines");
    size_t width = pick(200);
    for (const auto& l : lines) {
        auto rows = wrap_line(l, width);
        check(!rows.empty(), "wrap_line never returns no lines");
        std::string joined, original = line_text(l);
        for (const auto& r : rows) {
            check(line_width(r) <= std::max<size_t>(width, 8), "a wrapped row fits the width");
            joined += line_text(r);
        }
        // Breaking at a space drops that space; nothing else may go missing.
        std::erase(joined, ' ');
        std::erase(original, ' ');
        check(joined == original, "wrapping drops nothing but break spaces: " + show(line_text(l)));
    }
    return 1;
}

int fuzz_bans() {
    Bans bans;
    int ns = static_cast<int>(pick(4));
    for (int i = 0; i < ns; ++i) bans.strings.push_back(one_of(words));
    int np = static_cast<int>(pick(3));
    for (int i = 0; i < np; ++i) bans.patterns.push_back(pick(4) == 0 ? mutate(one_of(patterns), patterns) : one_of(patterns));
    if (pick(3) == 0) bans.tokens.push_back(nlohmann::json(one_of(words)));
    bans.ignore_case = pick(2) == 0;
    bans.window = 8 + static_cast<int>(pick(40));
    bans.replacement = "[*]";
    bool replace = pick(2) == 0;
    std::string input;
    int nw = 1 + static_cast<int>(pick(30));
    for (int i = 0; i < nw; ++i) input += one_of(words);
    if (pick(4) == 0) input = mutate(input, words);

    std::string setup = " [strings " + nlohmann::json(bans.strings).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + " patterns " + nlohmann::json(bans.patterns).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + (bans.ignore_case ? " icase" : "") +
                        " window " + std::to_string(bans.window) + (replace ? " replace" : " cut") + " input " + show(input) + "]";
    BanFilter filter(bans, replace);
    std::string out;
    size_t at = 0;
    while (at < input.size()) {
        size_t n = 1 + pick(std::min<size_t>(12, input.size() - at));
        out += filter.feed(std::string_view(input).substr(at, n));
        at += n;
    }
    out += filter.flush();
    check(filter.clean() == out, "clean() is exactly what feed and flush returned");
    std::vector<std::string> literal = bans.strings;
    for (const auto& t : bans.tokens) literal.push_back(t.get<std::string>());
    for (const auto& ban : literal) {
        check(!contains(out, ban, bans.ignore_case), "the shown text never contains the banned string " + show(ban) + " in " + show(out));
    }
    if (!replace) {
        check(input.compare(0, out.size(), out) == 0, "in cut mode the shown text is a prefix of the reply");
        if (filter.triggered()) {
            // The cut lands where the hit starts, unless a regex match began in text already shown: then it is
            // longer than the hold-back window by construction.
            bool at_hit = contains(input.substr(out.size(), filter.hit().size()), filter.hit(), bans.ignore_case) && input.substr(out.size()).size() >= filter.hit().size();
            check(at_hit || filter.hit().size() > static_cast<size_t>(bans.window), "a cut starts at the hit, or the hit outgrew the window: hit " + show(filter.hit()) + " shown " + show(out) + setup);
            bool literal_hit = false;
            for (const auto& ban : literal) literal_hit = literal_hit || (bans.ignore_case ? lower(ban) == lower(filter.hit()) : ban == filter.hit());
            if (!literal_hit && at_hit) {
                // A regex hit must be a real match of the whole reply at that spot: ^ and $ are the reply's edges.
                bool real = false;
                for (const auto& p : bans.patterns) {
                    regex_t re;
                    if (regcomp(&re, p.c_str(), REG_EXTENDED | (bans.ignore_case ? REG_ICASE : 0)) != 0) continue;
                    regmatch_t m[1];
                    m[0].rm_so = static_cast<regoff_t>(out.size());
                    m[0].rm_eo = static_cast<regoff_t>(input.size());
                    real = real || (regexec(&re, input.c_str(), 1, m, REG_STARTEND) == 0 && static_cast<size_t>(m[0].rm_so) == out.size());
                    regfree(&re);
                }
                check(real, "a regex cut sits on a match of the whole reply: hit " + show(filter.hit()) + " shown " + show(out) + setup);
            }
        } else {
            check(out == input, "with no hit the whole reply is shown");
        }
    }
    // Regex bans: no match of a size the window could hold survives in text shown verbatim. A cut output is a
    // prefix of the reply, so its end is not the reply's end: $ may not match there. In replace mode the
    // replacement tokens are not verbatim text: a match touching one is not the filter's miss.
    int not_eol = out == input ? 0 : REG_NOTEOL;
    std::vector<bool> replaced(out.size(), false);
    if (replace) {
        for (size_t at = out.find(bans.replacement); at != std::string::npos; at = out.find(bans.replacement, at + bans.replacement.size())) {
            std::fill(replaced.begin() + static_cast<long>(at), replaced.begin() + static_cast<long>(at + bans.replacement.size()), true);
        }
    }
    for (const auto& p : bans.patterns) {
        regex_t re;
        if (regcomp(&re, p.c_str(), REG_EXTENDED | (bans.ignore_case ? REG_ICASE : 0)) != 0) continue;
        regmatch_t m[1];
        m[0].rm_so = 0;
        m[0].rm_eo = 0;
        if (regexec(&re, "", 1, m, REG_STARTEND) == 0) {
            regfree(&re);  // matches the empty string: the filter's leftmost match is always empty, so it bans nothing
            continue;
        }
        size_t from = 0;
        while (from < out.size()) {
            m[0].rm_so = static_cast<regoff_t>(from);
            m[0].rm_eo = static_cast<regoff_t>(out.size());
            if (regexec(&re, out.c_str(), 1, m, REG_STARTEND | not_eol) != 0) break;
            size_t s = static_cast<size_t>(m[0].rm_so), e = static_cast<size_t>(m[0].rm_eo);
            if (e == s) {
                ++from;
                continue;
            }
            bool over_replacement = std::any_of(replaced.begin() + static_cast<long>(s), replaced.begin() + static_cast<long>(e), [](bool r) { return r; });
            check(e - s > static_cast<size_t>(bans.window) || over_replacement,
                   "a regex match shown verbatim is longer than the window: /" + p + "/ matched " + show(out.substr(s, e - s)) + " in " + show(out) + setup);
            from = e;
        }
        regfree(&re);
    }
    return 1;
}

}  // namespace

int main() {
    unsigned seed = std::getenv("FUZZ_SEED") ? static_cast<unsigned>(std::strtoul(std::getenv("FUZZ_SEED"), nullptr, 10)) : static_cast<unsigned>(std::random_device{}());
    double seconds = std::getenv("FUZZ_SECONDS") ? std::atof(std::getenv("FUZZ_SECONDS")) : 2.0;
    rng.seed(seed);
    std::cout << "seed " << seed << " (FUZZ_SEED=" << seed << " repeats this run), " << seconds << " s per target (FUZZ_SECONDS)" << std::endl;
    fs::path dir = fs::temp_directory_path() / ("maic-fuzz-" + std::to_string(getpid()));
    fs::create_directories(dir);
    setenv("XDG_STATE_HOME", (dir / "state").c_str(), 1);  // find_session during a resumed_from lookup stays out of the real state

    ReplayServer server;
    struct Target {
        const char* name;
        std::function<int()> run;
    };
    std::vector<Target> targets = {
        {"openai SSE", [&] { return fuzz_sse(server, false, seconds); }},
        {"anthropic SSE", [&] { return fuzz_sse(server, true, seconds); }},
        {"session and import JSONL", [&] { return fuzz_jsonl(dir); }},
        {"markdown", fuzz_markdown},
        {"ban filter", fuzz_bans},
    };
    for (const auto& t : targets) {
        int before = failures();
        long iterations = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            iterations += t.run();
            if (failures() > before + 5) break;  // enough examples
        }
        section(std::string(t.name) + ": " + std::to_string(iterations) + " iterations, " + std::to_string(failures() - before) + " invariant failures");
    }
    fs::remove_all(dir);
    return finish();
}
