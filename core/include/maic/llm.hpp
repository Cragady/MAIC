#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace maic {

struct ToolCall {
    std::string id;  // provider's call id; empty from providers that don't use them
    std::string name;
    nlohmann::json arguments;
};

// Token counts a provider reported for one reply. `context` is the model's window when known (0 otherwise).
struct Usage {
    int input = 0;   // prompt tokens this call (the conversation so far, cache reads included)
    int output = 0;
    int context = 0;
};

struct Message {
    std::string role;  // system, user, assistant, tool
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_name;     // role == "tool"
    std::string tool_call_id;  // role == "tool"
    bool is_error = false;     // role == "tool"
    // The provider's own content blocks for an assistant turn (Anthropic thinking blocks with signatures,
    // fallback blocks, ...). Replayed unchanged to the same provider kind; other kinds use the fields above.
    std::string raw_kind;
    nlohmann::json raw;
    Usage usage;  // assistant replies only
};

// Session files store messages this way; raw provider blocks survive the round trip.
nlohmann::json message_to_json(const Message& m);
Message message_from_json(const nlohmann::json& j);

// Where a model lives. Model strings are "<provider>/<model>", e.g. "anthropic/claude-opus-5-5".
struct Provider {
    std::string name;
    std::string kind;      // "ollama", "anthropic" or "openai" (any OpenAI-compatible API: DeepSeek, OpenRouter, ...)
    std::string base_url;  // e.g. http://127.0.0.1:11434, https://api.anthropic.com, https://api.deepseek.com
    std::string api_key_env;      // environment variable holding the API key
    std::string api_key_command;  // or a command that prints it (a password manager); never a key in a file
    nlohmann::json options = nlohmann::json::object();  // kind-specific, see docs/providers.md

    // Anything not on this machine. Prompts, files the agent reads and tool output leave the machine.
    bool remote() const;
    std::string api_key() const;  // throws with a clear message when it can't be found
};

// ollama and llamacpp (local), anthropic, deepseek, openrouter. Settings can add or override providers by name.
std::vector<Provider> default_providers();

// "anthropic/claude-opus-5-5" -> the anthropic provider and "claude-opus-5-5". A string whose prefix isn't a
// provider name (Ollama names can contain '/') goes to the first provider whole.
std::pair<Provider, std::string> resolve_model(const std::vector<Provider>& providers, const std::string& model);

struct ChatOptions {
    std::string model;  // without the provider prefix
    bool think = false;
    int num_ctx = 16384;  // ollama only
    // Retry on 429, 5xx and connection failures, only while nothing has been streamed yet: 2 s, doubling,
    // 25% jitter, 30 s cap, Retry-After honoured. `notice` hears about each wait.
    int retries = 3;
    int retry_base_ms = 2000;
    std::function<void(const std::string&)> notice;
    // Generation controls. `stop` and `sampling` go to every provider that has them; `logit_bias` only to
    // OpenAI-compatible ones ({"<token id or text>": -100}).
    std::vector<std::string> stop;
    nlohmann::json logit_bias;  // null when none
    nlohmann::json sampling;    // temperature, top_k, top_p, min_p, seed, ... merged into the provider's options
};

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

// A provider answered with an error status. `retry_after_ms` is from the Retry-After header when present.
struct ApiError : std::runtime_error {
    int status;
    int retry_after_ms;
    ApiError(int status, std::string message, int retry_after_ms = 0) : std::runtime_error(std::move(message)), status(status), retry_after_ms(retry_after_ms) {}
};

// The host could not be reached or the connection dropped before any response arrived.
struct TransportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Receives streamed text as it arrives; `thinking` marks reasoning rather than the answer.
using TextSink = std::function<void(std::string_view delta, bool thinking)>;

// Names of the models an Ollama server has (GET /api/tags).
std::vector<std::string> list_ollama_models(const Provider& provider);
// GET <base_url>/models on an OpenAI-compatible server (llama.cpp's router lists every GGUF it can load).
std::vector<std::string> list_openai_models(const Provider& provider);

// Tool schemas are given in OpenAI/Ollama function format and converted per provider.
// Throws Cancelled if `cancel` is set, std::runtime_error on transport or API errors.
Message chat(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
             const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel);

}  // namespace maic
