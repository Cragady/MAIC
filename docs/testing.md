# Testing

How MAIC is tested, how to run all of it, and the rules the flaky tests taught. The gate is `scripts/check.sh`; everything below is what it runs or what runs it.

## Running everything

```sh
export VCPKG_ROOT=~/Toolkits/vcpkg          # your shell does this
scripts/check.sh                            # configure, build, ctest, then the python suites verbosely
scripts/check.sh --quick                    # configure, build, ctest (the pre-push hook)
scripts/check-asan.sh [FUZZ_SECONDS]        # the same under AddressSanitizer and UBSan, plus a long fuzz run (default 20 s per target)
```

`check.sh` stops at the first failing step and gates on the **build's** exit code before running any test. That order matters: `ctest` happily runs stale binaries when a test target fails to compile, and a grep over its output once let a failure through. `scripts/release.sh` runs `check.sh` before it tags or installs anything, and refuses the release when it fails.

The pre-push hook is not installed by itself. Once per clone:

```sh
git config core.hooksPath .githooks         # .githooks/pre-push runs scripts/check.sh --quick
```

`git push --no-verify` skips it when that is the right thing to do.

### The suites, one by one

| ctest name | What | Where |
| :--- | :--- | :--- |
| `harness` | policy, sandbox, tripwire, classifier | `core/tests/harness_test.cpp` |
| `robustness` | settings, places, tools, the ban filter, services, vendoring | `core/tests/robustness_test.cpp` |
| `llm` | the provider clients against fake servers | `core/tests/llm_test.cpp` |
| `agent` | the agent loop against `FakeServer`: turns, tools, approvals, compaction, subagents, cancel, resume; then every chunk `FakeServer` sent against OpenAI's pinned `CreateChatCompletionStreamResponse` | `core/tests/agent_test.cpp` |
| `jsonschema` | the JSON Schema validator's keywords with known answers; OpenAI's pinned subset against a Responses stream and a llama-server chat stream (`core/tests/fixtures/`); the adapter normalizations, each refused as llama.cpp sends it and accepted once normalized | `core/tests/jsonschema_test.cpp` |
| `openai_subset` | `protocol/openai/openapi.json` is the pinned file and `subset.json` is what `extract.py` makes of it | `protocol/openai/extract.py --check` |
| `protocol_schema` | `protocol/` against itself, OpenAI's pinned description and the design: the keywords the schemas use, every method an `operationId`, a Responses WebSocket client event or `maic.`, no MAIC schema with an OpenAI name, the dispatcher and the event list against the OpenRPC document and the union both ways, `ordering.json` naming real events and reachable states, hand-made streams through the checker, every JSON example in `docs/design/engine-protocol.md` | `core/tests/protocol_schema_test.cpp` |
| `protocol_conformance` | the engine in-process against `FakeServer`, each exchange recorded and checked as it runs, then mutated (below) | `core/tests/protocol_conformance_test.cpp` |
| `protocol_check`, `protocol_check_openai` | `maic protocol check` over what `protocol_conformance` recorded, and over its OpenAI-only view (`--openai`); they need it to have run first (a ctest fixture) | `CMakeLists.txt` |
| `session` | session files, homes, forks, import | `core/tests/session_test.cpp` |
| `lua_tools` | user-defined Lua tools | `core/tests/lua_tools_test.cpp` |
| `tool_output` | streamed command output: chunks, offsets, batching, stdout and stderr apart, the model's result byte for byte, a slow consumer timed; kept outputs: the file against the stream, the size cap, the index, the screen and the replay's order and timing | `core/tests/tool_output_test.cpp` |
| `fuzz` | the parser fuzzer, 2 s per target by default | `core/tests/fuzz_parsers.cpp` |
| `server` | maic-server over HTTP | `server/tests/server_test.cpp` |
| `editor` | the vim input | `cli/tests/editor_test.cpp` |
| `nvim` | maic.nvim against a headless `nvim --listen`: the ancestry check (also the real `maic` as a terminal job of that nvim), the host connection, `:e`, the diff, the theme follow, User autocmds, the `diagnostics` tool as a read, `maic.nvim` in Lua; then the plugin's own Lua tests (`nvim -l maic.nvim/tests/maic_test.lua`). The interface itself (`maic.nvim/tests/ui_test.lua`, against `maic --rpc` and the fake) runs from `tests/cli_smoke.py`. Skipped (77) without nvim | `cli/tests/nvim_test.cpp`, `maic.nvim/tests/maic_test.lua` |
| `cli_smoke` | the real binary headless against a fake OpenAI-compatible server (a `--json` run's engine connection recorded and passed through `maic protocol check`), and `maic setup` off a terminal; `maic --rpc`, and `maic daemon` in a throwaway runtime directory (its life, clients leaving, a crash and a restart over the stale socket, its recorded connections checked; every daemon it starts is killed at the end) | `tests/cli_smoke.py` |
| `tui` | the real binary in a pty, read back with pyte; every case's engine connection recorded and checked | `tests/test_tui.py`, `tests/tui_driver.py` |
| `lint_includes` | cpp-httplib only through `maic/http.hpp` | `tests/lint_includes.py` |
| `comfy_node`, `workflow_edit`, `storyboard`, `danbooru_tags` | the ComfyUI node and the helper CLIs | `vendor/comfyui-maic-llamacpp`, `tools/comfyui` |
| `cai_tools` | cai-tools' own suites as written plus `test_maic.py`, through MAIC's runner; the network stays off (below) | `tools/cai/run_tests.py`, `tools/cai/tests/` |
| `diction` | diction against a fake whisper-server and scribe, WAV files for the mic | `diction/test_diction.py` |

### The network

The gate makes no outbound connection. The only suites that would are three of cai's, which reach SOPIA's published repository through a mirror (`git clone --bare` or `git fetch` over ssh into `~/.local/state/cai/mirror/SOPIA.git`, `tools/cai/src/cai/remote.py`) once they lift `CAI_NO_REMOTE`:

* `test_remote.py`: every remote check (`dstore.load()` with the remote on, `remote.read()`). Skipped.
* `test_sync.py`: the cases after it pops `CAI_NO_REMOTE` ("without it, SOPIA answers", `cli.main([])`, `cli.main(["list"])`). Runs.
* `test_notation.py`: the cases after it pops `CAI_NO_REMOTE` (`cli.main([])`, `lookup`, `audit`). Runs.

`tools/cai/run_tests.py` skips `test_remote.py` and runs every suite with `GIT_ALLOW_PROTOCOL=file`, so git refuses an ssh or https transport before it connects and the remote cases of the other two take cai's unreachable-remote path (an existing mirror as it stands, else the local tiers). cai's test files are unchanged. `MAIC_NETWORK_TESTS=1 scripts/check.sh` (or `ctest -R cai_tools` with it set) runs all of them with the network as written. The TUI suite, when python3 has no pyte, re-runs itself under `uv run --offline --with pyte`, so pyte comes from uv's cache (without `--offline` uv asks PyPI on most runs); with nothing cached it is skipped (77) unless `MAIC_NETWORK_TESTS=1` lets uv fetch it.

A single suite: `build/core/agent_test`, or `ctest --preset default -R agent`. Every C++ suite prints one line per check and exits with the failure count (`core/tests/check.hpp`).

## The protocol conformance tests

The engine protocol's checks ([design/engine-protocol.md](design/engine-protocol.md#15-schemas-the-ordering-machine-and-conformance)) are one checker used three ways: the engine runs it on every event it sends (the `guarded` tier logs a fault to `<state>/engine/protocol.log`), the tests run it on every exchange they drive, and `maic protocol check` runs it on recorded streams.

`protocol_conformance` drives the in-process engine through `Engine::call` with test clients, a `FakeServer` behind it (`core/tests/fake_server.hpp`, shared with `agent_test`; `unique_call_ids` gives each tool call its own id, as real providers do). Each client's messages, both ways, are recorded as `{"dir", "conn", "msg"}` lines and fed as they happen to `protocol::Conformance` (schemas, `schema_undeclared`, sequence numbers, the machines of `ordering.json`, every request answered once) and to its OpenAI-only twin (every `maic.*` event and `maic` object removed, then OpenAI's own event schemas and the response, item and content part machines). The scenarios: the hello and the dispatcher's refusals; a plain reply; a `run_shell` call with its output streamed and an approval; `cancelResponse` mid-stream (a held `FakeServer` reply); a disconnect and resume with `starting_after`, `maic_resync` for another load and for a number that left a small ring; two clients on one session, the local answer first and the remote one `maic_already_answered`, `always` refused from remote; a remote `response.steer` into a local turn in auto mode, after which the response ends steered and its successor, remote, asks the next command as remote; steering: the lane (a queued turn, one taken off it, steers refused for it and for ended or unknown responses), `drop` with its trim, `interrupt` and a resume, `keep`, `halt` with a waiting steer failed, `further` waiting on an approval and a `steer` withdrawing it, and an action a remote client is not allowed (`maic_steer_disabled`); a slow consumer that reads nothing during 6 MB of output and gets skip counts, never a gap; an in-process host's session (an engine of its own): `open_local`, the title after the first turn, `:` commands through `maic.session.command` with a question asked and answered by id, `maic.session.set` refusing auto under a dumb harness without `confirm`, `!cmd` streaming with no `output_index` and reaching the model as context, `maic.approval.proposed`, `maic.now`, a ban entry's `drop` steer and one that escalates to `halt` past its retries, and a remote client refused the local-only commands, questions and `!cmd`; the session index and its file; a driven run (seed 20261002, fourteen turns of messages, approvals answered by either client, cancels, mid-turn input and steers of random actions mid-reply and at an approval, a pause resumed, kept or cancelled); history over a transcript written for the test (60 exchanges, an attached file, a title, compactions, skeleton and bookkeeping lines): `attach`'s last three exchanges with ids `<file id>#<line>`, `listConversationItems` paging back to the first record in 25-exchange pages, by `maic.exchanges` and forward by `limit`, an attached file collapsed for a local client and whole at `collapse_over` 0, a command's output collapsed to three lines for a remote one, `maic.item.expand` reassembling the file from parts cut on characters, `maic_not_found`, a fork showing its parent's records under the parent's ids, a turn's records read on the next request, and `inflight` while a command runs; several sessions in one engine (two turns at once, the second completing while the first's reply is held, `unseen`, focus and `leave`, a fork naming its parent's epoch, an idle session parked by the default leave, `maic_busy` and a park with `interrupt` keeping a queued message that the resume runs, stop, a parked entry stopped, focus across disconnects, and a mutation that strips a reload's epoch); leaving (each case of the `leave` table in its own workspace: a session left working parked by `after` once its work is done, `maic_leave_ask` changing nothing, `stop` and `park` for a switch, a verb overriding the case, a background task parked when its job is done, `maic.session.leave` stopping an idle session, leaving a working one working until `after`, `--park` and `quit.working = stop`, and without `keeps_sessions` a working session parked and a background one stopped as each one's `no_daemon` says); shutdown leaving the session parked and a new engine resuming it. At the end `protocol.log` must hold no event or result fault from the engine's own checks.

The recordings go to `build/protocol-streams/<scenario>.jsonl`. Then the plain reply, tool call and steering recordings are mutated, and each mutation that breaks a rule must be caught with that rule's id: an event dropped or renumbered (`seq.next`), duplicated (`seq.repeat`), two swapped (`machine`), a type changed or a required field removed (`schema.event`), a field added beside OpenAI's (`schema.extra`), an event moved to another stream (`stream.id`), the stream cut before an answer (`request.answered`), a second response opened inside the first (`response.one_open`), output before its approval was answered (`machine`), a steer accepted after its turn ended (`steer.in_turn`), a pause while a response is open (`turn.paused`), a steer applied twice (`machine`); a changed delta text and a field added inside `maic` break nothing and must pass. `protocol_check` and `protocol_check_openai` then run the binary's `maic protocol check [--openai] build/protocol-streams` over the same files.

By hand, on any recorded stream: `maic protocol check FILE|DIR...` prints `ok` or the first violation per stream with its `sequence_number`, rule id and rule text, and exits 1 on a violation.

## Flake rules

The agent tests flaked under a parallel build and, it turned out, whenever two copies ran at once. What was wrong, and the rules that came out of it:

* **A test owns everything it touches.** `agent_test` used `/tmp/maic-agent-test` as its workspace and the real `~/.local/state/maic` for its sessions, so two runs (another worktree's `ctest`, a release build's) wiped each other's files mid-test, saw each other's sessions in `list_sessions`, and made `find_session` prefixes ambiguous. Every run now uses `/tmp/maic-agent-test-<pid>` with `XDG_STATE_HOME` under it. The same applies to any new suite: a workspace and a state directory per process, never the user's, never a fixed name. The ~180 `agent-test` transcripts the old test left in `~/.local/state/maic/sessions/general/` are litter, safe to delete.
* **Never time a move against a stream.** The cancel and deliver-now tests used to sleep 250 ms and hope the fake server was still streaming (it sent four characters every 150 to 200 ms). Under load either side could win. `FakeServer` now has `hold_left`: the next reply writes its first chunk and then idles with keepalive chunks until the agent hangs up, and `wait_streaming(n)` blocks until the n-th reply has started. The test waits for the stream, acts, and the stream ends because the agent closed the connection, not because a timer ran out. A held stream gives up after ten seconds so a broken cancel fails instead of hanging.
* **Synchronise with the fake, not with the clock.** When a test needs "the request has started" or "the stream has finished", the fake exposes it (a counter under a mutex and a condition variable). `delay_ms` is gone; nothing in the suite sleeps to wait for the agent.
* **Time bounds are a last resort, and wide.** The cancel test still asserts the turn ended within five seconds, which only says it did not wait for the ten-second hold. Anything tighter would be a timing test again.
* **Prove it under load.** The suite is considered deterministic when `agent_test` passes twenty times in a row under `nice -n 19` with a clean build running beside it, and when two copies run at once. That was done for this version; do it again after touching `FakeServer` or the agent loop:

  ```sh
  cmake --build --preset asan --clean-first -j &      # the load
  for i in $(seq 20); do nice -n 19 build/core/agent_test > /dev/null || echo "run $i failed"; done
  ```

## The pty harness

`tests/tui_driver.py` forks a program under a pseudo-terminal of a fixed size (120x40 in the tests), writes keys to it, and feeds everything it prints to a [pyte](https://github.com/selectel/pyte) screen, so a test asserts on what a person would see. The program runs under a tiny job-control shell inside the pty: forked straight into the pty it would be an orphaned process group, and Linux discards the stop action of SIGTSTP for those, so Ctrl-Z could never be tested. The stub reports when the job stops and resumes it on request, like `fg`.

Key notation, literal text plus:

| token | sends |
| :--- | :--- |
| `<esc>` `<cr>` `<tab>` `<s-tab>` `<bs>` `<space>` `<lt>` | Escape, Enter, Tab, Shift-Tab, Backspace, a space, a literal `<` |
| `<c-x>` | Ctrl plus a letter: `<c-w>`, `<c-p>`, `<c-z>` |
| `<m-x>` `<m-cr>` | Alt plus a key, as an ESC prefix; `<m-cr>` is Alt+Enter, which sends the input |
| `<up>` `<down>` `<left>` `<right>` `<home>` `<end>` | cursor keys |
| `<wheel-up>` `<wheel-down>` | SGR mouse wheel at the middle of the screen |
| `<wait>` | let the screen settle before the next key |

`send()` settles afterwards (nothing arrived for a quarter second); `wait_for(text)` pumps until the text is on the screen; `suspend()` is Ctrl-Z through the line discipline and returns once the job has stopped; `resume()` is `fg`; `wait_exit()` drains the last output (the transcript line maic prints after the screen is restored) into `raw`. By hand:

```sh
uv run --with pyte tests/tui_driver.py build/cli/maic -- 'ihello<esc>' ':help<cr>' ':q<cr>'
```

`tests/test_tui.py` starts the fake server from `cli_smoke.py` in its own process, gives maic a throwaway home (`XDG_*` under a temp dir, `--no-record`, `--harness dumb`, `--no-instructions`) and checks the welcome screen and status strip, typing and sending (`:w` and Alt+Enter), `:help`, `:path`, `:model`, Shift-Tab cycling the mode, Ctrl-Z and `fg`, and `:q` printing the transcript line. It needs pyte: run through `uv run --with pyte`, or plain `python3`, which re-runs itself under `uv` when pyte is missing. With neither pyte nor `uv` it exits 77, which ctest reports as a skip (`SKIP_RETURN_CODE`), never as a failure. pyte is a test tool only, nothing in MAIC links or ships it.

The TUI is an in-process client of the engine (engine protocol step 6), and the suite holds it to the protocol in every case: the class environment sets `MAIC_PROTOCOL_RECORD` to a directory, so each maic records its connection there (`tui-<pid>.jsonl`, the records `maic protocol check` reads, written one line per message and checked as they are written), and a cleanup registered first, so it runs after every maic of the case is gone, fails the case when `maic protocol check` finds a violation in those files or when the engine's guarded tier logged a fault (`protocol.log` anywhere under the throwaway home); then it clears both for the next case. `test_the_session_stream_passes_the_protocol_check` drives one stream through a tool call and its approval, a `!cmd`, `:rename` and `:q`, and checks the file on its own. By hand: `MAIC_PROTOCOL_RECORD=DIR maic` (or `maic -p`, which writes `headless-<pid>.jsonl`), then `maic protocol check DIR`.

## The fuzzer

`core/tests/fuzz_parsers.cpp` is a seeded mutation loop, no libFuzzer: for each target it takes an entry from a corpus, applies one to four random edits (a byte flipped, dropped, duplicated or inserted; a JSON or markdown fragment inserted; a slice of another entry spliced in) and feeds it to the real code. Nothing may crash; two targets also check invariants. `FUZZ_SECONDS` (default 2) is the time per target, `FUZZ_SEED` repeats a run; the seed is printed first so a failure can be replayed.

| target | corpus | what runs | invariants |
| :--- | :--- | :--- | :--- |
| openai SSE | chunk, reasoning, tool-call fragments, usage, errors, `[DONE]`, comments, junk | `chat()` through a local httplib server, so `LineSplitter` and the whole client path | no crash |
| anthropic SSE | `message_start` to `message_stop`, text, thinking and tool-use blocks, `input_json_delta`, errors, `ping` | the same, on `/v1/messages` | no crash |
| session and import JSONL | every record type `session.cpp` writes, Claude Code transcript lines, non-objects | `load_session`, `find_session` on a path, `read_import` | no crash |
| markdown | headings, lists, fences, quotes, rules, inline markup, unclosed markup, wide characters | `markdown_lines`, `plain_lines`, `wrap_line` at random widths | the lines joined with newlines are the input byte for byte; every wrapped row fits the width; wrapping drops nothing but the spaces it breaks at |
| ban filter | inputs built from a small vocabulary, bans drawn from it, valid and broken regexes, random chunking, both modes | `BanFilter` | `clean()` equals what `feed` and `flush` returned; the shown text never contains a banned string; in cut mode the shown text is a prefix of the reply and the hit sits where the cut is (or outgrew the window); no regex match the window could hold survives in verbatim text |

The SSE targets run eight calls at a time because every `chat()` ends with up to 100 ms of watcher wake-up in `stream_post`.

What it found on its first runs, both fixed in `core/src/bans.cpp` with regression checks in `robustness_test`: when a regex cut and a literal ban fell in the same chunk, the literal ban was reported as the hit although the regex had cut earlier, so the model was nudged about the wrong phrase; and `^` and `$` matched at the edges of whatever the filter held rather than at the reply's edges (a literal cut also counted as the end), so `^the` fired in the middle of a reply and `wide$` fired before a cut. A regex that matches the empty string (`i?`, `.*`) bans nothing, since the leftmost match is always empty: a limitation, not a bug, and the fuzzer skips those.

## The asan preset

`cmake --preset asan` configures `build-asan/` as a Debug build with `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`. `scripts/check-asan.sh` builds it, runs the whole ctest suite there (the in-process suites, `cli_smoke` on the asan binary, `tui` on the asan binary), then the fuzzer for longer. Leak detection is off (`ASAN_OPTIONS=detect_leaks=0`): LuaJIT and FTXUI keep allocations for the life of the process. The asan build is also a convenient load generator for the flake loop above.
