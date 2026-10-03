// Script tools: manifests, argument checks against the schema, the declared reads and writes as actions, and the
// sandboxed run with the arguments on stdin. The sandbox is the real bubblewrap, used the way harness_test uses it.
#include "check.hpp"

#include "maid/script_tools.hpp"
#include "maid/trust.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

using namespace maid;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// A tool directory under ws/.maid/tools/<dir> holding tool.json (from `manifest`, with `run` and a script when given).
fs::path make_tool(const fs::path& ws, const std::string& dir, json manifest, const std::string& script = "", const std::string& script_name = "main.sh") {
    fs::path d = ws / ".maid" / "tools" / dir;
    fs::create_directories(d);
    if (!script.empty()) write_file(d / script_name, script);
    std::ofstream(d / "tool.json") << manifest.dump(2);
    return d / "tool.json";
}

json manifest(const std::string& name, json run = json::array({"sh", "main.sh"})) {
    return {{"name", name}, {"description", "a probe"}, {"parameters", {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}}}, {"run", run}};
}

const ScriptTool* find(const ScriptToolSet& set, const std::string& name) {
    for (const auto& t : set.tools) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

bool has_notice(const ScriptToolSet& set, const std::string& file, const std::string& what) {
    for (const auto& n : set.notices) {
        if (n.find(file) != std::string::npos && n.find(what) != std::string::npos) return true;
    }
    return false;
}

std::string caught(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

int main() {
    setenv("MAID_TRIPWIRE_FILE", ("/tmp/maid-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);
    fs::path ws = fs::temp_directory_path() / "maid-script-tools-test";
    fs::remove_all(ws);
    fs::create_directories(ws / "cfg");
    setenv("XDG_CONFIG_HOME", (ws / "cfg").c_str(), 1);
    trust_for_session(ws);  // the tools below are this test's own (trust_test covers untrusted ones)
    Harness h(ws);
    std::atomic<bool> no_cancel{false};

    section("manifests");
    std::string echo_sh = "#!/bin/sh\ncat\n";
    make_tool(ws, "echo_args", manifest("echo_args"), echo_sh);
    make_tool(ws, "no_name", {{"description", "x"}, {"run", {"sh", "main.sh"}}}, echo_sh);
    make_tool(ws, "camel", manifest("CamelCase"), echo_sh);
    make_tool(ws, "digit", manifest("1st"), echo_sh);
    make_tool(ws, "builtin", manifest("read_file"), echo_sh);
    make_tool(ws, "net", [] { json m = manifest("net"); m["network"] = true; return m; }(), echo_sh);
    make_tool(ws, "no_run", {{"name", "no_run"}, {"description", "x"}});
    make_tool(ws, "run_str", {{"name", "run_str"}, {"description", "x"}, {"run", "sh main.sh"}});
    make_tool(ws, "missing_prog", manifest("missing_prog", {"no-such-interpreter-anywhere", "main.sh"}), echo_sh);
    make_tool(ws, "bad_params", [] { json m = manifest("bad_params"); m["parameters"] = {{"type", "object"}, {"properties", {{"n", {{"type", "count"}}}}}}; return m; }(), echo_sh);
    make_tool(ws, "params_list", [] { json m = manifest("params_list"); m["parameters"] = json::array(); return m; }(), echo_sh);
    make_tool(ws, "bad_timeout", [] { json m = manifest("bad_timeout"); m["timeout_s"] = "soon"; return m; }(), echo_sh);
    make_tool(ws, "bad_reads", [] { json m = manifest("bad_reads"); m["reads"] = "docs/**"; return m; }(), echo_sh);
    make_tool(ws, "zz_dup", manifest("echo_args"), echo_sh);
    make_tool(ws, "taken", manifest("word_count"), echo_sh);
    write_file(ws / ".maid" / "tools" / "not_json" / "tool.json", "{nope");
    write_file(ws / ".maid" / "tools" / "loose.lua", "return {}");  // a Lua file: not this loader's business
    write_file(ws / "cfg" / "maid" / "tools" / "global_tool" / "tool.json", manifest("global_tool", {"python3", "main.py"}).dump());
    write_file(ws / "cfg" / "maid" / "tools" / "global_tool" / "main.py", "import json, sys\nprint('from the config dir', json.load(sys.stdin)['text'])\n");
    ScriptToolSet set = load_script_tools(ws, {"word_count"});
    expect(set.tools.size() == 2 && find(set, "echo_args") && find(set, "global_tool"), "the good workspace tool and the global tool load; " + std::to_string(set.tools.size()) + " loaded");
    expect(set.notices.size() == 15, "every bad manifest gets one notice: " + std::to_string(set.notices.size()));
    expect(has_notice(set, "no_name", "`name`"), "a missing name is refused");
    expect(has_notice(set, "camel", "snake_case") && has_notice(set, "digit", "snake_case"), "CamelCase and a leading digit are refused");
    expect(has_notice(set, "builtin", "built-in"), "a built-in's name is refused");
    expect(has_notice(set, "net", "per-tool network grants are not implemented yet"), "network: true is refused with the message");
    expect(has_notice(set, "no_run", "`run`") && has_notice(set, "run_str", "`run`"), "run must be a non-empty list");
    expect(has_notice(set, "missing_prog", "not on PATH"), "an interpreter that is not on PATH is refused");
    expect(has_notice(set, "bad_params", "unknown type `count`") && has_notice(set, "params_list", "`parameters`"), "a bad schema is refused with the problem named");
    expect(has_notice(set, "bad_timeout", "`timeout_s`") && has_notice(set, "bad_reads", "`reads`"), "timeout_s and reads are checked");
    expect(has_notice(set, "zz_dup", "already defined by"), "a duplicate name is skipped, naming the first");
    expect(has_notice(set, "taken", "already defined by a Lua tool"), "a name a Lua tool holds is skipped");
    expect(has_notice(set, "not_json", "not valid JSON"), "a manifest that is not JSON is skipped");
    const ScriptTool& echo = *find(set, "echo_args");
    expect(echo.timeout_s == 60 && echo.reads.empty() && echo.writes.empty() && echo.dir == ws / ".maid" / "tools" / "echo_args", "defaults: 60 s, nothing declared, the directory remembered");
    expect(echo.run.size() == 2 && echo.run[0] == "sh" && echo.run[1] == (echo.dir / "main.sh").string(), "an argument naming a file in the directory is made absolute: " + echo.run[1]);
    expect(script_tool_language(echo) == "sh" && script_tool_language(*find(set, "global_tool")) == "python", "the language comes from the program");
    std::string err = caught([&] { read_script_tool(ws / ".maid" / "tools" / "net" / "tool.json"); });
    expect(err.find("network") != std::string::npos, "read_script_tool throws the same reason for `maid tools check`: " + err);

    section("arguments against the schema");
    json schema = {{"type", "object"},
                   {"properties", {{"path", {{"type", "string"}}}, {"n", {{"type", "integer"}}}, {"mode", {{"type", "string"}, {"enum", {"fast", "slow"}}}},
                                   {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"opt", {{"type", {"string", "null"}}}}}},
                   {"required", {"path"}},
                   {"additionalProperties", false}};
    expect(check_arguments(schema, {{"path", "a"}, {"n", 2}, {"mode", "fast"}, {"tags", {"x"}}, {"opt", nullptr}}).empty(), "fitting arguments pass");
    expect(check_arguments(schema, {{"n", 2}}) == "missing required argument path", "a missing required argument: " + check_arguments(schema, {{"n", 2}}));
    expect(check_arguments(schema, {{"path", 3}}) == "argument path must be a string, not integer", "a wrong type: " + check_arguments(schema, {{"path", 3}}));
    expect(check_arguments(schema, {{"path", "a"}, {"n", 2.5}}).find("integer") != std::string::npos, "a fraction is not an integer");
    expect(check_arguments(schema, {{"path", "a"}, {"n", 2.0}}).empty(), "a whole float passes as an integer");
    expect(check_arguments(schema, {{"path", "a"}, {"mode", "slower"}}) == "argument mode must be one of: fast, slow", "enum: " + check_arguments(schema, {{"path", "a"}, {"mode", "slower"}}));
    expect(check_arguments(schema, {{"path", "a"}, {"tags", {"x", 1}}}) == "argument tags[1] must be a string, not integer", "items are checked: " + check_arguments(schema, {{"path", "a"}, {"tags", {"x", 1}}}));
    expect(check_arguments(schema, {{"path", "a"}, {"extra", 1}}) == "unknown argument extra", "additionalProperties false refuses an unknown argument");
    expect(check_arguments(schema, json::array()) == "the arguments must be a JSON object", "non-object arguments are refused");
    json open_schema = {{"type", "object"}, {"properties", json::object()}};
    expect(check_arguments(open_schema, {{"anything", 1}}).empty(), "without additionalProperties false, extra arguments pass");
    expect(check_schema({{"type", "object"}, {"properties", {{"a", {{"type", "string"}}}}}, {"required", {"b"}}}).find("required `b`") != std::string::npos, "check_schema: a required name not in properties");
    expect(check_schema({{"type", "object"}, {"properties", json::array()}}).find("`properties`") != std::string::npos, "check_schema: properties must be an object");
    expect(check_schema(schema).empty(), "check_schema passes a good one");

    section("declared reads and writes as actions");
    ScriptTool decl = echo;
    decl.reads = {"docs/**/*.md", "*", "/etc/hosts"};
    decl.writes = {"out/*.txt", "**"};
    auto actions = script_tool_actions(h, decl);
    expect(actions.size() == 5, "one action per glob: " + std::to_string(actions.size()));
    expect(actions[0].kind == Action::Kind::Read && actions[0].path == ws / "docs" && actions[0].tool == "echo_args", "a read glob becomes a Read at its fixed prefix, from the tool");
    expect(actions[1].path == ws && actions[2].path == "/etc/hosts", "`*` is the workspace itself, an absolute path stays absolute");
    expect(actions[3].kind == Action::Kind::Write && actions[3].path == ws / "out" && actions[4].path == ws, "write globs become Writes");
    expect(h.check(actions[0], Mode::Manual, Origin::Local).verdict == Verdict::Allow && h.check(actions[2], Mode::Manual, Origin::Local).verdict == Verdict::Ask &&
               h.check(actions[3], Mode::Manual, Origin::Local).verdict == Verdict::Ask && h.check(actions[3], Mode::Auto, Origin::Local).verdict == Verdict::Allow,
           "the harness judges them like any read or write: a read outside asks, a write asks in manual and runs in auto");
    decl.writes = {"/tmp/elsewhere/*"};
    err = caught([&] { script_tool_actions(h, decl); });
    expect(err.find("writes only inside the workspace") != std::string::npos && err.find("/tmp/elsewhere") != std::string::npos, "a write glob outside the workspace throws: " + err);
    decl.writes = {"../sibling/*"};
    err = caught([&] { script_tool_actions(h, decl); });
    expect(err.find("writes only inside the workspace") != std::string::npos, "so does one that climbs out with ..");

    section("running");
    auto r = run_script_tool(echo, {{"text", "hi"}, {"n", 1}}, h, true, no_cancel);
    expect(r.ok && json::parse(r.text) == json({{"text", "hi"}, {"n", 1}}), "the arguments arrive as JSON on stdin and stdout is the result: " + r.text);
    r = run_script_tool(*find(set, "global_tool"), {{"text", "mica"}}, h, true, no_cancel);
    expect(r.ok && r.text == "from the config dir mica", "a Python tool from the config directory runs: " + r.text);
    make_tool(ws, "fail", manifest("fail"), "#!/bin/sh\necho partial\necho 'bad thing' >&2\nexit 3\n");
    make_tool(ws, "quiet_ok", manifest("quiet_ok"), "#!/bin/sh\necho 'noise' >&2\necho fine\n");
    make_tool(ws, "big", manifest("big"), "#!/bin/sh\nhead -c 100000 /dev/zero | tr '\\0' x\n");
    make_tool(ws, "slow", [] { json m = manifest("slow"); m["timeout_s"] = 1; return m; }(), "#!/bin/sh\nsleep 30\n");
    make_tool(ws, "touch", manifest("touch"), "#!/bin/sh\necho made > made.txt && echo ok\n");
    make_tool(ws, "pwd", manifest("pwd"), "#!/bin/sh\npwd\n");
    make_tool(ws, "nothing", manifest("nothing"), "#!/bin/sh\n");
    make_tool(ws, "net_probe", manifest("net_probe"), "#!/bin/sh\ncat /proc/net/route | wc -l\n");
    set = load_script_tools(ws);
    r = run_script_tool(*find(set, "fail"), json::object(), h, true, no_cancel);
    expect(!r.ok && r.text == "exit code 3\npartial\nstderr:\nbad thing", "a non-zero exit fails the call with stdout then stderr: " + r.text);
    r = run_script_tool(*find(set, "quiet_ok"), json::object(), h, true, no_cancel);
    expect(r.ok && r.text == "fine", "on success stderr is left out: " + r.text);
    r = run_script_tool(*find(set, "big"), json::object(), h, true, no_cancel);
    expect(r.ok && r.text.size() < 40 * 1024 && r.text.find("bytes omitted") != std::string::npos, "output is capped like command output: " + std::to_string(r.text.size()) + " bytes");
    auto t0 = std::chrono::steady_clock::now();
    r = run_script_tool(*find(set, "slow"), json::object(), h, true, no_cancel);
    expect(!r.ok && r.text.find("exceeded its timeout of 1 s") != std::string::npos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "the script is killed at timeout_s: " + r.text);
    r = run_script_tool(*find(set, "touch"), json::object(), h, true, no_cancel);
    expect(!r.ok && !fs::exists(ws / "made.txt"), "with the workspace read-only a write fails: " + r.text);
    r = run_script_tool(*find(set, "touch"), json::object(), h, false, no_cancel);
    expect(r.ok && fs::exists(ws / "made.txt"), "with the workspace writable it lands: " + r.text);
    r = run_script_tool(*find(set, "pwd"), json::object(), h, true, no_cancel);
    expect(r.ok && r.text == ws.string(), "the working directory is the workspace: " + r.text);
    r = run_script_tool(*find(set, "nothing"), json::object(), h, true, no_cancel);
    expect(r.ok && r.text == "(no output)", "nothing printed reads as no output");
    r = run_script_tool(*find(set, "net_probe"), json::object(), h, true, no_cancel);
    expect(r.ok && r.text == "1", "the sandbox has no network: only the header line in /proc/net/route, got " + r.text);
    {
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel = true;
        });
        t0 = std::chrono::steady_clock::now();
        auto c = run_script_tool(*find(set, "slow"), json::object(), h, true, cancel);
        canceller.join();
        expect(!c.ok && c.text.find("cancelled") != std::string::npos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "cancel stops it: " + c.text);
    }

    section("scaffolding");
    for (const char* lang : {"python", "sh", "perl", "node"}) {
        fs::path dir = ws / "new" / lang;
        auto files = scaffold_script_tool(dir, std::string("probe_") + lang, lang);
        ScriptTool t;
        err = caught([&] { t = read_script_tool(dir / "tool.json"); });
        bool loaded = err.empty();
        if (!loaded && err.find("not on PATH") != std::string::npos) {
            expect(true, std::string(lang) + ": scaffolded; its interpreter is not installed here, so the manifest check says so");
            continue;
        }
        expect(files.size() == 2 && loaded, std::string(lang) + ": the manifest and the stub are written and load: " + err);
        if (!loaded) continue;
        auto out = run_script_tool(t, {{"text", "echo me"}}, h, true, no_cancel);
        expect(out.ok && out.text.find("echo me") != std::string::npos, std::string(lang) + ": the stub echoes its arguments: " + out.text);
    }
    err = caught([&] { scaffold_script_tool(ws / "new" / "python", "again", "python"); });
    expect(err.find("already exists") != std::string::npos, "an existing directory is never overwritten");
    err = caught([&] { scaffold_script_tool(ws / "new" / "x", "read_file", "python"); });
    expect(err.find("built-in") != std::string::npos, "a built-in's name is refused");
    err = caught([&] { scaffold_script_tool(ws / "new" / "y", "ok_name", "ruby"); });
    expect(err.find("--lang") != std::string::npos, "an unknown language is refused");

    fs::remove_all(ws);
    return finish();
}
