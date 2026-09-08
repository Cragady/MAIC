# STT-TTS Stack

## Prompt - Search

stt stack

## Response

An **STT (Speech-to-Text) stack** is the core component or pipeline layer that converts spoken audio into written text, typically functioning as the "ears" in a modern real-time voice AI architecture (`STT → LLM → TTS`). [1, 2, 3, 4]

A breakdown of how real-time voice components work together in an STT pipeline:

### Core Layers of an STT/Voice Stack

* **Voice Activity Detection (VAD):** Detects when a user starts and stops speaking to manage turn-taking.
* **Speech-to-Text (STT/ASR):** Transcribes the incoming audio stream into text tokens with punctuation and formatting.
* **Orchestration / LLM Layer:** Receives the text transcription to reason, process context, and formulate a reply.
* **Text-to-Speech (TTS):** Converts the generated response text back into natural-sounding audio. [5]

### Popular STT Providers & Models

* **Deepgram (e.g., Nova-3 / Flux):** Industry leader optimized for ultra-low latency and real-time conversational agents.
* **AssemblyAI (e.g., Universal-3 Pro):** High accuracy with strong performance on messy speech and built-in turn detection.
* **OpenAI Whisper:** Popular open-source multilingual transcription model used for self-hosted or API implementations.
* **Sensory:** Edge-focused, private, on-device STT deployment for low-connectivity or high-privacy environments. [6, 7, 8, 9]

Are you looking to build a **self-hosted local stack** (like Whisper + Python), or do you need help choosing a **cloud API provider** for real-time latency?

[1] [https://community.asterisk.org](https://community.asterisk.org/t/fully-self-hosted-voice-ai-the-whole-stt-llm-tts-stack-on-your-own-nvidia-gpu-works-with-asterisk/113523)
[2] [https://www.retellai.com](https://www.retellai.com/blog/how-real-time-voice-ai-works-stt-llm-tts)
[3] [https://docs.retellai.com](https://docs.retellai.com/general/orchestration_overview)
[4] [https://www.assemblyai.com](https://www.assemblyai.com/blog/the-voice-ai-stack-for-building-agents)
[5] [https://www.youtube.com](https://www.youtube.com/watch?v=SPB2T-eLrOg)
[6] [https://hamming.ai](https://hamming.ai/resources/best-voice-agent-stack)
[7] [https://gradium.ai](https://gradium.ai/content/best-speech-apis-2026)
[8] [https://www.assemblyai.com](https://www.assemblyai.com/blog/choosing-a-stt-api-for-voice-agents)
[9] [https://sensory.com](https://sensory.com/product/speech-to-text/)

## Response - Separate Window



## From Reddit:


Go to SelfHostedAI
r/SelfHostedAI
•
26d ago
Key_Advantage1424
What STT/LLM/TTS stack are you using for production voice agents right now?

Curious what people are actually running in production for AI voice agents.

Not demo videos. Not “it worked once on a browser mic.”\
Actual calls, real users, interruptions, bad mics, background noise, CRM/tool calls, etc.

The stack I keep seeing is something like:

- Twilio / Telnyx / LiveKit for audio

- Deepgram / AssemblyAI / Whisper / Smallest AI Pulse / Speechmatics for STT

- OpenAI / Claude / Gemini for the brain

- ElevenLabs / Cartesia / PlayHT / Deepgram Aura for TTS

- Vapi / Retell / Pipecat / LiveKit Agents if not building orchestration yourself

The thing I’m struggling with is where to optimize first.

Everyone says “use a faster LLM,” but in my tests the awkward delay often starts before the LLM even gets a good transcript.

My current logging plan:

- user starts speaking

- first STT partial

- final STT transcript

- LLM first token

- tool call time

- TTS first audio

- audio starts playing

- barge-in detected

- agent stops speaking

For STT specifically, I’m looking at Deepgram, AssemblyAI, Smallest AI Pulse, Speechmatics, Soniox and OpenAI realtime/transcribe models.

What’s working for you right now? And where are you hitting walls?


## From Reddit:



Go to LocalLLaMA
r/LocalLLaMA
•
1y ago
wombweed
Open source stack for real time STT-TTS responses?
Question | Help

Currently running Open Web UI with Whisper for TTS and STT. It is serviceable but the response time is very slow. Is there a different set of software I can use to get a more responsive UX, maybe closer to say, Siri?



## From Reddit:


Pipecat, Unmute by Kyutai

