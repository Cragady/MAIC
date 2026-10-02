// OpenAI-compatible /chat/completions (DeepSeek, OpenRouter, vLLM, LM Studio, llama.cpp server, ...), streamed
// as server-sent events. Tool call arguments arrive as JSON text in pieces.
#include "llm_http.hpp"

#include <algorithm>
#include <deque>
#include <map>

namespace maic::detail {

namespace {

using nlohmann::json;

// DeepSeek's thinking models need every earlier assistant turn's reasoning_content replayed while the
// request carries tools, or they answer 400 (api-docs.deepseek.com/guides/thinking_mode). Other servers
// ignore the field or reject it, so it is kept and sent only where `replay_reasoning` says, by default to a
// model whose name says deepseek.
bool replays_reasoning(const Provider& provider, const std::string& model) {
    return provider.options.value("replay_reasoning", model.find("deepseek") != std::string::npos);
}

// `vision`: true (the default) or false for the provider, or the list of its models that take pictures.
bool takes_images(const Provider& provider, const std::string& model) {
    const json v = provider.options.value("vision", json(true));
    if (v.is_array()) return std::find(v.begin(), v.end(), json(model)) != v.end();
    return !v.is_boolean() || v.get<bool>();
}

// `think_sampling` while thinking is on, `nothink_sampling` while it is off: a key set to false is not sent, one set to
// [low, high] only within it.
void drop_sampling(json& body, const json& rules) {
    if (!rules.is_object()) return;
    for (const auto& [k, rule] : rules.items()) {
        if (!body.contains(k)) continue;
        bool out_of_range = rule.is_array() && rule.size() == 2 && body[k].is_number() && (body[k] < rule[0] || body[k] > rule[1]);
        if ((rule.is_boolean() && !rule.get<bool>()) || out_of_range) body.erase(k);
    }
}

// A word on what an error status means for the user, after the provider's own message.
std::string status_hint(const Provider& provider, int status) {
    if (status == 401 || status == 403) {
        return provider.api_key_env.empty() ? " (the key was refused)" : " (the key in $" + provider.api_key_env + " was refused; check it, or set the right one)";
    }
    if (status == 402) return " (the account's balance is used up; nothing was retried. Top it up with the provider, or switch models)";
    if (status == 429) return " (rate limited)";
    if (status >= 500) return " (the provider's server failed)";
    return "";
}

// error.maic.upstream: the software that sent the error, what each rule replaced, and the rules' names.
json& upstream(json& error, const std::string& provider, const char* rule) {
    json& u = error["maic"]["upstream"];
    u["provider"] = provider;
    u["rules"].push_back(rule);
    return u;
}

json* error_of(json& body) {
    return body.contains("error") && body["error"].is_object() ? &body["error"] : nullptr;
}

bool error_code_string(json& body, const std::string& provider) {
    json* e = error_of(body);
    if (!e || !e->contains("code") || !(*e)["code"].is_number()) return false;
    upstream(*e, provider, "error_code_string")["code"] = (*e)["code"];
    (*e)["code"] = "maic_" + provider + "_" + (*e)["code"].dump();
    return true;
}

bool error_param_null(json& body, const std::string& provider) {
    json* e = error_of(body);
    if (!e || e->contains("param")) return false;
    upstream(*e, provider, "error_param_null");
    (*e)["param"] = nullptr;
    return true;
}

bool logprobs_refusal_null(json& body, const std::string&) {
    bool applied = false;
    if (!body.contains("choices") || !body["choices"].is_array()) return false;
    for (auto& choice : body["choices"]) {
        if (!choice.is_object() || !choice.contains("logprobs") || !choice["logprobs"].is_object() || choice["logprobs"].contains("refusal")) continue;
        choice["logprobs"]["refusal"] = nullptr;
        applied = true;
    }
    return applied;
}

// One line per rule, each named in docs/standards.md with what the server sends and why it is rewritten.
struct Rule {
    const char* name;
    bool (*apply)(json& body, const std::string& provider);
};
constexpr Rule kRules[] = {
    {"error_code_string", error_code_string},          // a numeric error code becomes "maic_<upstream>_<code>" (llama.cpp's 500)
    {"error_param_null", error_param_null},            // an error without param gets param: null (llama.cpp)
    {"logprobs_refusal_null", logprobs_refusal_null},  // choices[].logprobs without refusal gets refusal: null (llama.cpp)
};

}  // namespace

Message chat_openai(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                    const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    nlohmann::json msgs = nlohmann::json::array();
    std::deque<std::string> pending_ids;
    int generated = 0;
    bool mid_system = provider.options.value("mid_system", false);  // true: send later system messages as system
    bool replay = replays_reasoning(provider, options.model);
    bool vision = takes_images(provider, options.model);
    for (const auto& m : messages) {
        if (m.role == "assistant") {
            if (m.content.empty() && m.tool_calls.empty()) continue;
            nlohmann::json j = {{"role", "assistant"}, {"content", m.content}};
            if (replay) {
                // A turn with none kept (thinking was off, another model wrote it) goes with an empty one.
                bool kept = m.raw_kind == "openai" && m.raw.is_object() && m.raw.contains("reasoning_content");
                j["reasoning_content"] = kept ? m.raw["reasoning_content"] : json("");
            }
            if (!m.tool_calls.empty()) {
                j["tool_calls"] = nlohmann::json::array();
                for (const auto& call : m.tool_calls) {
                    std::string id = call.id.empty() ? "call_maic_" + std::to_string(++generated) : call.id;
                    pending_ids.push_back(id);
                    j["tool_calls"].push_back({{"id", id}, {"type", "function"},
                                               {"function", {{"name", call.name}, {"arguments", dump(call.arguments)}}}});
                }
            }
            msgs.push_back(j);
        } else if (m.role == "tool") {
            std::string id = m.tool_call_id;
            if (id.empty() && !pending_ids.empty()) id = pending_ids.front();
            if (!pending_ids.empty() && pending_ids.front() == id) pending_ids.pop_front();
            msgs.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", m.content}});
        } else if (m.role == "system" && !msgs.empty() && !mid_system) {
            // A system message after the first: most local chat templates (Qwen's among them) reject or
            // mishandle one, so it goes as a user-role note unless the provider says otherwise.
            msgs.push_back({{"role", "user"}, {"content", "[system note] " + m.content}});
        } else if (m.role == "user" && !m.images.empty() && !vision) {
            std::string text = m.content;
            for (const auto& im : m.images) text += "\n[image " + im.name + " not sent: " + options.model + " does not take pictures]";
            msgs.push_back({{"role", "user"}, {"content", text}});
        } else if (m.role == "user" && !m.images.empty()) {
            // Pictures ride as image_url parts beside the text (llama-server with --mmproj, and the OpenAI shape).
            nlohmann::json parts = nlohmann::json::array();
            if (!m.content.empty()) parts.push_back({{"type", "text"}, {"text", m.content}});
            for (const auto& im : m.images) parts.push_back({{"type", "image_url"}, {"image_url", {{"url", im.data_url()}}}});
            msgs.push_back({{"role", "user"}, {"content", parts}});
        } else {
            msgs.push_back({{"role", m.role}, {"content", m.content}});
        }
    }

    nlohmann::json body = {{"model", options.model}, {"stream", true}, {"messages", msgs}, {"stream_options", {{"include_usage", true}}}};
    if (options.sampling.is_object()) {
        for (const auto& [k, v] : options.sampling.items()) body[k] = v;
    }
    if (!options.stop.empty()) body["stop"] = options.stop;
    if (options.logit_bias.is_object() && !options.logit_bias.empty()) body["logit_bias"] = options.logit_bias;
    // llama-server honours these; OpenAI's own API rejects unknown fields, so a provider opts in.
    if (provider.options.value("thinking_controls", false)) {
        body["chat_template_kwargs"] = {{"enable_thinking", options.think}};
        if (!options.think) body["reasoning_effort"] = "none";
    }
    // An API that switches thinking with a request field of its own (DeepSeek's "thinking": {"type": ...}).
    const json think_fields = provider.options.value(options.think ? "think_on" : "think_off", json::object());  // named: iterating a temporary dangles
    for (const auto& [k, v] : think_fields.items()) body[k] = v;
    if (provider.options.contains("max_tokens") && !body.contains("max_tokens")) body["max_tokens"] = provider.options["max_tokens"];
    if (!tools.empty()) body["tools"] = tools;
    nlohmann::json extra = provider.options.value("extra_body", nlohmann::json::object());
    for (const auto& [k, v] : extra.items()) body[k] = v;
    drop_sampling(body, provider.options.value(options.think ? "think_sampling" : "nothink_sampling", json::object()));

    std::vector<std::pair<std::string, std::string>> headers;
    std::string key;
    if (!provider.api_key_env.empty() || !provider.api_key_command.empty()) {
        key = provider.api_key();
        headers.push_back({"Authorization", "Bearer " + key});
    }
    // Whatever an error echoes back, the key never reaches a message, a log or a transcript.
    auto redact = [&](std::string text) {
        for (size_t at; key.size() >= 8 && (at = text.find(key)) != std::string::npos;) text.replace(at, key.size(), "[key]");
        return text;
    };

    Message reply{"assistant", "", {}, "", "", false, "", nullptr};
    struct PartialCall {
        std::string id, name, args;
    };
    std::map<int, PartialCall> calls;
    std::string finish, error, reasoning, stray, served, fingerprint;
    std::map<std::string, int> normalized;
    auto normalize = [&](nlohmann::json& j) {
        for (const auto& rule : normalize_openai(j, provider.upstream_name())) ++normalized[rule];
    };

    LineSplitter lines;
    auto on_line = [&](const std::string& line) {
        if (line.rfind("data:", 0) != 0) {
            // A stray line is skipped, unless the stream ends on it: llama-server's router ends a proxied stream it lost
            // with a bare "proxy error: ..." line, with no data: and no [DONE].
            std::string field = line.substr(0, line.find(':'));
            if (!line.empty() && line[0] != ':' && field != "event" && field != "id" && field != "retry") stray = line;
            return;
        }
        stray.clear();
        std::string data = line.substr(5);
        if (data.find("[DONE]") != std::string::npos) return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (!j.is_object()) return;
        normalize(j);
        try {
            if (j.contains("error")) {
                const auto& e = j["error"];
                error = e.is_object() ? e.value("message", e.dump()) : e.dump();
                return;
            }
            if (j.contains("model") && j["model"].is_string()) served = j["model"];
            if (j.contains("system_fingerprint") && j["system_fingerprint"].is_string()) fingerprint = j["system_fingerprint"];
            if (j.contains("usage") && j["usage"].is_object()) {
                const auto& u = j["usage"];
                reply.usage.input = u.value("prompt_tokens", reply.usage.input);
                reply.usage.output = u.value("completion_tokens", reply.usage.output);
                if (u.contains("prompt_cache_hit_tokens") && u["prompt_cache_hit_tokens"].is_number_integer()) reply.usage.cached = u["prompt_cache_hit_tokens"];
                else if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) reply.usage.cached = u["prompt_tokens_details"].value("cached_tokens", 0);
            }
            for (const auto& choice : j.value("choices", nlohmann::json::array())) {
                const auto& d = choice.value("delta", nlohmann::json::object());
                for (const char* key : {"reasoning_content", "reasoning"}) {
                    if (d.contains(key) && d[key].is_string() && !d[key].get<std::string>().empty()) {
                        on_text(d[key].get<std::string>(), true);
                        reasoning += d[key].get<std::string>();
                    }
                }
                if (d.contains("content") && d["content"].is_string()) {
                    std::string c = d["content"];
                    reply.content += c;
                    if (!c.empty()) on_text(c, false);
                }
                for (const auto& tc : d.value("tool_calls", nlohmann::json::array())) {
                    auto& pc = calls[tc.value("index", 0)];
                    if (tc.contains("id") && tc["id"].is_string()) pc.id = tc["id"];
                    const auto& fn = tc.value("function", nlohmann::json::object());
                    if (fn.contains("name") && fn["name"].is_string()) pc.name += fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string()) pc.args += fn["arguments"].get<std::string>();
                }
                if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) finish = choice["finish_reason"];
            }
        } catch (const nlohmann::json::exception&) {
        }
    };
    auto r = stream_post(provider.base_url, "/chat/completions", headers, dump(body),
                         [&](std::string_view d) { lines.feed(d, on_line); }, cancel);
    lines.finish(on_line);
    if (r.status != 200) {
        auto body = nlohmann::json::parse(r.error_body, nullptr, false);
        if (body.is_object()) {
            normalize(body);
            r.error_body = dump(body);
        }
    }
    if (options.normalized) {
        for (const auto& [rule, n] : normalized) options.normalized(rule, n);
    }
    if (r.status != 200) {
        r.error_body = redact(r.error_body);
        throw_api_error(provider.name, r, status_hint(provider, r.status));
    }
    if (error.empty()) error = stray;
    if (!error.empty()) throw std::runtime_error(provider.name + ": " + redact(error));

    // finish_reason values beyond OpenAI's set (DeepSeek's): the reply did not end normally.
    if (finish == "insufficient_system_resource") throw std::runtime_error(provider.name + ": the provider dropped the request for lack of capacity (insufficient_system_resource); send it again");
    if (finish == "aborted") throw std::runtime_error(provider.name + ": the provider aborted the reply before it was complete");
    if (finish == "content_filter") reply.content += "\n[The provider's content filter stopped the reply here.]";
    if (finish == "stop" && reply.content.empty() && reasoning.empty() && calls.empty() && provider.options.value("retry_empty", false)) {
        throw TransportError(provider.name + " sent an empty reply (no text, reasoning or tool call)");
    }

    reply.usage.context = provider.options.value("context_window", 0);
    if (replay || provider.metered()) {
        // Kept with the turn in the transcript: the reasoning to replay, and which model answered (a provider may route
        // a request to another model; the notice makes that visible as it happens).
        nlohmann::json raw = nlohmann::json::object();
        if (!reasoning.empty()) raw["reasoning_content"] = reasoning;
        if (!served.empty()) raw["model"] = served;
        if (!fingerprint.empty()) raw["system_fingerprint"] = fingerprint;
        if (!raw.empty()) reply.raw_kind = "openai", reply.raw = raw;
        if (!served.empty() && served != options.model && options.notice) options.notice(provider.name + ": asked for " + options.model + ", answered by " + served);
    }
    if (finish == "length" && !calls.empty()) {
        reply.content += "\n[Output hit the length limit before the tool call was complete.]";
        return reply;
    }
    for (auto& [i, pc] : calls) {
        if (pc.name.empty()) continue;
        auto args = pc.args.empty() ? nlohmann::json::object() : nlohmann::json::parse(pc.args, nullptr, false);
        if (!args.is_object()) args = {{"_maic_invalid_input", pc.args}};
        reply.tool_calls.push_back({pc.id, pc.name, args});
    }
    return reply;
}

}  // namespace maic::detail

namespace maic {

std::vector<std::string> normalize_openai(nlohmann::json& body, const std::string& upstream) {
    std::vector<std::string> applied;
    for (const auto& rule : detail::kRules) {
        if (rule.apply(body, upstream)) applied.push_back(rule.name);
    }
    return applied;
}

}  // namespace maic
