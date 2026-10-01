// Ollama's native /api/chat: NDJSON stream, tools in function format, arguments as objects.
#include "llm_http.hpp"

namespace maic::detail {

namespace {

nlohmann::json to_json(const Message& m) {
    nlohmann::json j = {{"role", m.role}, {"content", m.content}};
    if (!m.tool_calls.empty()) {
        j["tool_calls"] = nlohmann::json::array();
        for (const auto& call : m.tool_calls) {
            j["tool_calls"].push_back({{"function", {{"name", call.name}, {"arguments", call.arguments}}}});
        }
    }
    if (!m.tool_name.empty()) j["tool_name"] = m.tool_name;
    if (!m.images.empty()) {
        j["images"] = nlohmann::json::array();
        for (const auto& im : m.images) j["images"].push_back(im.base64);
    }
    return j;
}

}  // namespace

Message chat_ollama(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                    const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    nlohmann::json body = {
        {"model", options.model},
        {"stream", true},
        {"think", options.think},
        {"options", {{"num_ctx", options.num_ctx}}},
        {"messages", nlohmann::json::array()},
    };
    if (options.sampling.is_object()) {
        for (const auto& [k, v] : options.sampling.items()) body["options"][k] = v;
    }
    if (!options.stop.empty()) body["options"]["stop"] = options.stop;
    for (const auto& m : messages) {
        if (m.role == "assistant" && m.content.empty() && m.tool_calls.empty()) continue;
        body["messages"].push_back(to_json(m));
    }
    if (!tools.empty()) body["tools"] = tools;

    Message reply{"assistant", "", {}, "", "", false, "", nullptr};
    std::string error;
    auto handle_line = [&](const std::string& line) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) return;
        if (j.contains("error")) {
            error = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
            return;
        }
        // Anything malformed in a line is skipped rather than thrown through the HTTP client.
        try {
            if (j.contains("prompt_eval_count")) reply.usage.input = j.value("prompt_eval_count", 0);
            if (j.contains("eval_count")) reply.usage.output = j.value("eval_count", 0);
            const auto& msg = j.value("message", nlohmann::json::object());
            if (auto t = msg.value("thinking", ""); !t.empty()) on_text(t, true);
            if (auto c = msg.value("content", ""); !c.empty()) {
                reply.content += c;
                on_text(c, false);
            }
            for (const auto& call : msg.value("tool_calls", nlohmann::json::array())) {
                const auto& fn = call.value("function", nlohmann::json::object());
                if (!fn.contains("name") || !fn["name"].is_string()) continue;
                auto args = fn.value("arguments", nlohmann::json::object());
                reply.tool_calls.push_back({call.value("id", ""), fn["name"].get<std::string>(),
                                            args.is_object() ? args : nlohmann::json::object()});
            }
        } catch (const nlohmann::json::exception&) {
        }
    };

    LineSplitter lines;
    auto r = stream_post(provider.base_url, "/api/chat", {}, dump(body),
                         [&](std::string_view d) { lines.feed(d, handle_line); }, cancel);
    lines.finish(handle_line);
    if (!error.empty()) throw std::runtime_error(provider.name + ": " + error);
    if (r.status != 200) throw_api_error(provider.name, r);
    reply.usage.context = options.num_ctx;
    return reply;
}

}  // namespace maic::detail
