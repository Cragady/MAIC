#pragma once

#include "maid/image.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace maid {

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
    int cached = 0;  // of `input`, what the provider read from its prompt cache (DeepSeek's prompt_cache_hit_tokens), when it says
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
    // Billed per token to the account the key belongs to, or spending a plan's usage: `options.metered`, by default
    // true for a remote OpenAI-compatible provider with a key (deepseek, openrouter, one you add), and set on the
    // shipped anthropic and claude-cli. The automatic model picks never
    // move onto a metered model of another provider, and chat() never retries a request that may have run.
    bool metered() const;
    // Throws with a clear message when it can't be found, and refuses to hand a key to a plain http URL that
    // leaves the machine: a key goes only over https, or to loopback.
    std::string api_key() const;
};

// The environment variables that hold model keys: every `api_key_env` of the providers passed to add_key_envs (load_settings
// passes each set it loads; names are only ever added) and any NAME_API_KEY. A child MAID starts that is not meant to
// hold a key (claude-cli, a service, nvim, git) starts without them; the command sandbox starts from an empty environment.
void add_key_envs(const std::vector<Provider>& providers);
bool is_key_env(std::string_view name);

// llamacpp (local), anthropic, deepseek, openrouter, claude-cli. Settings can add or override providers by name.
std::vector<Provider> default_providers();

// "anthropic/claude-opus-5-5" -> the anthropic provider and "claude-opus-5-5". A string whose prefix isn't a
// provider name (model names can contain '/') goes to the first provider whole.
std::pair<Provider, std::string> resolve_model(const std::vector<Provider>& providers, const std::string& model);

// A one-line title for a conversation that starts with `first_prompt`, from `model` on `provider`; "" when the
// reply was not usable as one. The auto-title after a first turn and `maid sessions name` share it.
std::string generate_title(const Provider& provider, const std::string& model, const std::string& first_prompt);

struct ChatOptions {
    std::string model;  // without the provider prefix
    bool think = false;
    // Retry on 429 (not a usage limit), 408, 409, 5xx and connection failures, only while nothing has been streamed yet,
    // at most `retries` times: full jitter, a wait drawn from 0 to min(cap, base * 2^attempt), the cap 60 times the
    // base (1 s and 60 s), or Retry-After when the provider asks for longer. 400, 401, 402 and 403 never are. `notice`
    // hears about each wait. A metered provider is not retried when its answer was lost while being read
    // (TransportError::retry_safe), since the request may have run and been billed.
    // Every request to one provider (its name and base_url) shares the account's state: a 429 holds them all for its
    // wait, five 429s within the cap open a breaker that sends nothing for the cap and fails each request at once with
    // a message saying so, and `options.max_concurrent` queues what is past the cap (docs/models.md, Rate limits).
    int retries = 3;
    int retry_base_ms = 1000;
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

// The wait before retry number `attempt` (0 for the first): full jitter, uniform in [0, min(60 * base_ms, base_ms * 2^attempt)].
int retry_wait_ms(int attempt, int base_ms);

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

// The host could not be reached or the connection dropped before any response arrived. `retry_safe` is false when
// the request went out whole and the answer was lost while being read: the provider may have run it to the end.
struct TransportError : std::runtime_error {
    bool retry_safe;
    explicit TransportError(const std::string& what, bool retry_safe = true) : std::runtime_error(what), retry_safe(retry_safe) {}
};

// Receives streamed text as it arrives; `thinking` marks reasoning rather than the answer.
using TextSink = std::function<void(std::string_view delta, bool thinking)>;

// GET <base_url>/models on an OpenAI-compatible server (llama.cpp's router lists every GGUF it can load).
std::vector<std::string> list_openai_models(const Provider& provider);

// What an API's GET /models says about one model, as far as it says: DeepSeek's `context_window`, `max_output_tokens`
// and `effort` (`supported_levels`, `default_level`). 0 or empty where it says nothing.
struct ModelFacts {
    long context = 0;
    long output = 0;
    std::vector<std::string> efforts;
    std::string default_effort;
};
struct ApiModelFacts {
    std::map<std::string, ModelFacts> models;  // by the provider's model id
    long read_at = 0;   // when they were read (unix time); 0 when never
    bool saved = false; // from <state>/api-models/<provider>.json, because the read failed
    std::string error;  // why the last read failed; "" when it did not
};
// For a provider with `options.read_models`: GET /models once per process, at its first use, with a short timeout
// (2 s to connect, 3 s to answer) and no retry, kept for the process and saved with the time in
// <state>/api-models/<provider>.json. When the read fails, the saved copy for the same base_url stands in, else nothing
// does and the provider's options (and the catalog) apply as before. `refresh` reads again (`maid models refresh`).
ApiModelFacts api_model_facts(const Provider& provider, bool refresh = false);
// The saved copy alone, without a request (`maid models`): empty when there is none for this base_url.
ApiModelFacts saved_model_facts(const Provider& provider);
// Whether something answers HTTP at the provider's host (GET /health, any status): a local server that is up.
bool server_answers(const Provider& provider);

// The OpenAI-compatible client's adapter rules ("Adapter normalizations" in docs/standards.md): rewrites one parsed
// chunk or error body that `provider` sent into the shape OpenAI's description (protocol/openai/) gives it, in
// place, and returns the names of the rules that applied. A numeric error code becomes "maid_<upstream>_<code>"; an
// error object keeps what was replaced, `upstream` as its provider, and the rules' names in error.maid.upstream.
// chat() runs it on everything it reads from an OpenAI-compatible server, with the provider's upstream_name().
std::vector<std::string> normalize_openai(nlohmann::json& body, const std::string& upstream);

// Tool schemas are given in OpenAI function format and converted per provider.
// Throws Cancelled if `cancel` is set, std::runtime_error on transport or API errors.
Message chat(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
             const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel);

// The MCP versions MAID's servers speak, newest first (docs/standards.md, MCP).
extern const std::vector<std::string> kMcpVersions;

// `maid mcp-bridge SOCKET`: what a `cli` agent starts as its MCP server. It joins its stdin and stdout to the unix
// socket where MAID serves its tools to that agent, and returns when either side closes.
int run_mcp_bridge(const std::string& socket_path);

}  // namespace maid
