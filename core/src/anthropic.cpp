// Anthropic Messages API (POST /v1/messages), streamed as server-sent events.
//
// History is replayed append-only and each assistant turn goes back exactly as it was received (thinking
// blocks with their signatures included); newer models reject a history whose earlier turns were edited.
#include "llm_http.hpp"

#include <deque>
#include <map>

namespace maid::detail {

namespace {

// OpenAI function schemas -> Anthropic tools. Large inputs (whole files) stream as they're generated.
nlohmann::json convert_tools(const nlohmann::json& tools) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& t : tools) {
        const auto& fn = t.at("function");
        out.push_back({{"name", fn.at("name")},
                       {"description", fn.value("description", "")},
                       {"input_schema", fn.at("parameters")},
                       {"eager_input_streaming", true}});
    }
    return out;
}

struct Converted {
    std::string system;
    nlohmann::json messages = nlohmann::json::array();
};

Converted convert_messages(const std::vector<Message>& messages, bool mid_system) {
    Converted c;
    std::deque<std::string> pending_ids;  // tool_use ids waiting for their results, in order
    int generated = 0;
    nlohmann::json results;               // tool_result blocks for the current user turn

    auto flush_results = [&] {
        if (results.is_array() && !results.empty()) c.messages.push_back({{"role", "user"}, {"content", results}});
        results = nlohmann::json::array();
    };
    results = nlohmann::json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& m = messages[i];
        if (m.role == "tool") {
            std::string id = m.tool_call_id;
            if (id.empty() && !pending_ids.empty()) id = pending_ids.front();
            if (!pending_ids.empty() && pending_ids.front() == id) pending_ids.pop_front();
            nlohmann::json block = {{"type", "tool_result"}, {"tool_use_id", id}, {"content", m.content}};
            if (m.is_error) block["is_error"] = true;
            results.push_back(block);
            continue;
        }
        flush_results();
        if (m.role == "system") {
            if (i == 0) c.system = m.content;
            else if (mid_system) c.messages.push_back({{"role", "system"}, {"content", m.content}});
            else c.messages.push_back({{"role", "user"}, {"content", "[system note] " + m.content}});
        } else if (m.role == "user" && !m.images.empty()) {
            nlohmann::json blocks = nlohmann::json::array();
            for (const auto& im : m.images) blocks.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", im.mime}, {"data", im.base64}}}});
            if (!m.content.empty()) blocks.push_back({{"type", "text"}, {"text", m.content}});
            c.messages.push_back({{"role", "user"}, {"content", blocks}});
        } else if (m.role == "user") {
            c.messages.push_back({{"role", "user"}, {"content", m.content}});
        } else if (m.role == "assistant") {
            nlohmann::json content;
            if (m.raw_kind == "anthropic" && m.raw.is_array()) {
                content = m.raw;
                for (const auto& b : content) {
                    if (b.value("type", "") == "tool_use") pending_ids.push_back(b.value("id", ""));
                }
            } else {
                content = nlohmann::json::array();
                if (!m.content.empty()) content.push_back({{"type", "text"}, {"text", m.content}});
                for (const auto& call : m.tool_calls) {
                    std::string id = call.id.empty() ? "toolu_maid_" + std::to_string(++generated) : call.id;
                    pending_ids.push_back(id);
                    content.push_back({{"type", "tool_use"}, {"id", id}, {"name", call.name}, {"input", call.arguments}});
                }
            }
            if (!content.empty()) c.messages.push_back({{"role", "assistant"}, {"content", content}});
        }
    }
    flush_results();
    return c;
}

}  // namespace

Message chat_anthropic(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                       const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    const auto& opt = provider.options;
    std::vector<std::string> betas;

    auto build = [&](bool mid_system) {
        Converted c = convert_messages(messages, mid_system);
        nlohmann::json body = {
            {"model", options.model},
            {"max_tokens", opt.value("max_tokens", 64000)},
            {"stream", true},
            {"messages", c.messages},
        };
        if (!c.system.empty()) body["system"] = c.system;
        if (!options.stop.empty()) body["stop_sequences"] = options.stop;
        if (!tools.empty()) body["tools"] = convert_tools(tools);
        // Thinking is adaptive by default on current models; depth is set with effort.
        std::string effort_key = options.think ? "think_effort" : "effort";
        if (opt.contains(effort_key) && opt[effort_key].is_string()) {
            body["output_config"] = {{"effort", opt[effort_key]}};
        }
        if (opt.value("fallbacks", "") == "default") {
            body["fallbacks"] = "default";
        }
        return body;
    };

    std::vector<std::pair<std::string, std::string>> headers = {{"anthropic-version", "2023-06-01"}};
    if (opt.value("auth", "") == "bearer") {
        headers.push_back({"Authorization", "Bearer " + provider.api_key()});
        betas.push_back("oauth-2025-04-20");
    } else {
        headers.push_back({"x-api-key", provider.api_key()});
    }
    if (opt.value("fallbacks", "") == "default") betas.push_back("server-side-fallback-2026-07-01");
    if (!betas.empty()) {
        std::string joined;
        for (const auto& b : betas) joined += (joined.empty() ? "" : ",") + b;
        headers.push_back({"anthropic-beta", joined});
    }

    for (bool mid_system : {true, false}) {
        std::map<int, nlohmann::json> blocks;
        std::map<int, std::string> partial_input;
        std::string stop_reason, error, served_by;
        Usage usage;

        auto handle_event = [&](const nlohmann::json& ev) {
            std::string type = ev.value("type", "");
            try {
                if (type == "message_start") {
                    const auto& msg = ev.value("message", nlohmann::json::object());
                    served_by = msg.value("model", "");
                    const auto& u = msg.value("usage", nlohmann::json::object());
                    usage.input = u.value("input_tokens", 0) + u.value("cache_read_input_tokens", 0) + u.value("cache_creation_input_tokens", 0);
                } else if (type == "content_block_start") {
                    int i = ev.value("index", 0);
                    blocks[i] = ev.value("content_block", nlohmann::json::object());
                } else if (type == "content_block_delta") {
                    int i = ev.value("index", 0);
                    const auto& d = ev.value("delta", nlohmann::json::object());
                    std::string dt = d.value("type", "");
                    if (dt == "text_delta") {
                        std::string t = d.value("text", "");
                        blocks[i]["text"] = blocks[i].value("text", "") + t;
                        on_text(t, false);
                    } else if (dt == "thinking_delta") {
                        std::string t = d.value("thinking", "");
                        blocks[i]["thinking"] = blocks[i].value("thinking", "") + t;
                        on_text(t, true);
                    } else if (dt == "signature_delta") {
                        blocks[i]["signature"] = d.value("signature", "");
                    } else if (dt == "input_json_delta") {
                        partial_input[i] += d.value("partial_json", "");
                    }
                } else if (type == "content_block_stop") {
                    int i = ev.value("index", 0);
                    if (blocks[i].value("type", "") == "tool_use") {
                        // Strict parse: eager streaming means the API hasn't validated this input.
                        const std::string& raw = partial_input[i];
                        auto input = raw.empty() ? nlohmann::json::object() : nlohmann::json::parse(raw, nullptr, false);
                        if (!input.is_object()) {
                            blocks[i]["_maid_invalid_input"] = raw;
                            input = nlohmann::json::object();
                        }
                        blocks[i]["input"] = input;
                    }
                } else if (type == "message_delta") {
                    auto sr = ev.value("delta", nlohmann::json::object()).value("stop_reason", nlohmann::json());
                    if (sr.is_string()) stop_reason = sr.get<std::string>();
                    usage.output = ev.value("usage", nlohmann::json::object()).value("output_tokens", usage.output);
                } else if (type == "error") {
                    const auto& e = ev.value("error", nlohmann::json::object());
                    error = e.value("type", "error") + ": " + e.value("message", "");
                }
            } catch (const nlohmann::json::exception&) {
            }
        };

        LineSplitter lines;
        auto on_line = [&](const std::string& line) {
            if (line.rfind("data:", 0) != 0) return;  // event:, id:, comments and blank lines carry nothing we need
            auto j = nlohmann::json::parse(line.substr(5), nullptr, false);
            if (j.is_object()) handle_event(j);
        };
        auto r = stream_post(provider.base_url, "/v1/messages", headers, dump(build(mid_system)),
                             [&](std::string_view d) { lines.feed(d, on_line); }, cancel);
        lines.finish(on_line);

        if (r.status == 400 && mid_system && r.error_body.find("system") != std::string::npos &&
            r.error_body.find("not supported") != std::string::npos) {
            continue;  // this model takes no mid-conversation system messages; resend them as notes
        }
        if (r.status != 200) throw_api_error(provider.name, r);
        if (!error.empty()) throw std::runtime_error(provider.name + ": " + error);

        Message reply{"assistant", "", {}, "", "", false, "anthropic", nlohmann::json::array()};
        reply.usage = usage;
        // Current Anthropic models have a 1M window; Haiku 4.5 has 200K. A provider option can override.
        reply.usage.context = opt.value("context_window", options.model.find("haiku") != std::string::npos ? 200000 : 1000000);
        for (auto& [i, b] : blocks) {
            std::string bt = b.value("type", "");
            if (bt == "text") reply.content += b.value("text", "");
            if (bt == "tool_use") {
                nlohmann::json args = b["input"];
                if (b.contains("_maid_invalid_input")) {
                    args = {{"_maid_invalid_input", b["_maid_invalid_input"]}};
                    b.erase("_maid_invalid_input");
                }
                reply.tool_calls.push_back({b.value("id", ""), b.value("name", ""), args});
            }
            reply.raw.push_back(b);
        }
        // A refusal or a max_tokens cut can leave a tool call half-written: never run those.
        if (stop_reason == "refusal") {
            reply.tool_calls.clear();
            reply.content += "\n[The model declined this request.]";
        } else if (stop_reason == "max_tokens" && !reply.tool_calls.empty()) {
            reply.tool_calls.clear();
            reply.content += "\n[Output hit max_tokens before the tool call was complete.]";
        }
        if (!reply.tool_calls.empty() || stop_reason == "refusal" || stop_reason == "max_tokens") {
            // Tool calls we won't answer can't stay in the replayed turn.
            if (reply.tool_calls.empty()) {
                nlohmann::json kept = nlohmann::json::array();
                for (const auto& b : reply.raw) {
                    if (b.value("type", "") != "tool_use") kept.push_back(b);
                }
                reply.raw = kept;
            }
        }
        return reply;
    }
    throw std::runtime_error(provider.name + ": request failed");
}

}  // namespace maid::detail
