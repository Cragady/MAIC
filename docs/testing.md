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
| `agent` | the agent loop against `FakeServer`: turns, tools, approvals, compaction, subagents, cancel, resume | `core/tests/agent_test.cpp` |
| `session` | session files, homes, forks, import | `core/tests/session_test.cpp` |
| `lua_tools` | user-defined Lua tools | `core/tests/lua_tools_test.cpp` |
| `fuzz` | the parser fuzzer, 2 s per target by default | `core/tests/fuzz_parsers.cpp` |
| `server` | maic-server over HTTP | `server/tests/server_test.cpp` |
| `editor` | the vim input | `cli/tests/editor_test.cpp` |
| `cli_smoke` | the real binary headless against a fake OpenAI-compatible server, and `maic setup` off a terminal | `tests/cli_smoke.py` |
| `tui` | the real binary in a pty, read back with pyte | `tests/test_tui.py`, `tests/tui_driver.py` |
| `lint_includes` | cpp-httplib only through `maic/http.hpp` | `tests/lint_includes.py` |
| `comfy_node`, `workflow_edit`, `storyboard`, `danbooru_tags` | the ComfyUI node and the helper CLIs | `vendor/comfyui-maic-llamacpp`, `tools/comfyui` |

A single suite: `build/core/agent_test`, or `ctest --preset default -R agent`. Every C++ suite prints one line per check and exits with the failure count (`core/tests/check.hpp`).

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
