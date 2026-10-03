#pragma once
// A fake DeepSeek API for llm_test and agent_test, from its public docs (api-docs.deepseek.com, the pricing page and
// guides/thinking_mode, checked 2026-10-02; docs/references/deepseek.md): POST /chat/completions at the root, OpenAI's
// stream format with reasoning_content before content and `content: null` beside it, keep-alive comments, usage on the
// final chunk only (null before), DeepSeek's usage fields and finish reasons. It refuses what the real one refuses: a
// wrong key (401), a `thinking` that is not {"type": "enabled" | "disabled"}, a request with tools whose earlier
// assistant turns lack their reasoning_content, a picture sent to deepseek-v4-pro, stream_options without stream,
// tool_choice required or named while thinking. It is stricter than the real API where MAID must not send misleading
// or extra data: a top_p while thinking outside 0.95 to 1.0 (the API raises it), any top_p without thinking (the API
// fixes it at 1.0), and any field DeepSeek does not document (the API's behaviour is unstated; MAID sends only standard
// fields, no ids or telemetry). GET /models answers with the documented per-model facts (window, output cap, effort
// levels), or with `models_status` when that is not 200. Nothing here reaches the network.
#include "maid/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct FakeDeepSeek {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::mutex mu;
    std::string key = "sk-fake-deepseek-0123456789abcdef";
    std::vector<nlohmann::json> requests;  // every body, refused ones included
    std::vector<std::string> refused;      // why each refused one was
    std::function<nlohmann::json(const nlohmann::json&)> tool_call_for;  // {"name", "arguments"} for this request, or null for text
    std::function<std::string(const nlohmann::json&)> reply;            // the text reply; "done" when unset
    std::vector<std::pair<int, std::string>> failures;  // answered in order, one per request, before anything else
    bool break_mid_stream = false;  // the next reply sends one piece of text, then an error event, and ends
    std::string finish;             // the next reply ends with this finish_reason (a tool call's arguments cut off for "length")
    int empty_left = 0;             // this many replies are empty: no text, reasoning or tool call, finish_reason stop
    std::string served_as;          // the model replies say they come from ("" = the one asked for), as a silent re-route would
    int served = 0;
    int models_status = 200;        // what GET /models answers with
    int models_reads = 0;
    long flash_context = 1048576;   // what GET /models says deepseek-flash's window is
    int delay_ms = 0;               // each chat request is held this long before it is answered
    std::atomic<int> active{0}, max_active{0};  // chat requests open at once, and the most there were

    // GET /models as the documentation's example gives it: every field MAID reads, and some it does not.
    static nlohmann::json model_list(long flash_context) {
        auto model = [](const std::string& id, long context, nlohmann::json modalities, const std::string& prompt_update) {
            return nlohmann::json{{"id", id}, {"object", "model"}, {"owned_by", "deepseek"}, {"context_window", context}, {"max_output_tokens", 393216},
                                  {"input_modalities", modalities}, {"effort", {{"supported_levels", {"low", "high", "max"}}, {"default_level", "high"}}},
                                  {"api_capabilities", {{"anthropic_messages", {{"system_prompt_update", prompt_update}}}}}};
        };
        return {{"object", "list"}, {"data", {model("deepseek-flash", flash_context, {"text", "image"}, "in-history"), model("deepseek-v4-pro", 1048576, {"text"}, "leading-only")}}};
    }

    static nlohmann::json error(const std::string& message, const std::string& type) {
        return {{"error", {{"message", message}, {"type", type}, {"param", nullptr}, {"code", "invalid_request_error"}}}};
    }

    // Why the real API would answer 400, or "" when it takes the request.
    static std::string refusal(const nlohmann::json& b) {
        std::string model = b.value("model", "");
        if (model != "deepseek-flash" && model != "deepseek-v4-pro") return "Model Not Exist";
        static const std::set<std::string> documented = {"model", "messages", "thinking", "reasoning_effort", "max_tokens", "temperature", "top_p", "frequency_penalty",
                                                         "presence_penalty", "stop", "stream", "stream_options", "logprobs", "top_logprobs", "response_format", "tools", "tool_choice"};
        for (const auto& [k, v] : b.items()) {
            if (!documented.count(k)) return "unknown field " + k;
        }
        if (b.contains("stream_options") && b.value("stream", false) != true) return "stream_options is only allowed with stream: true";
        bool thinking = true;  // on unless the request turns it off
        if (b.contains("thinking")) {
            const auto& t = b["thinking"];
            if (!t.is_object() || !t.contains("type") || (t["type"] != "enabled" && t["type"] != "disabled")) return "thinking.type must be enabled or disabled";
            thinking = t["type"] == "enabled";
        }
        if (thinking && b.contains("top_p") && (!b["top_p"].is_number() || b["top_p"] < 0.95 || b["top_p"] > 1.0)) return "top_p must be between 0.95 and 1.0 in thinking mode";
        if (!thinking && b.contains("top_p")) return "(fake) top_p is fixed at 1.0 without thinking: sending it misleads";
        if (thinking && b.contains("tool_choice") && b["tool_choice"] != "auto" && b["tool_choice"] != "none") return "tool_choice required or a named function is not supported in thinking mode";
        if (b.value("max_tokens", 0) > 393216) return "max_tokens must be at most 393216";
        bool tools = b.contains("tools") && b["tools"].is_array() && !b["tools"].empty();
        for (const auto& m : b["messages"]) {
            if (tools && m["role"] == "assistant" && !(m.contains("reasoning_content") && m["reasoning_content"].is_string())) {
                return "The reasoning_content in the thinking mode must be passed back to the API.";
            }
            if (model == "deepseek-v4-pro" && m["role"] == "user" && m["content"].is_array()) {
                for (const auto& part : m["content"]) {
                    if (part.value("type", "") == "image_url") return "deepseek-v4-pro does not support image input";
                }
            }
        }
        return "";
    }

    static std::string chunk(const std::string& model, const nlohmann::json& delta, const nlohmann::json& finish = nullptr, const nlohmann::json& usage = nullptr) {
        nlohmann::json c = {{"id", "fake-ds"}, {"object", "chat.completion.chunk"}, {"created", 1790000000}, {"model", model}, {"system_fingerprint", "fp_fake"},
                            {"choices", {{{"index", 0}, {"delta", delta}, {"logprobs", nullptr}, {"finish_reason", finish}}}}, {"usage", usage}};
        return "data: " + c.dump() + "\n\n";
    }

    FakeDeepSeek() {
        port = srv.bind_to_any_port("127.0.0.1");
        srv.Get("/models", [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard lock(mu);
            ++models_reads;
            if (req.get_header_value("Authorization") != "Bearer " + key) {
                res.status = 401;
                res.set_content(error("Authentication Fails", "authentication_error").dump(), "application/json");
                return;
            }
            res.status = models_status;
            res.set_content(models_status == 200 ? model_list(flash_context).dump() : error("Service Unavailable", "server_error").dump(), "application/json");
        });
        srv.Post("/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            int now = ++active;
            for (int m = max_active; now > m && !max_active.compare_exchange_weak(m, now);) {}
            if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            struct Leave {
                std::atomic<int>& n;
                ~Leave() { --n; }
            } leave{active};
            nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            std::lock_guard lock(mu);
            requests.push_back(body);
            std::string sent = req.get_header_value("Authorization");
            if (sent != "Bearer " + key) {
                // The real API masks all but the end of the key; this one echoes it whole, so a test sees MAID's own redaction.
                res.status = 401;
                res.set_content(error("Authentication Fails, Your api key: " + sent.substr(sent.find(' ') + 1) + " is invalid", "authentication_error").dump(), "application/json");
                return;
            }
            if (!failures.empty()) {
                res.status = failures.front().first;
                res.set_content(failures.front().second, "application/json");
                failures.erase(failures.begin());
                return;
            }
            std::string why = body.is_object() ? refusal(body) : "invalid JSON";
            if (!why.empty()) {
                refused.push_back(why);
                res.status = 400;
                res.set_content(error(why, "invalid_request_error").dump(), "application/json");
                return;
            }
            int n = ++served;
            std::string model = served_as.empty() ? body["model"].get<std::string>() : served_as;
            bool thinking = !body.contains("thinking") || body["thinking"]["type"] == "enabled";
            nlohmann::json call = tool_call_for ? tool_call_for(body) : nlohmann::json();
            std::string text = reply ? reply(body) : "done";
            bool broken = std::exchange(break_mid_stream, false);
            std::string end = std::exchange(finish, "");
            bool empty = empty_left > 0 && empty_left--;
            std::string out = ": keep-alive\n\n" + chunk(model, {{"role", "assistant"}, {"content", ""}, {"reasoning_content", nullptr}});
            if (empty) {
                out += chunk(model, {{"content", ""}, {"reasoning_content", nullptr}}, "stop") + "data: [DONE]\n\n";
                res.set_content(out, "text/event-stream");
                return;
            }
            if (thinking) {
                for (const auto& piece : std::vector<std::string>{"thought ", std::to_string(n)}) out += chunk(model, {{"content", nullptr}, {"reasoning_content", piece}});
            }
            if (broken) {
                out += chunk(model, {{"content", "partial "}, {"reasoning_content", nullptr}});
                out += "data: " + error("Service is too busy", "server_error").dump() + "\n\n";
            } else {
                if (!call.is_null()) {
                    // The first fragment carries the id and name, the rest only arguments, assembled by index.
                    std::string args = call["arguments"].dump();
                    if (end == "length") args = args.substr(0, args.size() / 2);
                    nlohmann::json first = {{"index", 0}, {"id", "call_ds_" + std::to_string(n)}, {"type", "function"}, {"function", {{"name", call["name"]}, {"arguments", ""}}}};
                    out += chunk(model, {{"content", nullptr}, {"reasoning_content", nullptr}, {"tool_calls", {first}}});
                    for (size_t i = 0; i < args.size(); i += 7) {
                        out += chunk(model, {{"content", nullptr}, {"reasoning_content", nullptr}, {"tool_calls", {{{"index", 0}, {"function", {{"arguments", args.substr(i, 7)}}}}}}});
                    }
                } else {
                    for (size_t i = 0; i < text.size(); i += 5) out += chunk(model, {{"content", text.substr(i, 5)}, {"reasoning_content", nullptr}});
                }
                // Usage rides on the chunk that carries finish_reason, not on one of its own.
                nlohmann::json usage = {{"prompt_tokens", 120}, {"completion_tokens", 30}, {"total_tokens", 150}, {"prompt_cache_hit_tokens", 64},
                                        {"prompt_cache_miss_tokens", 56}, {"prompt_tokens_details", {{"cached_tokens", 64}}},
                                        {"completion_tokens_details", {{"reasoning_tokens", thinking ? 12 : 0}}}};
                out += chunk(model, {{"content", ""}, {"reasoning_content", nullptr}}, !end.empty() ? end : call.is_null() ? "stop" : "tool_calls", usage);
                out += "data: [DONE]\n\n";
            }
            res.set_content(out, "text/event-stream");
        });
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    ~FakeDeepSeek() {
        srv.stop();
        thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    nlohmann::json last() {
        std::lock_guard lock(mu);
        return requests.empty() ? nlohmann::json() : requests.back();
    }
};

}  // namespace
