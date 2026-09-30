#include "maic/ollama.hpp"

#include <httplib.h>

#include <thread>

namespace maic {

namespace {

constexpr const char* kHost = "127.0.0.1";
constexpr int kPort = 11434;

nlohmann::json to_json(const Message& m) {
    nlohmann::json j = {{"role", m.role}, {"content", m.content}};
    if (!m.tool_calls.empty()) {
        j["tool_calls"] = nlohmann::json::array();
        for (const auto& call : m.tool_calls) {
            j["tool_calls"].push_back({{"function", {{"name", call.name}, {"arguments", call.arguments}}}});
        }
    }
    if (!m.tool_name.empty()) {
        j["tool_name"] = m.tool_name;
    }
    return j;
}

}  // namespace

Message ollama_chat(const ChatOptions& options, const std::vector<Message>& messages, const nlohmann::json& tools,
                    const TextSink& on_text, const std::atomic<bool>& cancel) {
    nlohmann::json body = {
        {"model", options.model},
        {"stream", true},
        {"think", options.think},
        {"options", {{"num_ctx", options.num_ctx}}},
        {"messages", nlohmann::json::array()},
    };
    for (const auto& m : messages) {
        body["messages"].push_back(to_json(m));
    }
    if (!tools.empty()) {
        body["tools"] = tools;
    }

    Message reply{"assistant", "", {}, ""};
    std::string pending;  // partial NDJSON line between chunks
    std::string error;

    auto handle_line = [&](const std::string& line) {
        if (line.empty()) {
            return;
        }
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded()) {
            return;
        }
        if (j.contains("error")) {
            error = j["error"].get<std::string>();
            return;
        }
        const auto& msg = j.value("message", nlohmann::json::object());
        if (auto t = msg.value("thinking", ""); !t.empty()) {
            on_text(t, true);
        }
        if (auto c = msg.value("content", ""); !c.empty()) {
            reply.content += c;
            on_text(c, false);
        }
        for (const auto& call : msg.value("tool_calls", nlohmann::json::array())) {
            const auto& fn = call.at("function");
            reply.tool_calls.push_back({fn.at("name").get<std::string>(), fn.value("arguments", nlohmann::json::object())});
        }
    };

    httplib::Client client(kHost, kPort);
    client.set_connection_timeout(5);
    client.set_read_timeout(600);

    httplib::Request req;
    req.method = "POST";
    req.path = "/api/chat";
    req.set_header("Content-Type", "application/json");
    req.body = body.dump();
    req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
        pending.append(data, len);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            handle_line(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
        return !cancel.load();
    };

    // The receiver only sees cancel when data arrives. While the model loads or reads a long prompt nothing
    // does, so a watcher closes the socket instead.
    std::atomic<bool> finished{false};
    std::thread watcher([&] {
        while (!finished.load()) {
            if (cancel.load()) {
                client.stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    httplib::Response res;
    httplib::Error err = httplib::Error::Success;
    bool ok = client.send(req, res, err);
    finished = true;
    watcher.join();
    if (cancel.load()) {
        throw Cancelled();
    }
    if (!ok) {
        throw std::runtime_error("can't reach Ollama at 127.0.0.1:11434 (" + httplib::to_string(err) + "). Is it running? `maic up ollama`");
    }
    handle_line(pending);
    if (!error.empty()) {
        throw std::runtime_error("Ollama: " + error);
    }
    if (res.status != 200) {
        throw std::runtime_error("Ollama returned HTTP " + std::to_string(res.status));
    }
    return reply;
}

}  // namespace maic
