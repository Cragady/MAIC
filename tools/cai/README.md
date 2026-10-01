# cai-tools

Claude-specific tooling. One entry point on PATH.

```
cai trans-fairy …    rebuild a chat export into a resumable transcript
cai redact …         remove credential material from a transcript
cai                  list installed tools, and name known ones that are not
```

Subcommands are discovered, not hardcoded. A tool registers itself under the `cai.tools` entry-point group from its own distribution:

```toml
[project.entry-points."cai.tools"]
diction = "diction.cli:main"
```

So `cai` stays one command on PATH while each tool installs independently with its own dependencies. A tool needing CUDA never drags it into one that needs nothing. The two built-ins register through the same mechanism an external package would use.

Exit codes: `0` ok, `2` no such tool, `3` not implemented, `4` known tool not installed.

## Install

Run these from the root of a clone. Everything here installs with [`uv`](https://docs.astral.sh/uv/). If you do not have it:

```
curl -LsSf https://astral.sh/uv/install.sh | sh          # macOS / Linux
powershell -c "irm https://astral.sh/uv/install.ps1 | iex"  # Windows
```

Other routes — Homebrew, pipx, pip — are on the [installation page](https://docs.astral.sh/uv/getting-started/installation/). Nothing below needs anything else.

**Just `cai`, no dependencies:**

```
uv tool install --editable .
```

One `cai` on PATH, in an isolated environment, editable so changes are live.

**`cai` with diction:**

```
uv tool install --editable . --with ./diction
```

`--with` puts diction into the same environment, which is what lets `cai` find it — a plugin installed as its own separate tool would be invisible, because `uv tool install` isolates each tool. So diction's dependencies arrive only when asked for.

Expect this one to be slow and large: it pulls Whisper and the CUDA runtime wheels, and the speech model itself downloads on first run to `~/.cache/huggingface`.

**diction on its own, without `cai`:**

```
uv tool install --editable ./diction
```

Puts `diction` on PATH directly. `cai diction` and `diction` run the same code.

**Check it worked:**

```
cai
```

Lists what is installed, and names known tools that are not.

## Usage — trans-fairy

Move one claude.ai conversation into a resumable Claude Code session. `--target-cwd` is the directory the resumed session will belong to; it is named, never written.

```
cai trans-fairy --target-cwd ~/my-project init
cai trans-fairy --target-cwd ~/my-project split --export conversations.json --target-uuid <UUID>
cai trans-fairy --target-cwd ~/my-project build
cai trans-fairy --target-cwd ~/my-project install
# then run the printed command:  cd ~/my-project && claude -r <SID> --fork-session
```

`install` never overwrites an existing transcript — it only ever creates. `--fork-session` on resume leaves the built transcript pristine.

The convenience wrapper runs the whole non-agent flow:

```
bin/run-all.sh conversations.json <UUID> --target-cwd ~/my-project
```

For an agent driving the tool, `--agent` runs one stage per invocation and prints the exact next command, so the agent advances by re-invoking rather than by answering a prompt:

```
cai trans-fairy --agent --target-cwd ~/my-project init      # prints the next command
```

Recovering a working context out of a long session — cut a transcript back to a clean prefix and resume there:

```
cp ~/.claude/projects/<slug>/<session>.jsonl /tmp/source-copy.jsonl   # the live one is still appending
cai trans-fairy --target-cwd ~/my-project split --truncate /tmp/source-copy.jsonl --at-line 1231
cai trans-fairy --target-cwd ~/my-project install --staged <staged path from the previous step>
cd ~/my-project && claude -r <new session id>
```

`--at-uuid` and `--before-text` are the other cut points; `--before-text` is the one to reach for when you know the phrase but not the index. **Tail only** — the prefix is kept verbatim, nothing past the cut survives, and the source is never modified. Resume **without** `--fork-session` here: the original transcript is the pristine copy, so forking would only add a third file.

**On an id collision** — which happens when a rebuild reuses the same pool position — `install` refuses and offers `--reflow`, which mints a fresh id and installs beside the existing transcript rather than over it. The reflow carries a lineage record naming what it came from, which a native `--fork-session` does not.

Placing material rather than migrating: `install --graft-onto <SID>` attaches the staged transcript onto an existing base, and `install --inject` places a repo-read or understanding on top (loud marks it in a meta record + a sidecar; silent is for testing and is never installed in a projects dir). Full reference: `cai trans-fairy --man-help`, and the design map at [`docs/transfairy/README.md`](docs/transfairy/README.md).

## Why diction is a separate package

`cai` itself has no dependencies. diction needs Whisper, CTranslate2 and the CUDA runtime wheels — a few gigabytes, plus a model download on first run.

Keeping them separate means installing `cai` costs nothing, and diction's weight arrives only for someone who asks for it with `--with`. The two are joined by entry-point registration rather than by being one package, so a light tool never inherits a heavy tool's dependency chain.

Any future tool follows the same rule: if it needs more than the standard library, it is its own distribution in this repository rather than another dependency on `cai`.

## Layout

```
src/cai/grammar/     the shared record grammar -- both tools import it
src/cai/transfairy/  the tool: init/split/build/install/graft/inject/--agent
src/cai/redact/      the tool: scrub + verify a transcript
docs/                specification per tool; PROBES.md holds measured client behaviour,
                     isolated-harness.md the method for running transcripts safely
reference/           prior implementations, retained for their field shapes
tests/               known-answer fixtures with negative controls
```

## State

`trans-fairy` and `redact` are both implemented (`src/cai/transfairy/`, `src/cai/redact/`), each with tests. `docs/` holds the specifications, `reference/` holds the earlier working code the implementations were built from.

`src/cai/grammar/` is implemented and tested. It exists because the record grammar was previously known in two places at once, which produced a defect: a redaction routine that handled a payload key as a string and skipped the same key when it held an object. Holding it once is the point of this package.

```
python3 tests/test_grammar.py
```

Four checks, one of them a negative control asserting a shape-blind reader still gets the wrong answer.
