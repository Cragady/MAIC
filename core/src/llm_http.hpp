#pragma once

// Shared plumbing for the provider implementations. Not part of the public API.

#include "maic/llm.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace maic::detail {

struct HttpResult {
    int status = 0;
    std::string error_body;  // first few KB of a non-200 body
};

// POSTs `body` to base_url + path and feeds the response body to `on_data` as it arrives. Cancel closes the
// connection even while nothing is arriving. Throws Cancelled, or runtime_error when the host can't be reached.
HttpResult stream_post(const std::string& base_url, const std::string& path,
                       const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                       const std::function<void(std::string_view)>& on_data, const std::atomic<bool>& cancel);

// Splits a byte stream into lines, holding partial lines between chunks.
class LineSplitter {
public:
    template <typename F>
    void feed(std::string_view data, F&& on_line) {
        pending_.append(data);
        for (size_t nl; (nl = pending_.find('\n')) != std::string::npos;) {
            std::string line = pending_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, nl + 1);
            on_line(line);
        }
    }
    template <typename F>
    void finish(F&& on_line) {
        if (!pending_.empty()) on_line(pending_);
        pending_.clear();
    }

private:
    std::string pending_;
};

// Tool output can hold invalid UTF-8 (binary files); replace it instead of failing every later request.
std::string dump(const nlohmann::json& j);

// Pulls a readable message out of a provider's error body.
std::string api_error(const std::string& provider, const HttpResult& r);

Message chat_ollama(const Provider&, const ChatOptions&, const std::vector<Message>&, const nlohmann::json& tools,
                    const TextSink&, const std::atomic<bool>&);
Message chat_anthropic(const Provider&, const ChatOptions&, const std::vector<Message>&, const nlohmann::json& tools,
                       const TextSink&, const std::atomic<bool>&);
Message chat_openai(const Provider&, const ChatOptions&, const std::vector<Message>&, const nlohmann::json& tools,
                    const TextSink&, const std::atomic<bool>&);

}  // namespace maic::detail
