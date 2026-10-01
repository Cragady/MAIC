# diction

Narrate out loud; get it written down, cleaned up, and correctable by voice.

Continuous mic capture -> WebRTC VAD splits on pauses -> whisper.cpp's `whisper-server` (`distil-large-v3`, CUDA) transcribes each utterance -> a scribe model writes it into a markdown file: Claude Haiku through the `claude` CLI by default, as always, or Haiku through Anthropic's API, or a local model on `llama-server` (see Presets).

The three stages run concurrently, so speech is never dropped while an earlier utterance is still being transcribed or written up.

diction is Micaiah's tool. It lived in cai-tools with faster-whisper and a headless `claude` process as the scribe; it moved into MAIC on 2026-10-01 as the top-level `diction/` package, and transcription was rebuilt on whisper.cpp, with a local scribe on llama.cpp as an option. Everything else (the modes, the voice commands, the tap gesture, the files it writes, the flags) works as it did, and with nothing set it runs what it always ran: Claude Haiku through a `claude` process on your own login as the scribe, and `distil-large-v3` for speech. The text below is her README, carried over and updated where the engines changed.

## Three modes

    maic diction              transcript  (default) cleaned prose, in your words
    maic diction --steps      procedure   each utterance becomes a numbered step
    maic diction --no-agent   raw         transcription only, no cleanup at all

`cai diction ...` is the same program through the cai port's dispatcher, which imports `diction.cli:main` from this repository's `diction/`.

All three produce the same artifacts -- the document, the raw log, and `--session-log` / `--raw` where asked for. They differ only in what the scribe is asked to do.

**Transcript** is the default. Each utterance is written out as clean prose: disfluencies dropped, obvious mistranscriptions repaired, punctuation added, but your wording, register and vocabulary left alone. It is not summarised and not rewritten into instructions.

**Procedure** (`--steps`) turns each utterance into a terse imperative step, numbered. Good for SOPs.

**Raw** (`--no-agent`) writes only the verbatim log. No model, no request to a scribe, nothing rephrased.

Resuming an existing document keeps that document's own shape, whatever flag you pass -- a numbered procedure is never reflowed into prose.

## Use

    cd ~/notes/some-process
    maic diction                 # writes ./some-process.md, Ctrl-C to stop
    maic diction devices         # list mics with live/dead status
    maic help diction            # diction's own --help

`maic diction` hands its arguments to diction untouched and exits with diction's exit code.

## Presets

A preset names a scribe backend, a scribe and a whisper model together:

    maic diction                       # the default preset: as diction always ran
    maic diction --preset api          # the same Haiku, through Anthropic's API
    maic diction --preset local        # everything on this machine
    DICTION_PRESET=local-small maic diction
    maic diction presets               # each preset, and whether it is ready

| Preset | Backend | Scribe | Whisper | VRAM | The narration's text goes to |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `default` | `claude-cli` | `haiku` (Claude Haiku, the `claude` CLI's alias) | `distil-large-v3` | about 1.7 GB, whisper alone | Anthropic, through your `claude` login, as before; diction says so at start |
| `api` | `api` | `haiku-4.5` (Claude Haiku) | `distil-large-v3` | about 1.7 GB, whisper alone | Anthropic, through its API with `ANTHROPIC_API_KEY`; diction says so at start |
| `local` | `local` | `llamacpp-2/Qwen3.5-9B-Q4_K_M-text` (the 9B text entry, no image processing, on the side server at 8k) | `large-v3-turbo-q5_0` | about 7.3 GB: needs the card to itself | nowhere: it stays on this machine |
| `local-small` | `local` | `llamacpp-2/Qwen3.5-4B-Q4_K_M` (the 4B on the side server at 8k) | `large-v3-turbo-q5_0` | about 4.7 GB: fits beside a parked ComfyUI | nowhere: it stays on this machine |

The audio never leaves the machine under any preset. The side server's window is `context_2`, 8192 unless settings change it (`core/include/maic/settings.hpp`, and `${MAIC_CONTEXT_2}` in `services/llamacpp-2.json`), which is the 8k above. The VRAM figures are estimates in the manner of `maic gpu`.

**The three backends.**

* `claude-cli` is the scribe diction always had, ported from the original: a persistent headless `claude -p --input-format stream-json --output-format stream-json --verbose --system-prompt PROMPT --model NAME --no-session-persistence` per mode, started on first use in a neutral directory (`$XDG_STATE_HOME/maic/diction/agent-cwd`, beside the rest of MAIC's state) and in its own process group, so Ctrl-C leaves it alive to drain. It runs on your own `claude` login, so it needs no API key. `--agent-model` goes to `claude --model` unchanged (`haiku`, `sonnet`, `opus` or a full model name). Each message carries the newest 25 passages, as it always did; the process keeps its own conversation. On shutdown its input is closed and it is given 5 seconds to exit before it is terminated. Without `claude` on PATH diction stops before listening and names the presets that still work.
* `api` sends each utterance to a cloud provider's API: `--agent-model` goes through `maic model resolve`, where `haiku`, `sonnet` and `opus` mean `haiku-4.5`, `sonnet-5` and `opus-5.5`, and the key comes from the provider's variable (`ANTHROPIC_API_KEY`) or its key command.
* `local` sends it to an OpenAI-compatible MAIC server, llama-server on loopback, resolved the same way. A name that resolves to a cloud model is refused under it, so `local` always means the text stays here.

To switch the default's Haiku from your `claude` login to the API, take the whole preset, `--preset api`, or keep the preset and change only the backend, `--backend api` (or `DICTION_BACKEND=api`): the default's scribe `haiku` then maps to `haiku-4.5`.

Each field is chosen on its own, highest first:

    --backend / -m / --agent-model                      the flag
    DICTION_BACKEND / DICTION_MODEL / DICTION_AGENT_MODEL the environment
    the chosen preset                                   --preset NAME, else DICTION_PRESET
    the default preset

So `maic diction --preset local -m distil-large-v3` keeps the local scribe and takes the bigger whisper model, and with nothing set at all diction is Claude Haiku through `claude` and `distil-large-v3`. An unknown preset or backend name is an error that lists the ones there are.

**Your own presets** go in `~/.config/diction/config.toml`, each with any of `backend`, `scribe`, `whisper` and `note`:

    [presets.local]
    whisper = "distil-large-v3"         # the built-in local, with the bigger whisper model

    [presets.night]
    backend = "local"
    scribe = "qwen-4b"
    note = "the 4B wherever it is loaded"

A preset named like a built-in one overrides it field by field; a field a preset leaves out comes from `default`, the backend too, so a new preset for a local or API model names its `backend`.

**`diction presets`** lists every preset with its backend, scribe, whisper model and note, marks the chosen one, and checks each without a network call: whether the whisper file is under `<models_dir>/whisper/` (else `not installed: maic models install whisper-NAME`); for `claude-cli`, whether `claude` is on PATH (it is not run); for a local scribe, whether its GGUF is under `<models_dir>/llamacpp/` (else `maic models install qwen3.5-9b-text` or `qwen3.5-4b`) and whether its server answers on loopback (else `maic up llamacpp-2`); for an API scribe, only whether its key variable is set or a key command is configured.

## Normal and insert

Transcript mode has two editing modes, after vim.

**Normal** (`--mode normal`) parses commands out of the prose, acts on them, *and* keeps them in the transcript. A chunk is content unless it clearly opens with a marker:

    "correction ..."     "diction, ..."      "scratch that"
    "strike that"        "delete that"       "edit that"
    "read that back"     "what do I have so far"

Or it names a passage by number and asks for something to be done to it: "change row 22 to be verbatim", "delete row 24", "follow the instruction on row 33". That form needs no opening marker, because it is how commands actually get spoken. Merely mentioning a number is not enough -- "the invoice on row 12 is the one that bounced" stays content.

So "correction, it's the last sixty days not thirty" revises the passage that said thirty, and the command itself is recorded as a quoted passage:

    So you open the shared inbox and filter to unbilled invoices
    from the last sixty days.

    Then you copy the PO number out of the ERP into the subject line.

    > correction, it's the last sixty days not thirty

    Attach the signed PDF before sending.

The blockquote keeps spoken instructions distinguishable from dictation, both by eye and by grep, and they survive a reload as commands rather than prose. When the model is unsure whether a chunk is a command or content it treats it as content -- a stray sentence is trivial to fix, silently eaten dictation is not.

**Insert** (`--mode insert`, the default) is pure dictation and is lossless by construction. No command is parsed, so no marker can misfire, and earlier passages cannot be touched -- `apply_reply` refuses mutating directives outright rather than trusting the prompt. Anything the model declines to write down, by reading dictation as a command or skipping it as an aside, is appended verbatim instead. Everything you say ends up in the document.

### Switching mid-session

Voice has no Esc key. Every phrase you could pick is also a thing you might legitimately say, because speech only reaches a functional meaning through the communicative gate. So the switch is two independent gates in sequence:

    triple tap  +  "diction normal" | "diction insert" | "diction toggle"

The tap is detected on the raw PCM in the capture thread -- before VAD, before Whisper, before the scribe -- so it is an acoustic event, not language, and can never collide with dictation. It only arms a window; the phrase has to follow. Neither gate has to be strict on its own, which is why the tap detector is tuned for recall: a stray onset arms a window that expires harmlessly.

Calibrate the detector against your own desk:

    maic diction taptest

Tap three times, then deliberately type and talk at it. Every onset prints, and also goes to `<logdir>/taptest-<stamp>.log` with its amplitude and inter-onset gap. `--clean` narrows the terminal to completed taps only, for gauging the fire rate at a glance (`--visual` is kept as an alias).

Printing every onset is what makes the shell's own echo of your keystrokes useful: it interleaves with the onsets and labels which were typing. The log has no equivalent of that.

Three taps, not two: measured on a real run, pair spacings from typing and talking (0.14-0.44s) overlap deliberate taps (0.16-0.52s) completely, so no threshold on pair timing separates them. A third tap in the same rhythm is far rarer by accident.

What separates a deliberate tap from typing is not rhythm but what follows it. Tapping cadence varies between sessions -- measured at 0.4-0.8s once and 0.18-0.30s the next, the latter sitting exactly on top of typing -- so the gap window is deliberately wide and does no discrimination on its own.

`--tap-gap LO-HI`, `--tap-tail`, `--tap-quiet`, `--tap-count`, `--tap-ratio` and `--tap-floor` all tune it; `--no-taps` disables arming entirely.

Inside an armed window the wake word is not required at all -- the tap has already established intent -- and the verb is matched fuzzily. Both matter, because Whisper mangles the phrase badly: one measured session produced "Dixon normal", "Dixen Toggle", "Dekchin Tago", "Diction Tangle" and "Diction Togel" for the same spoken words. Matching only the verb, loosely, recovers all but the worst of those. The window stays armed for `--arm-window` seconds (default 20) or until a switch is recognised, so a garbled attempt is simply repeated rather than needing a fresh tap.

A tap does not print a log line. It raises a transient status line (the time of the tap, then "say normal / insert / toggle") which clears when a switch is recognised, or reports "tap expired" with "no switch phrase heard" if the window lapses. So the tap has three visible states, and none of them leave noise behind.

There is no trailing guard on the tap any more. It existed to reject triples that fall out of typing, but a false arm is already inert: it is a window that expires unless a spoken phrase lands in it, and nobody says "diction normal" mid-sentence. The guard duplicated the second gate and cost the natural gesture.

`--tone` plays a short tone on a switch (through `paplay`), rising into normal and falling into insert, for when you are not looking at the terminal. Independently, every line in the terminal and in the logs that was processed in normal mode carries a `~` mark, so the mode announces itself continuously rather than only at the switch.

Each mode runs its own scribe conversation, started on first use. One conversation told about both modes does not hold the line -- with the command markers listed in its own prompt it emits commands during insert turns regardless of how the mode is stated per turn.

`--steps` has its own protocol and ignores `--mode`; natural corrections ("no wait, actually...") are already part of describing a task out loud.

## The agent protocol

Each utterance is sent with the document so far.

A mode-switch phrase never reaches the scribe. Switching is handled in Python, so the scribe has no directive for it; an unarmed switch phrase is recognised strictly, reported, and dropped before the scribe sees it, with a prompt rule as backstop. The scribe replies with one directive per line:

    APPEND: <text>          new passage / step
    REVISE <n>: <text>      replaces passage / step n
    DETAIL <n>: <clause>    adds a caveat to step n        (--steps only)
    DROP <n>                removes passage / step n
    SHOW <n>                prints passage n itself
    RECAP <n>               prints the last n passages
    WRITE: <text>           composes, only when explicitly asked
    SKIP                    noise, asides, remarks about the recording

Python owns the file; the scribe only emits directives. It cannot clobber the document.

Because the scribe runs a few seconds behind the transcriber, each directive is printed tagged with the timestamp of the utterance that caused it. A skipped utterance prints a dim `- skipped`, so it is never ambiguous whether the scribe is still working. On resume the last few entries print automatically (`--recap N`).

### What the scribe is sent

The system prompts and the per-utterance message are the ones diction always sent: the document as numbered lines inside `<steps>`, then the chunk inside `<utterance>`. The `claude-cli` backend sends them exactly as before, the newest 25 passages to a `claude` process that keeps every turn itself. For the `api` and `local` backends what changed is the conversation around them: diction keeps it, and a local model has a 16k window (`context` in MAIC's settings; 8k on the side server), so each request is built to fit it:

* always the system prompt, and last the current message with the **whole document**, every passage under its real number, so "change row 12" means row 12 however long the session has run;
* then the most recent earlier exchanges, newest first, up to 8 and only while they fit; the oldest go first. An earlier exchange carries its utterance and the reply, not the document it saw at the time, which is stale and is already here in full;
* 1024 tokens are kept for the reply. Should the document alone outgrow the window, its oldest passages are left out and the message says which; should the server still answer that the request exceeds its context, the request is sent once more with no history.

Thinking is off for a local scribe (`chat_template_kwargs.enable_thinking=false`, `reasoning_effort: none`), and the temperature is 0.2.

## Options

    -o/--out FILE        output file (default: <cwd-name>.md)
    -t/--title TITLE     document title
    -d/--device SRC      mic source (default: auto, falls back if default is dead)
    --preset NAME        a backend, a scribe and a whisper model together
                         (default: default)
    --backend NAME       claude-cli, api or local (default: the preset's, claude-cli)
    -m/--model NAME      whisper ggml model (default: the preset's, distil-large-v3;
                         current is whatever whisper-server loaded)
    --agent-model NAME   scribe: for claude-cli what claude --model takes; for api
                         and local a MAIC preset or provider/model (default: the
                         preset's, haiku)
    --silence MS         pause length that ends an utterance (default: 700)
    --aggressiveness 0-3 VAD strictness; raise in a noisy room (default: 2)
    --min-utterance MS   drop blips shorter than this (default: 200) -- raise if
                         room noise creates junk steps, lower if short words
                         like "no" or "wait" get dropped
    --max-utterance S    force-split a long monologue (default: 30)
    --agent-timeout S    how long a scribe or whisper request may take (default: 90)
    --recap N            on resume, print the last N entries (default: 3, 0 off)
    --steps              numbered procedure instead of a transcript
    --no-agent           raw transcription only; no scribe, no cleanup
    --log-dir DIR        where session logs go
    --local-logs         force diction-logs/ next to the document, ignoring
                         any configured global log directory
    --hidden-logs        shorthand for --log-dir .h-diction-logs
    --session-log        also write the enriched turn-by-turn log
    --raw                also write the verbatim narration into the document

### The two flags whose meaning changed

| Flag | Before | Now |
| :--- | :--- | :--- |
| `-m/--model` (`DICTION_MODEL`) | a faster-whisper model name, `distil-large-v3` by default, downloaded on first use | a whisper.cpp ggml file: a path, or a name under `<models_dir>/whisper/` tried as `NAME`, `NAME.bin` and `ggml-NAME.bin` (so `distil-large-v3` still works when `ggml-distil-large-v3.bin` is there). Default: the preset's, `distil-large-v3` as before; it is not downloaded on first use any more, and a missing one says `maic models install whisper-NAME`. `current` means whatever `whisper-server` loaded at start, the file `maic vendor use whisper FILE` linked. Naming another file loads it into the running server (its `/load` route) before the mic opens; the server keeps it until it restarts or another `-m` swaps it |
| `--agent-model` (`DICTION_AGENT_MODEL`) | a `claude` model alias, `haiku` by default | with the `claude-cli` backend, unchanged: what `claude --model` takes, `haiku` by default. With `api` or `local`, a MAIC preset (`haiku-4.5`; `qwen-4b`; `qwen-9b`; any preset in settings) or `provider/model` (`llamacpp/Qwen3.5-4B-Q4_K_M`, `llamacpp-2/...`, `deepseek/deepseek-chat`), resolved by `maic model resolve NAME`, which prints the provider, kind, base URL, model and context as JSON; there `haiku`, `sonnet` and `opus` mean `haiku-4.5`, `sonnet-5` and `opus-5.5` |

**Dictation never evicts the main model.** A name without a provider (`qwen-4b`, or any other preset) that lands on the main llama server is sent to the side server `llamacpp-2` instead whenever that one answers, with the same model name and the side server's own window. With `maic up llamacpp-2` running, the session on 8081 keeps its model while you dictate. Without it, the scribe uses the main server, and a model other than the one it holds replaces it there. Written out as `llamacpp/MODEL`, the scribe stays on the main server on purpose.

A scribe on the `api` backend, the `api` preset's Claude Haiku or any Anthropic or OpenAI-compatible provider, takes its key from the environment variable the provider names (`ANTHROPIC_API_KEY` for Anthropic) or its key command; without one diction stops before listening and says which variable to set, or `--preset local`. With one, it prints one line at start naming the provider, see Privacy below.

## Raw alongside processed

`--raw` adds a `## Verbatim narration` section to the document, listing every utterance with what it became:

    1. Open the shared inbox in Outlook.
    2. Copy the PO number out of the ERP.

    ## Verbatim narration

    - **00:04** first open the shared inbox in outlook _(became step 1)_
    - **00:19** uh, hang on a sec _(skipped)_
    - **00:26** paste that into the subject line _(became step 3)_
    - **00:33** actually scratch that last step, delete it _(dropped step 3)_

So you can audit what the scribe discarded or reworded, not just what it kept. Once a document has that section it keeps being maintained on resume, with or without the flag -- narration already on disk is never silently dropped.

`--no-agent --raw` gives a plain timestamped transcript and no steps.

## Files written

Alongside the document, each session writes into `diction-logs/`, all prefixed with a slug of the document title:

    <slug>-raw-<stamp>.log       verbatim, only what was heard
    <slug>-scribe-<stamp>.log    what went wrong talking to the scribe: HTTP errors and what the server said

`--hidden-logs` puts them in `.h-diction-logs/` instead. The `h-` marks it as hidden right where the eye lands, so it cannot be mistaken at a glance for the visible `diction-logs/` -- a leading dot alone is too easy to skim past when the rest of the name is identical.

`--session-log` adds a third, `<slug>-session-<stamp>.log`, reading a turn at a time:

    [00:02] dropped   'Thank you.' (stock phrase, likely hallucinated)
    [00:04] operator  first open the shared inbox in outlook
            agent     APPEND: Open the shared inbox in Outlook.
            applied   became step 1
    [00:19] operator  uh, hang on a sec
            agent     SKIP
            applied   skipped

The scribe runs seconds behind the transcriber, so entries are buffered and emitted a whole turn at a time in the order spoken -- not in the order the two threads happen to finish.

## A global log directory

By default logs land next to the document being dictated. Set a global directory to collect every session in one place instead:

    # ~/.config/diction/config.toml
    log_dir = "~/diction-logs"

or per-shell, which takes precedence:

    export DICTION_LOG_DIR=~/diction-logs

Precedence, highest first:

    --log-dir DIR      explicit, wins over everything
    --hidden-logs      .h-diction-logs/ next to the document
    --local-logs       diction-logs/ next to the document
    DICTION_LOG_DIR    environment
    log_dir            ~/.config/diction/config.toml
    (default)          diction-logs/ next to the document

`--local-logs` and `--hidden-logs` both mean "next to the document", so they override a configured global directory -- that is what they are for once one exists. When the directory comes from a global setting, diction prints where it is at startup, so it is never a surprise.

Filenames carry both the document slug and a timestamp, so a single global directory holds many documents and many sessions without collisions.

MAIC lists the directory as the artifact `diction/logs`: `maic artifacts` shows its size, `maic path diction/logs` prints it, `maic open diction/logs` opens it. It resolves the way diction does without flags: `DICTION_LOG_DIR`, then `log_dir` in the config file, then `diction-logs/` in the current directory.

## Setup

```sh
maic vendor add whisper          # builds the pinned whisper.cpp out of tree (CUDA when nvcc is found); no download
maic vendor model whisper https://huggingface.co/distil-whisper/distil-large-v3-ggml/resolve/main/ggml-distil-large-v3.bin \
    2883a11b90fb10ed592d826edeaee7d2929bf1ab985109fe9e1e7b4d2b69a298
maic vendor model whisper https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin \
    2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987
maic up whisper                  # whisper-server on 127.0.0.1:8083
maic diction                     # the default: Claude Haiku through `claude`, logged in as you

export ANTHROPIC_API_KEY=...     # or Haiku through the API
maic diction --preset api

maic up llamacpp-2               # or, for --preset local or local-small: the scribe's server
maic diction --preset local
```

`maic diction presets` says what each preset still needs.

The models, with their SHA-256 as Hugging Face's API gives them (`?blobs=true` on the tree listing):

| File | Size | SHA-256 | What |
| :--- | :--- | :--- | :--- |
| [`ggml-distil-large-v3.bin`](https://huggingface.co/distil-whisper/distil-large-v3-ggml/resolve/main/ggml-distil-large-v3.bin) (distil-whisper/distil-large-v3-ggml) | 1.42 GB | `2883a11b90fb10ed592d826edeaee7d2929bf1ab985109fe9e1e7b4d2b69a298` | the model diction always used, English, fp16 |
| [`ggml-large-v3-turbo-q5_0.bin`](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin) (ggerganov/whisper.cpp) | 0.53 GB | `394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2` | the small-VRAM choice: large-v3-turbo quantized to 5 bits, about a third of the memory |
| [`ggml-silero-v6.2.0.bin`](https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin) (ggml-org/whisper-vad) | 0.9 MB | `2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987` | Silero VAD, required by the service |

`maic vendor model whisper URL SHA256` downloads into `<models_dir>/whisper/`, refuses to keep a file whose hash does not match, and links a speech model as `current.bin`; the Silero file is kept under its own name, which `services/whisper.json` loads. A file you already have: put it under `<models_dir>/whisper/` and `maic vendor use whisper FILE`. `current.bin` is what `whisper-server` loads when it starts; each session then asks for the preset's model (or `-m`'s, or `DICTION_MODEL`'s) and loads it into the running server when it holds another, so it pays to link the one you use most: `distil-large-v3` for the default preset, `large-v3-turbo-q5_0` for the local ones. `-m current` takes whatever the server holds.

**The Silero VAD is the second gate.** WebRTC VAD opens utterances on breathing and room noise, and Whisper reliably hallucinates stock phrases into that silence, with `no_speech_prob` as low as 0.10, so the usual confidence thresholds do not catch it. faster-whisper ran Silero on every utterance (`vad_filter=True`); `whisper-server` does the same with `--vad`, using faster-whisper's settings (no minimum speech length, 2 s minimum silence, 400 ms padding). The stock-phrase list in diction stays as the last gate.

A local scribe's model is a GGUF under `<models_dir>/llamacpp/`, as for MAIC itself ([llamacpp.md](llamacpp.md)): `local` takes the 9B's text entry `Qwen3.5-9B-Q4_K_M-text` (a folder with a link to the weights and no projector, as llamacpp.md describes), `local-small` takes `Qwen3.5-4B-Q4_K_M`, and `--agent-model qwen-4b` is the 4B too.

## The card: an 8 GB GPU

`maic gpu` lists the whisper server beside the llama servers and ComfyUI, and its budget sentence counts whisper's model (its file plus about 300 MB of buffers, labelled an estimate). On an 8 GB card:

| Resident | Estimate |
| :--- | :--- |
| whisper, distil-large-v3 | 1.7 GB |
| whisper, large-v3-turbo q5_0 | 0.8 GB |
| scribe, the 4B on the side server at 8k (text only) | 3.0 GB |
| scribe, the 9B text entry on the side server at 8k | 6.5 GB |
| scribe, the 4B at 16k | 3.6 GB |

So whisper plus a 4B scribe is about 5 GB and fits with ComfyUI stopped. A third model does not: with the agent's own 4B at 16k on 8081 and the scribe's on 8082 as well, the estimate is 8.3 GB before each process's own CUDA overhead, so either let the scribe share the main server's model (`--agent-model llamacpp/Qwen3.5-4B-Q4_K_M` while that is the session's model, which evicts nothing) or take the turbo q5_0 model and lower `context_2`. `whisper-server` holds its model from the moment it starts; `maic down whisper` gives the memory back. It is marked `needs_gpu`, so `maic up whisper` first asks the llama servers to unload what they hold, as ComfyUI does; they reload on the next request.

## Privacy

**The audio never leaves the machine.** It goes to `whisper-server` on 127.0.0.1:8083, MAIC's own build of whisper.cpp without its downloader, bound to loopback. It never left before either (faster-whisper ran in-process).

**The text goes where the preset says.** The default preset is diction as it always ran: the scribe is Claude Haiku in a `claude` process, so every utterance's text goes to Anthropic. The `api` preset sends it to Anthropic too, through the API. Either way diction prints, before it starts listening, one line saying that the text of every utterance goes to Anthropic. The same holds for any cloud scribe you name (`--agent-model sonnet`, `anthropic/...`, `deepseek/...`, `openrouter/...`).

`--preset local` and `--preset local-small` keep the text here too: it goes to `llama-server` on 127.0.0.1:8082 (or 8081), MAIC's own build of llama.cpp without TLS, bound to loopback. With either, dictation never leaves the machine.

Nothing else reaches the network. `-m` resolves model files on disk; `maic vendor model` is the only download, and only when you type it. The one Python dependency, `webrtcvad-wheels`, is fetched by `uv` the first time the mic is used and then run from uv's cache with `--offline`, so a normal start makes no outbound connection.

## Engines, for reference

| | Endpoint | Sent | Read |
| :--- | :--- | :--- | :--- |
| transcription | `POST http://127.0.0.1:8083/inference` (whisper.cpp v1.9.4 `examples/server`), multipart | `file`: the utterance as a 16 kHz mono 16-bit WAV, built in Python, so `--convert` stays off; `temperature=0.0`, `response_format=json`, `language=en`, `beam_size=5` | `{"text": ...}`, one line per segment; the lines are joined with spaces, as faster-whisper's segments were |
| readiness | `GET /health` | | 200 once the model is loaded, 503 while it loads; the mic opens only after 200 |
| model swap (`-m`) | `POST /load`, multipart `model=<path>` | a file checked for the ggml header first (the server exits on a failed load) | |
| scribe | `POST <base_url>/chat/completions` (OpenAI-compatible; llama-server), SSE | `model`, `messages`, `stream: true`, `max_tokens: 1024`, `temperature: 0.2`, and for a local server `chat_template_kwargs: {enable_thinking: false}`, `reasoning_effort: "none"` | `choices[0].delta.content` until `[DONE]` |
| scribe, `api` backend on Anthropic | `POST https://api.anthropic.com/v1/messages` | the same messages, the system prompt as `system` | the text blocks |
| scribe, `claude-cli` backend | `claude -p --input-format stream-json --output-format stream-json --verbose --system-prompt PROMPT --model NAME --no-session-persistence`, one process per mode | one line per utterance: `{"type": "user", "message": {"role": "user", "content": [{"type": "text", "text": ...}]}}` | lines until `{"type": "result"}`: its `result`, or an error when `is_error` |
| `--agent-model` (`api`, `local`) | `maic model resolve NAME` | | `{"provider", "kind", "base_url", "model", "context", "remote", "api_key_env", "api_key_command"}`; never a key |

`DICTION_WHISPER_URL` moves the whisper endpoint (the tests point it at a fake).

## Notes on the shutdown path

The mic is not opened until `whisper-server` reports its model loaded, so the clock starts when we are genuinely listening -- a cold CUDA load can take a while and anything said during it would otherwise be captured against a misleading timestamp.

Ctrl-C stops the capture and drains: the transcriber finishes the utterances already cut and the scribe writes up whatever is still queued, up to `--agent-timeout` plus 15 seconds. A `claude-cli` scribe runs in its own process group, so the terminal's Ctrl-C does not reach it and it stays alive to drain; once the queue is empty its input is closed and it exits (terminated after 5 seconds if not). The `api` and `local` scribes are plain requests, with no process to protect. The recorder is terminated explicitly rather than waiting on its blocking read, which can stall indefinitely on a flaky USB device.

## Install, stable and beta

Needs a PipeWire/PulseAudio mic (`parecord`, and `paplay` for `--tone`), Python 3.12 or newer, and `uv` for the one dependency. A CUDA GPU is optional: the whisper.cpp and llama.cpp builds fall back to the CPU, slower.

MAIC installs diction with everything else: the package to `share/maic/diction/` and the launcher `maic-diction` beside `maic`. That copy is the stable one, and prints `diction (stable <tag>)`; a dev build of `maic` (no `maic-diction` beside it) runs the repository's `diction/` instead and prints `diction-beta (working tree <commit>)`, with `+` when the directory has uncommitted changes. That is the old `dist/` and `promote` split, made by MAIC's release (`scripts/release.sh` installs a tag) rather than by a copy script.

Dependencies, before and after:

| | |
| :--- | :--- |
| before | faster-whisper, ctranslate2, onnxruntime, av, numpy, huggingface-hub, hf-xet, tokenizers, nvidia-cublas/cudnn/nvrtc and their dependencies (28 pinned packages in a venv), the `claude` CLI |
| after | the Python standard library, the `claude` CLI for the default preset (as before; the `api` and `local` presets need none), and `webrtcvad-wheels==2.0.14` (`diction/requirements.txt`). When `python3` lacks it, diction runs itself under `uv run --python 3.13 --with-requirements diction/requirements.txt` (the package has wheels up to Python 3.13), so nothing is pip-installed anywhere. The tap detector and the device probe do their arithmetic with `array` and `math` |

## Layout

    diction/cli.py          main(): flags, logs, the three threads; `maic diction` and `cai diction` land here
    diction/audio.py        parecord capture with WebRTC VAD, the tap detector, device probing, the mode tone
    diction/pipeline.py     the transcriber stage: whisper, stock-phrase gate, mode switching
    diction/whisper.py      the whisper-server client
    diction/scribe.py       the system prompts, the chat client, the context budget
    diction/presets.py      the presets and backends, their precedence, `diction presets`
    diction/document.py     the document and apply_reply
    diction/journal.py      the raw and session logs
    diction/ui.py           colours and the status line
    diction/maic-diction    the launcher
    diction/test_diction.py ctest `diction`: fake whisper-server and scribe, WAV files instead of the mic
    diction/TODO.md         open items, Micaiah's
    diction/tools-reflow.py the one-sentence-per-line reflow for the markdown, with its frozen snapshot in sync-frozen/
