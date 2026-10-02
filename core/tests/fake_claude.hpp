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
// It refuses to start (a failed result, exit 2) unless its tools are off, MCP is strict, the permission mode is
// dontAsk and --setting-sources is given ("" by default, or a subset of user,project,local): the tests' check that
// MAIC passes those flags. The MCP config is empty for text only. For an agent it names only `maic`, a stdio server
// whose last argument is MAIC's socket, with --allowedTools mcp__maic and MCP_TOOL_TIMEOUT set; the fake then
// speaks MCP to that socket (or, with FAKE_CLAUDE_BRIDGE=1, through the server command itself), records the
// handshake and tool list in <dir>/mcp.jsonl, and acts on lines of the text it is sent:
//   CALL <tool> <json>   a call to MAIC's tool, one model step each, its result read back over MCP
//   PARALLEL             all the CALLs in one step, sent over MCP at once
//   STRAY                a call to a tool that is not MAIC's
// and ends with "pid <pid> call <n>: results: <each result>" once the calls are done. A conversation handed over
// whole (labelled turns) is echoed like any other text.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/stat.h>

namespace fake_claude {

inline const char* kScript = R"PY(#!/usr/bin/env python3
import json, os, socket, subprocess, sys, time

args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
def out(j):
    sys.stdout.write(json.dumps(j) + "\n")
    sys.stdout.flush()
def ev(e):
    out({"type": "stream_event", "event": e})

home = os.environ["FAKE_CLAUDE_DIR"]
with open(os.path.join(home, "spawns.jsonl"), "a") as f:
    f.write(json.dumps({"pid": os.getpid(), "argv": args, "api_key": "ANTHROPIC_API_KEY" in os.environ, "other_key": "DEEPSEEK_API_KEY" in os.environ, "work_key": "MAIC_TEST_WORK_TOKEN" in os.environ, "cwd": os.getcwd(),
                        "tool_timeout": os.environ.get("MCP_TOOL_TIMEOUT")}) + "\n")
sources = opt("--setting-sources")
try:
    servers = json.loads(opt("--mcp-config") or "")["mcpServers"]
except Exception:
    servers = None
maic = servers.get("maic") if isinstance(servers, dict) else None
agent = maic is not None
ok = (opt("--tools") == "" and "--strict-mcp-config" in args and opt("--permission-mode") == "dontAsk"
      and sources is not None and (not sources or set(sources.split(",")) <= {"user", "project", "local"}))
if agent:
    ok = ok and list(servers) == ["maic"] and maic.get("type") == "stdio" and opt("--allowedTools") == "mcp__maic" and os.environ.get("MCP_TOOL_TIMEOUT")
else:
    ok = ok and servers == {} and "--allowedTools" not in args
if not ok:
    out({"type": "result", "subtype": "error_during_execution", "is_error": True, "result": "fake claude: started with tools reachable"})
    sys.exit(2)

class Mcp:
    def __init__(self):
        if os.environ.get("FAKE_CLAUDE_BRIDGE") == "1":
            self.proc = subprocess.Popen([maic["command"]] + maic["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            self.w, self.r = self.proc.stdin, self.proc.stdout
        else:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(maic["args"][-1])
            self.w, self.r = self.sock.makefile("wb"), self.sock.makefile("rb")
        self.next = 0
        self.answers = {}
    def send(self, j):
        self.w.write((json.dumps(j) + "\n").encode())
        self.w.flush()
    def request(self, method, params):
        self.next += 1
        self.send({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params})
        return self.next
    def wait(self, rid):
        while rid not in self.answers:
            line = self.r.readline()
            if not line:
                raise SystemExit(3)
            j = json.loads(line)
            if "id" in j:
                self.answers[j["id"]] = j
        return self.answers.pop(rid)

mcp = None
if agent:
    mcp = Mcp()
    init = mcp.wait(mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "fake-claude", "version": "0"}}))
    mcp.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    listed = mcp.wait(mcp.request("tools/list", {}))
    unknown = mcp.wait(mcp.request("resources/list", {}))
    with open(os.path.join(home, "mcp.jsonl"), "a") as f:
        f.write(json.dumps({"pid": os.getpid(), "initialize": init, "tools": listed, "unknown": unknown}) + "\n")

system = opt("--system-prompt") or ""
model = opt("--model") or ""
partial = "--include-partial-messages" in args
uses = 0

def stream_text(reply):
    if partial:
        for i in range(0, len(reply), 4):
            ev({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": reply[i:i + 4]}})

def step(calls):
    # One model message that calls `calls` [(name, args)], then the calls over MCP; returns their results.
    global uses
    ev({"type": "message_start", "message": {"usage": {"input_tokens": 11, "cache_read_input_tokens": 2}}})
    lead = "calling " + ", ".join(n for n, _ in calls)
    ev({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})
    stream_text(lead)
    ev({"type": "content_block_stop", "index": 0})
    blocks = []
    for i, (name, a) in enumerate(calls, start=1):
        uses += 1
        tid = "toolu_%d_%d" % (os.getpid(), uses)
        full = name if name == "Bash" else "mcp__maic__" + name
        blocks.append({"type": "tool_use", "id": tid, "name": full, "input": a})
        ev({"type": "content_block_start", "index": i, "content_block": {"type": "tool_use", "id": tid, "name": full, "input": {}}})
        text = json.dumps(a)
        for k in range(0, len(text), 5):
            ev({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": text[k:k + 5]}})
        ev({"type": "content_block_stop", "index": i})
    ev({"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 4}})
    ev({"type": "message_stop"})
    out({"type": "assistant", "message": {"content": [{"type": "text", "text": lead}] + blocks}})
    if any(n == "Bash" for n, _ in calls):
        return ["not one of MAIC's"]
    ids = [mcp.request("tools/call", {"name": n, "arguments": a}) for n, a in calls]
    results = []
    for rid, b in zip(ids, blocks):
        r = mcp.wait(rid)["result"]
        text = "".join(c.get("text", "") for c in r["content"])
        results.append(("ERROR " if r.get("isError") else "") + text)
        out({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": b["id"], "content": text}]}})
    return results

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
    calls = []
    # A conversation handed over whole ("[User]: ...") is only echoed: its calls were made before.
    for l in ([] if text.startswith("[") else text.splitlines()):
        l = l.strip()
        if l.startswith("CALL "):
            name, _, a = l[5:].partition(" ")
            calls.append((name, json.loads(a) if a else {}))
        elif l == "STRAY":
            calls.append(("Bash", {"command": "true"}))
    if agent and calls:
        results = []
        for group in ([calls] if "PARALLEL" in text else [[c] for c in calls]):
            results += step(group)
        reply = "pid %d call %d: results: %s" % (os.getpid(), n, " | ".join(results))
    elif system.startswith("You review one action"):
        reply = "ALLOW: fine"
    elif system.startswith("You write a handover note"):
        reply = "## Objective\n- summarised by " + model
    elif system.startswith("Write a title"):
        reply = "Title from " + model
    else:
        reply = "pid %d call %d: %s" % (os.getpid(), n, text)
    if agent:
        ev({"type": "message_start", "message": {"usage": {"input_tokens": 9}}})
    stream_text(reply)
    if agent:
        ev({"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 6}})
        ev({"type": "message_stop"})
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
    std::ofstream(dir / "mcp.jsonl", std::ios::trunc);
    setenv("FAKE_CLAUDE_DIR", dir.c_str(), 1);
    setenv("PATH", ((dir / "bin").string() + ":/usr/bin:/bin").c_str(), 1);
}

// Every start so far: {"pid", "argv", "api_key", "other_key" (DEEPSEEK_API_KEY was there), "work_key" (MAIC_TEST_WORK_TOKEN was), "cwd", "tool_timeout"}.
inline std::vector<nlohmann::json> spawns(const std::filesystem::path& dir, const char* file = "spawns.jsonl") {
    std::vector<nlohmann::json> out;
    std::ifstream in(dir / file);
    for (std::string l; std::getline(in, l);) {
        auto j = nlohmann::json::parse(l, nullptr, false);
        if (j.is_object()) out.push_back(j);
    }
    return out;
}

// Every agent's MCP handshake: {"pid", "initialize", "tools", "unknown"}, the answers MAIC's server gave.
inline std::vector<nlohmann::json> handshakes(const std::filesystem::path& dir) {
    return spawns(dir, "mcp.jsonl");
}

}  // namespace fake_claude
