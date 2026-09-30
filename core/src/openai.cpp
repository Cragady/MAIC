// OpenAI-compatible /chat/completions (DeepSeek, OpenRouter, vLLM, LM Studio, llama.cpp server, ...), streamed
// as server-sent events. Tool call arguments arrive as JSON text in pieces.
#include "llm_http.hpp"

#include <deque>
#include <map>

namespace maic::detail {

Message chat_openai(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                    const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    nlohmann::json msgs = nlohmann::json::array();
    std::deque<std::string> pending_ids;
    int generated = 0;
    for (const auto& m : messages) {
        if (m.role == "assistant") {
            if (m.content.empty() && m.tool_calls.empty()) continue;
            nlohmann::json j = {{"role", "assistant"}, {"content", m.content}};
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
        } else {
            msgs.push_back({{"role", m.role}, {"content", m.content}});
        }
    }

    nlohmann::json body = {{"model", options.model}, {"stream", true}, {"messages", msgs}, {"stream_options", {{"include_usage", true}}}};
    if (!tools.empty()) body["tools"] = tools;
    nlohmann::json extra = provider.options.value("extra_body", nlohmann::json::object());
    for (const auto& [k, v] : extra.items()) body[k] = v;

    std::vector<std::pair<std::string, std::string>> headers;
    if (!provider.api_key_env.empty() || !provider.api_key_command.empty()) {
        headers.push_back({"Authorization", "Bearer " + provider.api_key()});
    }

    Message reply{"assistant", "", {}, "", "", false, "", nullptr};
    struct PartialCall {
        std::string id, name, args;
    };
    std::map<int, PartialCall> calls;
    std::string finish, error;

    LineSplitter lines;
    auto on_line = [&](const std::string& line) {
        if (line.rfind("data:", 0) != 0) return;
        std::string data = line.substr(5);
        if (data.find("[DONE]") != std::string::npos) return;
        auto j = nlohmann::json::parse(data, nullptr, false);
        if (!j.is_object()) return;
        try {
            if (j.contains("error")) {
                const auto& e = j["error"];
                error = e.is_object() ? e.value("message", e.dump()) : e.dump();
                return;
            }
            if (j.contains("usage") && j["usage"].is_object()) {
                reply.usage.input = j["usage"].value("prompt_tokens", reply.usage.input);
                reply.usage.output = j["usage"].value("completion_tokens", reply.usage.output);
            }
            for (const auto& choice : j.value("choices", nlohmann::json::array())) {
                const auto& d = choice.value("delta", nlohmann::json::object());
                for (const char* key : {"reasoning_content", "reasoning"}) {
                    if (d.contains(key) && d[key].is_string() && !d[key].get<std::string>().empty()) on_text(d[key].get<std::string>(), true);
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
    if (r.status != 200) throw_api_error(provider.name, r);
    if (!error.empty()) throw std::runtime_error(provider.name + ": " + error);

    reply.usage.context = provider.options.value("context_window", 0);
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
