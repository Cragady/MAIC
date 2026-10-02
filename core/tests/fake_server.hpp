#pragma once
// A fake OpenAI-compatible server for the agent and engine tests.
#include "maic/llm.hpp"

#include "maic/http.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using nlohmann::json;
using maic::Provider;

// An OpenAI-compatible /v1/chat/completions that answers every chat with an SSE stream of the last user
// message's text, echoed four characters at a time. mid_system keeps later system messages as system turns, so
// the tests can look for them; context_window matches the shipped llamacpp provider.
//
// Tests that act in the middle of a reply (cancel, deliver-now) never time their move against the stream:
// with hold_left > 0 the next reply writes its first chunk and then idles, sending empty keepalive chunks
// until the agent hangs up (the write fails) or ten seconds pass, so a broken cancel fails instead of hanging.
// wait_streaming(n) blocks until the n-th reply has written its first chunk.
struct FakeServer {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::vector<json> requests;
    std::mutex mu;
    std::condition_variable cv;
    int hold_left = 0;
    int streaming = 0;
    int usage_input = 0;  // reported as prompt_tokens in the final usage chunk when set
    json tool_call;       // when set and calls_left > 0, the reply is this one tool call ({"name", "arguments"})
    int calls_left = 0;
    std::function<json(const json&)> tool_call_for;  // when set: the tool call for this request, or null for the echo
    bool usage_on_calls = false;  // report usage_input on tool-call replies too (llama-server does)
    std::function<std::string(const json&)> reply;  // when set and non-empty for a request, replaces the echo
    int fail_left = 0;      // answer this many requests with fail_status / fail_body first
    int fail_status = 400;
    std::string fail_body;
    std::function<bool(const json&)> fail_when;  // when set: also fail every request it is true for
    bool unique_call_ids = false;  // each tool call gets its own id (call_1, call_2, ...) instead of call_1 every time
    int calls_made = 0;

    static std::string text_of(const json& content) {
        if (content.is_string()) return content;
        std::string out;
        for (const auto& part : content) {
            if (part.value("type", "") == "text") out += part.value("text", "");
        }
        return out;
    }
    // One chunk as OpenAI's CreateChatCompletionStreamResponse shapes it, and as llama-server sends it: id, object,
    // created and model on every chunk, finish_reason null until the last. Every distinct chunk any FakeServer sends
    // is kept in `sent`, and the suite checks them against the pinned schema at the end.
    static inline std::mutex sent_mu;
    static inline std::set<std::string> sent;
    static std::string event(const std::string& model, const json& delta, const json& finish = nullptr, const json& usage = nullptr) {
        json c = {{"id", "chatcmpl-fake"}, {"object", "chat.completion.chunk"}, {"created", 1767225600}, {"model", model},
                  {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
        if (!usage.is_null()) c["usage"] = usage;
        std::string line = c.dump();
        {
            std::lock_guard lock(sent_mu);
            sent.insert(line);
        }
        return "data: " + line + "\n\n";
    }
    static json usage_of(int input) { return {{"prompt_tokens", input}, {"completion_tokens", 5}, {"total_tokens", input + 5}}; }

    void wait_streaming(int n) {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return streaming >= n; });
    }

    FakeServer() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = json::parse(req.body);
            bool hold = false;
            {
                std::lock_guard lock(mu);
                requests.push_back(body);
                if (hold_left > 0) {
                    --hold_left;
                    hold = true;
                }
            }
            std::string last;
            for (const auto& m : body["messages"]) {
                if (m["role"] == "user") last = text_of(m["content"]);
            }
            if (fail_left > 0 || (fail_when && fail_when(body))) {
                if (fail_left > 0) --fail_left;
                res.status = fail_status;
                res.set_content(fail_body, "application/json");
                return;
            }
            std::string echo = "echo: " + last;
            if (reply) {
                std::string r = reply(body);
                if (!r.empty()) echo = r;
            }
            int usage = usage_input;
            bool call_usage = usage_on_calls;
            json call;
            if (tool_call_for) {
                call = tool_call_for(body);
            } else if (!tool_call.is_null() && calls_left > 0) {
                --calls_left;
                call = tool_call;
            }
            std::string call_id = "call_1";
            if (!call.is_null() && unique_call_ids) {
                std::lock_guard lock(mu);
                call_id = "call_" + std::to_string(++calls_made);
            }
            std::string model = body.value("model", "");
            res.set_chunked_content_provider("text/event-stream", [this, echo, hold, usage, call, call_usage, model, call_id](size_t, httplib::DataSink& sink) {
                auto write = [&](const std::string& s) { return sink.write(s.data(), s.size()); };
                // The first chunk is out: tell a waiting test, and idle here while held.
                auto started = [&] {
                    std::unique_lock lock(mu);
                    ++streaming;
                    cv.notify_all();
                    if (!hold) return true;
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (std::chrono::steady_clock::now() < deadline) {
                        cv.wait_for(lock, std::chrono::milliseconds(20));
                        if (!write(event(model, json::object()))) return false;
                    }
                    return true;
                };
                if (!call.is_null()) {
                    json tc = {{"index", 0}, {"id", call_id}, {"type", "function"},
                               {"function", {{"name", call["name"]}, {"arguments", call["arguments"].dump()}}}};
                    write(event(model, {{"content", ""}, {"tool_calls", {tc}}}));
                    if (!started()) return false;
                    write(event(model, json::object(), "tool_calls", usage && call_usage ? usage_of(usage) : json()));
                    write("data: [DONE]\n\n");
                    sink.done();
                    return true;
                }
                for (size_t i = 0; i < echo.size(); i += 4) {
                    if (!write(event(model, {{"content", echo.substr(i, 4)}}))) return false;
                    if (i == 0 && !started()) return false;
                }
                write(event(model, json::object(), "stop", usage ? usage_of(usage) : json()));
                write("data: [DONE]\n\n");
                sink.done();
                return true;
            });
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeServer() {
        srv.stop();
        thread.join();
    }
    Provider provider() const {
        return {"fake", "openai", "http://127.0.0.1:" + std::to_string(port) + "/v1", "", "", {{"mid_system", true}, {"context_window", 16384}}};
    }
};

}  // namespace
