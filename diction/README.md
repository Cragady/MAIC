# diction

Narrate out loud; get it written down, cleaned up, and correctable by voice. Micaiah's dictation tool: the mic, `whisper-server` and a scribe model: Claude Haiku through the `claude` CLI by default, as always; Haiku through Anthropic's API with `--preset api`; or with `--preset local` a model on `llama-server`, so that nothing leaves the machine.

    maid diction            # or: cai diction
    maid help diction       # its flags

Setup, the modes, the voice commands and the tap gesture, the flags, the privacy notes and the GPU budget: [docs/diction.md](../docs/diction.md). Open items: [TODO.md](TODO.md).

Tests: `python3 -m unittest -v diction.test_diction` from the repository root (ctest `diction`).
