#pragma once

// A stand-in for `claude -p` for the `cli` provider tests: it speaks the stream-json diction's ClaudeScribe
// reads (a `result` line ends each reply, `is_error` marks a failure), streams partial text, and records every
// start in <dir>/spawns.jsonl. The real CLI is never run: install() puts this script first on a PATH that
// holds only it and the system directories, so `claude` can only be the fake.
//
// What it answers, by the text it is sent or its system prompt:
//   LIMIT   a failed result with the Fable usage-limit message
//   HANG    nothing, until it is killed
//   EXIT    the usual reply, then it exits
//   reviewer / handover note / title system prompts: "ALLOW: fine", a note, a title
//   anything else: "pid <pid> call <n>: <text>"
// It refuses to start (a failed result, exit 2) unless its tools are off, MCP is strict and empty, the
// permission mode is dontAsk and --setting-sources is given ("" by default, or a subset of user,project,local):
// the tests' check that MAIC passes those flags.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/stat.h>

namespace fake_claude {

inline const char* kScript = R"PY(#!/usr/bin/env python3
import json, os, sys, time

args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
def out(j):
    sys.stdout.write(json.dumps(j) + "\n")
    sys.stdout.flush()

with open(os.path.join(os.environ["FAKE_CLAUDE_DIR"], "spawns.jsonl"), "a") as f:
    f.write(json.dumps({"pid": os.getpid(), "argv": args, "api_key": "ANTHROPIC_API_KEY" in os.environ, "cwd": os.getcwd()}) + "\n")
sources = opt("--setting-sources")
if (opt("--tools") != "" or "--strict-mcp-config" not in args or opt("--mcp-config") != '{"mcpServers":{}}' or opt("--permission-mode") != "dontAsk"
        or sources is None or (sources and not set(sources.split(",")) <= {"user", "project", "local"})):
    out({"type": "result", "subtype": "error_during_execution", "is_error": True, "result": "fake claude: started with tools reachable"})
    sys.exit(2)
system = opt("--system-prompt") or ""
model = opt("--model") or ""
partial = "--include-partial-messages" in args
n = 0
while True:
    line = sys.stdin.readline()
    if not line:
        break
    text = "".join(b.get("text", "") for b in json.loads(line)["message"]["content"])
    n += 1
    if n == 1:
        out({"type": "system", "subtype": "init", "model": model, "tools": []})
    if "HANG" in text:
        time.sleep(120)
        continue
    if "LIMIT" in text:
        msg = "You've reached your Fable limit. Run /usage-credits to continue or switch models with /model."
        out({"type": "assistant", "message": {"content": [{"type": "text", "text": msg}]}})
        out({"type": "result", "subtype": "success", "is_error": True, "result": msg})
        continue
    if system.startswith("You review one action"):
        reply = "ALLOW: fine"
    elif system.startswith("You write a handover note"):
        reply = "## Objective\n- summarised by " + model
    elif system.startswith("Write a title"):
        reply = "Title from " + model
    else:
        reply = "pid %d call %d: %s" % (os.getpid(), n, text)
    if partial:
        for i in range(0, len(reply), 4):
            out({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": reply[i:i + 4]}}})
    out({"type": "assistant", "message": {"content": [{"type": "text", "text": reply}]}})
    out({"type": "result", "subtype": "success", "is_error": False, "result": reply,
         "usage": {"input_tokens": 7, "cache_read_input_tokens": 3, "output_tokens": 5}, "modelUsage": {model: {"contextWindow": 200000}}})
    if "EXIT" in text:
        sys.exit(0)
)PY";

// Writes the fake as <dir>/bin/claude and makes PATH that directory plus the system ones.
inline void install(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "bin");
    std::filesystem::path script = dir / "bin" / "claude";
    std::ofstream(script) << kScript;
    chmod(script.c_str(), 0755);
    std::ofstream(dir / "spawns.jsonl", std::ios::trunc);
    setenv("FAKE_CLAUDE_DIR", dir.c_str(), 1);
    setenv("PATH", ((dir / "bin").string() + ":/usr/bin:/bin").c_str(), 1);
}

// Every start so far: {"pid", "argv", "api_key", "cwd"}.
inline std::vector<nlohmann::json> spawns(const std::filesystem::path& dir) {
    std::vector<nlohmann::json> out;
    std::ifstream in(dir / "spawns.jsonl");
    for (std::string l; std::getline(in, l);) {
        auto j = nlohmann::json::parse(l, nullptr, false);
        if (j.is_object()) out.push_back(j);
    }
    return out;
}

}  // namespace fake_claude
