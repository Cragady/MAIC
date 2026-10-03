// User-defined Lua tools: loading, the sandboxed state, and the harness in front of every maid.* call. The
// authorise step here follows the real policy but never touches the tripwire lock: a Trip is recorded as a
// verdict, the way harness_test checks trip patterns.
#include "check.hpp"

#include "maid/lua_tools.hpp"
#include "maid/trust.hpp"

#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace maid;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// An in-memory tool whose run body is `body`.
LuaTool tool(const std::string& body) {
    return {"probe", "a probe", json::object(), "=probe", "return { name = 'probe', description = 'a probe', run = function(args) " + body + " end }"};
}

const LuaTool* find(const LuaToolSet& set, const std::string& name) {
    for (const auto& t : set.tools) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

bool has_notice(const LuaToolSet& set, const std::string& what) {
    for (const auto& n : set.notices) {
        if (n.find(what) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main() {
    setenv("MAID_TRIPWIRE_FILE", ("/tmp/maid-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);  // never the machine's lock
    fs::path ws = fs::temp_directory_path() / "maid-lua-tools-test";
    fs::remove_all(ws);
    fs::create_directories(ws / "cfg");
    setenv("XDG_CONFIG_HOME", (ws / "cfg").c_str(), 1);
    trust_for_session(ws);  // the tools below are this test's own (trust_test covers untrusted ones)
    Harness h(ws);
    std::atomic<bool> no_cancel{false};

    // Policy in auto mode, with the user answering No to anything asked. Records what was gated.
    std::vector<std::string> gated;
    std::vector<Verdict> verdicts;
    Authorise authorise = [&](const Action& a, const std::string& summary, const std::string&) {
        Decision d = h.check(a, Mode::Auto, Origin::Local);
        gated.push_back(summary);
        verdicts.push_back(d.verdict);
        switch (d.verdict) {
            case Verdict::Allow: return d;
            case Verdict::Ask: return Decision{Verdict::Deny, "DENIED by the user, who says: not there"};
            case Verdict::Deny: return Decision{Verdict::Deny, "DENIED: " + d.reason};
            case Verdict::Trip: return Decision{Verdict::Trip, "BLOCKED and the harness was tripped (" + d.reason + "). Stop and explain to the user."};
        }
        return d;
    };
    auto run = [&](const LuaTool& t, json args = json::object(), std::chrono::seconds timeout = std::chrono::seconds(60)) {
        return run_lua_tool(t, args, h, authorise, no_cancel, timeout);
    };

    section("loading");
    write_file(ws / ".maid" / "tools" / "word_count.lua",
               "return {\n"
               "  name = 'word_count',\n"
               "  description = 'counts the words in a file',\n"
               "  parameters = { type = 'object', properties = { path = { type = 'string', description = 'the file' } }, required = { 'path' } },\n"
               "  run = function(args)\n"
               "    local n = 0\n"
               "    for _ in maid.read(args.path):gmatch('%S+') do n = n + 1 end\n"
               "    return n .. ' words'\n"
               "  end,\n"
               "}\n");
    write_file(ws / ".maid" / "tools" / "broken.lua", "return { name = 'broken', description = 'x', run = function(args) return 1 +  end }\n");
    write_file(ws / ".maid" / "tools" / "builtin.lua", "return { name = 'read_file', description = 'x', run = function() end }\n");
    write_file(ws / ".maid" / "tools" / "norun.lua", "return { name = 'norun', description = 'x' }\n");
    write_file(ws / ".maid" / "tools" / "badname.lua", "return { name = 'Bad Name', description = 'x', run = function() end }\n");
    write_file(ws / ".maid" / "tools" / "eager.lua", "return { name = 'eager', description = 'x', body = maid.read('x'), run = function() end }\n");
    write_file(ws / ".maid" / "tools" / "zz_dup.lua", "return { name = 'word_count', description = 'again', run = function() end }\n");
    write_file(ws / "cfg" / "maid" / "tools" / "global_tool.lua", "return { name = 'global_tool', description = 'from the config dir', required = {}, run = function() return 'g' end }\n");
    write_file(ws / "cfg" / "maid" / "tools" / "notes.txt", "not a tool\n");
    LuaToolSet set = load_lua_tools(ws);
    expect(set.tools.size() == 2 && find(set, "word_count") && find(set, "global_tool"), "the good workspace tool and the global tool load; " + std::to_string(set.tools.size()) + " loaded");
    expect(find(set, "word_count") && find(set, "word_count")->file == ws / ".maid" / "tools" / "word_count.lua" && find(set, "word_count")->description == "counts the words in a file",
           "a tool remembers its file and description");
    expect(find(set, "word_count") && find(set, "word_count")->parameters["required"] == json::array({"path"}) && find(set, "word_count")->parameters["type"] == "object",
           "the parameters table becomes a JSON schema");
    expect(find(set, "global_tool") && find(set, "global_tool")->parameters == json({{"type", "object"}, {"properties", json::object()}}),
           "missing parameters default to an empty object schema");
    expect(set.notices.size() == 6, "every bad file gets one notice: " + std::to_string(set.notices.size()));
    expect(has_notice(set, "broken.lua") && has_notice(set, "expected"), "a syntax error names the file and the Lua message");
    expect(has_notice(set, "builtin.lua") && has_notice(set, "built-in"), "a built-in name is refused");
    expect(has_notice(set, "norun.lua") && has_notice(set, "`run`"), "a missing run function is refused");
    expect(has_notice(set, "badname.lua") && has_notice(set, "lowercase"), "a name with spaces or capitals is refused");
    expect(has_notice(set, "eager.lua") && has_notice(set, "maid"), "a file that acts at load time has no maid table and is skipped");
    expect(has_notice(set, "zz_dup.lua") && has_notice(set, "already defined"), "a duplicate name is skipped");
    write_file(ws / "poem.txt", "one two three\nfour\n");
    auto wc = run(*find(set, "word_count"), {{"path", "poem.txt"}});
    expect(wc.ok && wc.text == "4 words", "the loaded tool runs with its arguments: " + wc.text);

    section("reads, writes and the harness");
    gated.clear();
    auto r = run(tool("return maid.read('poem.txt')"));
    expect(r.ok && r.text == "one two three\nfour\n" && gated == std::vector<std::string>{"read_file poem.txt"} && verdicts.back() == Verdict::Allow,
           "maid.read goes through the harness as a read_file and returns the contents");
    fs::path outside = fs::temp_directory_path() / "maid-lua-tools-escape.txt";
    fs::remove(outside);
    gated.clear();
    r = run(tool("maid.write('" + outside.string() + "', 'x') return 'wrote'"));
    expect(!r.ok && r.text == "DENIED by the user, who says: not there", "a write outside the workspace is asked, denied, and the Lua error is the denial text: " + r.text);
    expect(!fs::exists(outside) && verdicts.back() == Verdict::Ask, "nothing was written");
    r = run(tool("local ok, err = pcall(maid.write, '" + outside.string() + "', 'x') return tostring(ok) .. ':' .. err"));
    expect(r.ok && r.text == "false:DENIED by the user, who says: not there", "pcall inside the tool sees the same text");
    r = run(tool("maid.write('made/inside.txt', 'hello') return maid.read('made/inside.txt')"));
    expect(r.ok && r.text == "hello" && fs::exists(ws / "made" / "inside.txt"), "a write inside the workspace runs in auto mode and creates directories");
    r = run(tool("return maid.read('" + std::string(std::getenv("HOME")) + "/.ssh/id_ed25519')"));
    expect(!r.ok && r.text.rfind("DENIED: credentials", 0) == 0, "secrets are denied by policy: " + r.text);
    r = run(tool("return table.concat(maid.list('.'), ',')"));
    expect(r.ok && r.text.find("poem.txt") != std::string::npos && r.text.find("made/") != std::string::npos, "maid.list returns a table of entries: " + r.text);
    r = run(tool("return maid.search('four', '.')"));
    expect(r.ok && r.text.find("poem.txt:2: four") != std::string::npos, "maid.search returns grep-style lines: " + r.text);
    r = run(tool("return maid.json_encode({ a = 1, b = { 'x', 'y' } })"));
    expect(r.ok && r.text == R"({"a":1,"b":["x","y"]})", "json_encode: " + r.text);
    r = run(tool("local t = maid.json_decode('{\"b\":[1,2],\"s\":\"q\"}') return t.b[2] .. t.s"));
    expect(r.ok && r.text == "2q", "json_decode: " + r.text);
    r = run(tool("return maid.json_decode('{nope')"));
    expect(!r.ok && r.text.find("not valid JSON") != std::string::npos, "bad JSON is an error");
    r = run(tool("return { n = 2, list = { 'a' } }"));
    expect(r.ok && json::parse(r.text) == json({{"n", 2}, {"list", {"a"}}}), "a table result is returned as JSON: " + r.text);
    r = run(tool("print('first', 1) return 'second'"));
    expect(r.ok && r.text == "first\t1\nsecond", "print output comes before the returned value");
    r = run(tool("return args.name .. '!'"), {{"name", "mica"}});
    expect(r.ok && r.text == "mica!", "arguments arrive as a table");
    r = run(tool("error('boom')"));
    expect(!r.ok && r.text.find("boom") != std::string::npos, "an error inside run fails the call with its message: " + r.text);
    r = run(tool("return nil"));
    expect(r.ok && r.text == "(no output)", "nothing returned reads as no output");

    section("shell");
    gated.clear();
    r = run(tool("return maid.shell('sudo ls')"));
    expect(!r.ok && r.text.rfind("BLOCKED and the harness was tripped (privilege escalation)", 0) == 0 && verdicts.back() == Verdict::Trip,
           "a shell call with sudo is a trip verdict and the tool sees BLOCKED: " + r.text);
    expect(gated.size() == 1 && gated[0] == "$ sudo ls", "the action is the command itself");
    r = run(tool("local out, code = maid.shell('echo hi; exit 3') return out .. code"));
    expect(r.ok && r.text == "hi\n3", "an allowed command runs in the sandbox and returns output and exit code: " + r.text);
    r = run(tool("local out = maid.shell('pwd', { workdir = 'made' }) return out"));
    expect(r.ok && r.text.find("/made") != std::string::npos, "opts.workdir is honoured: " + r.text);
    r = run(tool("return maid.shell('sleep 30', { timeout = 1 })"));
    expect(!r.ok && r.text.find("timeout") != std::string::npos, "opts.timeout is honoured: " + r.text);

    section("runaway tools");
    auto t0 = std::chrono::steady_clock::now();
    r = run(tool("while true do end"), json::object(), std::chrono::seconds(1));
    expect(!r.ok && r.text.find("time limit") != std::string::npos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5),
           "an endless loop is aborted by the hook at the time limit: " + r.text);
    {
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel = true;
        });
        t0 = std::chrono::steady_clock::now();
        auto c = run_lua_tool(tool("local n = 0 while true do n = n + 1 end"), json::object(), h, authorise, cancel);
        canceller.join();
        expect(!c.ok && c.text.find("cancelled") != std::string::npos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "cancel stops a loop: " + c.text);
    }
    r = run(tool("return string.rep('x', 100000)"));
    expect(r.ok && r.text.size() < 66 * 1024 && r.text.find("[truncated at 64 KB]") != std::string::npos, "output is capped at 64 KB");
    r = run(tool("for i = 1, 100000 do print('line') end"));
    expect(!r.ok && r.text.find("more than 64 KB") != std::string::npos, "printing past the cap stops the tool");

    section("what the sandbox has and lacks");
    r = run(tool("return tostring(io) .. tostring(os) .. tostring(require) .. tostring(load) .. tostring(loadstring) .. tostring(dofile) .. tostring(loadfile) "
                 ".. tostring(package) .. tostring(debug) .. tostring(ffi) .. tostring(jit)"));
    expect(r.ok && r.text == "nilnilnilnilnilnilnilnilnilnilnil", "io, os, require, load, loadstring, dofile, loadfile, package, debug, ffi and jit are absent: " + r.text);
    r = run(tool("return string.format('%d', math.floor(2.7)) .. table.concat({ 'a', 'b' }) .. bit.band(6, 3) .. type(pairs)"));
    expect(r.ok && r.text == "2ab2function", "string, table, math, bit and the base library are there: " + r.text);
    r = run(tool("leak = 1 return tostring(leak)"));
    auto again = run(tool("return tostring(leak)"));
    expect(r.text == "1" && again.text == "nil", "every call gets a fresh state: globals do not persist");

    fs::remove_all(ws);
    return finish();
}
