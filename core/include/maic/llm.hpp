#pragma once

#include "maic/image.hpp"

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
    std::vector<ImageData> images;  // user turns only: pictures sent with the text
};

// Session files store messages this way; raw provider blocks survive the round trip.
nlohmann::json message_to_json(const Message& m);
Message message_from_json(const nlohmann::json& j);

// Whether `host` (a URL's host; [brackets] allowed) is this machine's loopback: localhost, ::1, or 127.0.0.0/8
// written as a plain dotted quad. Anything else is not, a name that merely starts with "127." included.
bool loopback_host(std::string host);
// Whether `url` reaches only this machine: an http(s) URL whose host, once the scheme, userinfo and port are
// parsed off, is a loopback host, or a unix socket (unix:PATH). Everything else is remote.
bool local_url(const std::string& url);

// Where a model lives. Model strings are "<provider>/<model>", e.g. "anthropic/claude-opus-5-5".
struct Provider {
    std::string name;
    std::string kind;      // "anthropic", "openai" (any OpenAI-compatible API: llama.cpp server, DeepSeek, OpenRouter, ...)
                           // or "cli" (an agent CLI run headless as a text-only model: `claude -p`)
    std::string base_url;  // e.g. http://127.0.0.1:8081/v1, https://api.anthropic.com, https://api.deepseek.com
    std::string api_key_env;      // environment variable holding the API key
    std::string api_key_command;  // or a command that prints it (a password manager); never a key in a file
    nlohmann::json options = nlohmann::json::object();  // kind-specific, see docs/settings.md
    // The software behind it, whatever it is called here: the shipped llamacpp and llamacpp-2 are both "llamacpp".
    // "" lets the name stand for it. Names the source in what the adapter rules rewrite (normalize_openai).
    std::string upstream;

    std::string upstream_name() const { return upstream.empty() ? name : upstream; }
    // Anything not on this machine. Prompts, files the agent reads and tool output leave the machine.
    bool remote() const;
    std::string api_key() const;  // throws with a clear message when it can't be found
};

// llamacpp (local), anthropic, deepseek, openrouter, claude-cli. Settings can add or override providers by name.
std::vector<Provider> default_providers();

// "anthropic/claude-opus-5-5" -> the anthropic provider and "claude-opus-5-5". A string whose prefix isn't a
// provider name (model names can contain '/') goes to the first provider whole.
std::pair<Provider, std::string> resolve_model(const std::vector<Provider>& providers, const std::string& model);

// A one-line title for a conversation that starts with `first_prompt`, from `model` on `provider`; "" when the
// reply was not usable as one. The auto-title after a first turn and `maic sessions name` share it.
std::string generate_title(const Provider& provider, const std::string& model, const std::string& first_prompt);

struct ChatOptions {
    std::string model;  // without the provider prefix
    bool think = false;
    // Retry on 429 (not a usage limit), 5xx and connection failures, only while nothing has been streamed yet: 2 s, doubling,
    // 25% jitter, 30 s cap, Retry-After honoured. `notice` hears about each wait.
    int retries = 3;
    int retry_base_ms = 2000;
    std::function<void(const std::string&)> notice;
    // Generation controls. `stop` and `sampling` go to every provider that has them; `logit_bias` only to
    // OpenAI-compatible ones ({"<token id or text>": -100}).
    std::vector<std::string> stop;
    nlohmann::json logit_bias;  // null when none
    nlohmann::json sampling;    // temperature, top_k, top_p, min_p, seed, ... merged into the provider's options
    // Hears, once per call, each adapter rule that rewrote what an OpenAI-compatible server sent into OpenAI's
    // shape (normalize_openai) and how many times it applied, before the reply returns or the error is thrown.
    std::function<void(const std::string& rule, int count)> normalized;
};

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

// A provider answered with an error status. `retry_after_ms` is from the Retry-After header when present;
// `type` is the provider's own error type or code ("rate_limit_error", "insufficient_quota"), "" when it gave none.
struct ApiError : std::runtime_error {
    int status;
    int retry_after_ms;
    std::string type;
    ApiError(int status, std::string message, int retry_after_ms = 0, std::string type = "")
        : std::runtime_error(std::move(message)), status(status), retry_after_ms(retry_after_ms), type(std::move(type)) {}
};

// The plan's usage limit or credit is used up (Fable's "You've reached your Fable limit", OpenAI's
// insufficient_quota, a 402), as opposed to a per-minute rate limit that clears by itself. chat() does not
// retry one: waiting does not bring a used-up limit back.
bool is_usage_limit(const ApiError& e);

// The host could not be reached or the connection dropped before any response arrived.
struct TransportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Receives streamed text as it arrives; `thinking` marks reasoning rather than the answer.
using TextSink = std::function<void(std::string_view delta, bool thinking)>;

// GET <base_url>/models on an OpenAI-compatible server (llama.cpp's router lists every GGUF it can load).
std::vector<std::string> list_openai_models(const Provider& provider);
// Whether something answers HTTP at the provider's host (GET /health, any status): a local server that is up.
bool server_answers(const Provider& provider);

// The OpenAI-compatible client's adapter rules ("Adapter normalizations" in docs/standards.md): rewrites one parsed
// chunk or error body that `provider` sent into the shape OpenAI's description (protocol/openai/) gives it, in
// place, and returns the names of the rules that applied. A numeric error code becomes "maic_<upstream>_<code>"; an
// error object keeps what was replaced, `upstream` as its provider, and the rules' names in error.maic.upstream.
// chat() runs it on everything it reads from an OpenAI-compatible server, with the provider's upstream_name().
std::vector<std::string> normalize_openai(nlohmann::json& body, const std::string& upstream);

// Tool schemas are given in OpenAI function format and converted per provider.
// Throws Cancelled if `cancel` is set, std::runtime_error on transport or API errors.
Message chat(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
             const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel);

}  // namespace maic
