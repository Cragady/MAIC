"""MAIC's chat nodes for ComfyUI: talk to llama-server over its OpenAI-compatible endpoint.

Two nodes under "MAIC/llm". MaicLlmServer names the server (loopback, the one `maic up llamacpp` runs
on 127.0.0.1:8081) and MaicLlmChat sends one chat turn to it. The conversation is kept in this process
per session id, so the deep pass of a workflow can read what the quick pass said. Standard library only.
"""

import base64
import json
import struct
import urllib.error
import urllib.request
import zlib

# session_id -> [{"role": "user"|"assistant", "content": str}, ...]; lives until ComfyUI exits.
SESSIONS = {}

NOT_RUNNING = "llama.cpp is not running: maic up llamacpp"


class MaicLlmServer:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "base_url": ("STRING", {"default": "http://127.0.0.1:8081/v1", "tooltip": "The OpenAI-compatible root of a llama-server. MAIC's main server listens on 127.0.0.1:8081, its side server (llamacpp-2) on 8082."}),
                "model": ("STRING", {"default": "current", "tooltip": "The model field of each request. `current` is the alias services/llamacpp.json gives the linked GGUF; a single-model llama-server answers with that model whatever the name."}),
                "timeout": ("INT", {"default": 300, "min": 1, "max": 3600, "tooltip": "Seconds to wait for a reply."}),
            },
        }

    RETURN_TYPES = ("MAIC_LLM",)
    RETURN_NAMES = ("connection",)
    FUNCTION = "connect"
    CATEGORY = "MAIC/llm"
    DESCRIPTION = "A llama-server to chat with. One resident model per server: MAIC's main server is on 8081, its side server (maic up llamacpp-2) on 8082."

    def connect(self, base_url, model, timeout):
        return ({"base_url": base_url.rstrip("/"), "model": model, "timeout": timeout},)


class MaicLlmChat:
    @classmethod
    def INPUT_TYPES(cls):
        return {
            "required": {
                "connection": ("MAIC_LLM",),
                "system": ("STRING", {"multiline": True, "default": "", "tooltip": "System prompt, sent first on every turn."}),
                "prompt": ("STRING", {"multiline": True, "default": "", "tooltip": "This turn's user message."}),
                "think": ("BOOLEAN", {"default": False, "tooltip": "Let the model reason before answering (Qwen3.5 and other thinking models). The reasoning comes back on the thinking output, not in the response."}),
                "format": (["text", "json"], {"tooltip": "json makes the server constrain the reply to a JSON object."}),
                "temperature": ("FLOAT", {"default": 0.8, "min": 0.0, "max": 5.0, "step": 0.05}),
                "top_k": ("INT", {"default": 40, "min": 0, "max": 1000}),
                "top_p": ("FLOAT", {"default": 0.95, "min": 0.0, "max": 1.0, "step": 0.01}),
                "min_p": ("FLOAT", {"default": 0.05, "min": 0.0, "max": 1.0, "step": 0.01}),
                "seed": ("INT", {"default": -1, "min": -1, "max": 0xffffffff, "tooltip": "-1 lets the server pick."}),
                "extra_json": ("STRING", {"multiline": True, "default": "", "tooltip": "A JSON object merged into the request body last, so any llama-server field works: xtc_probability, dry_multiplier, grammar, json_schema, logit_bias, n_predict, samplers (docs/llamacpp.md)."}),
                "keep_context": ("BOOLEAN", {"default": True, "tooltip": "Remember this conversation (per session_id) until ComfyUI restarts, and send it with every turn."}),
                "session_id": ("STRING", {"default": "", "tooltip": "Which conversation. Empty means this node's own; connect another chat node's session_id output to share its conversation."}),
                "reset": ("BOOLEAN", {"default": False, "tooltip": "Forget the conversation before this turn."}),
            },
            "optional": {
                "images": ("IMAGE", {"tooltip": "Sent with the prompt as PNG. The server needs a vision model and --mmproj, or it rejects the request."}),
            },
            "hidden": {"unique_id": "UNIQUE_ID"},
        }

    RETURN_TYPES = ("STRING", "STRING", "STRING")
    RETURN_NAMES = ("response", "thinking", "session_id")
    FUNCTION = "chat"
    CATEGORY = "MAIC/llm"
    DESCRIPTION = "One chat turn against llama-server, with the conversation kept per session id."

    def chat(self, connection, system, prompt, think, format, temperature, top_k, top_p, min_p, seed, extra_json, keep_context, session_id, reset, images=None, unique_id=""):
        key = session_id or str(unique_id)
        if reset:
            SESSIONS.pop(key, None)
        history = SESSIONS.setdefault(key, []) if keep_context else []

        messages = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.extend(history)
        turn = {"role": "user", "content": prompt}
        if images is not None:
            turn = {"role": "user", "content": [{"type": "text", "text": prompt}] + [
                {"type": "image_url", "image_url": {"url": png_data_url(image)}} for image in images]}
        messages.append(turn)

        body = {
            "model": connection["model"],
            "messages": messages,
            "stream": False,
            "temperature": temperature,
            "top_k": top_k,
            "top_p": top_p,
            "min_p": min_p,
            "seed": seed,
            "chat_template_kwargs": {"enable_thinking": think},
            "reasoning_format": "deepseek",
        }
        if not think:
            # Both are llama-server's documented off switches; Ollama's /v1 only reads the second.
            body["reasoning_effort"] = "none"
        if format == "json":
            body["response_format"] = {"type": "json_object"}
        if extra_json.strip():
            extra = json.loads(extra_json)
            if not isinstance(extra, dict):
                raise ValueError("extra_json must be a JSON object")
            body.update(extra)

        message = post_chat(connection, body)
        response = message.get("content") or ""
        thinking = message.get("reasoning_content") or message.get("reasoning") or ""
        if keep_context:
            history.append({"role": "user", "content": prompt})
            history.append({"role": "assistant", "content": response})
        return (response, thinking, key)


def post_chat(connection, body):
    request = urllib.request.Request(connection["base_url"] + "/chat/completions", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=connection["timeout"]) as reply:
            data = json.load(reply)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"llama.cpp answered {e.code}: {e.read().decode(errors='replace')[:2000]}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"{NOT_RUNNING} ({connection['base_url']}: {e.reason})") from None
    return data["choices"][0]["message"]


def png_data_url(image):
    """One ComfyUI IMAGE frame, [H, W, C] floats in 0..1, as a data: URL of a PNG."""
    height, width, channels = image.shape
    pixels = bytes(image.mul(255).round().clamp(0, 255).byte().cpu().flatten().tolist())
    stride = width * channels
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    color_type = 6 if channels == 4 else 2
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    return "data:image/png;base64," + base64.b64encode(png).decode()


NODE_CLASS_MAPPINGS = {
    "MaicLlmServer": MaicLlmServer,
    "MaicLlmChat": MaicLlmChat,
}

NODE_DISPLAY_NAME_MAPPINGS = {
    "MaicLlmServer": "MAIC LLM Server (llama.cpp)",
    "MaicLlmChat": "MAIC LLM Chat",
}
