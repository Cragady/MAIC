// Every model provider against a fake server that misbehaves on purpose. No network, no API keys.
#include "check.hpp"

#include "maic/llm.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace maic;
using nlohmann::json;

namespace {

struct Fake {
    httplib::Server srv;
    int port = 0;
    std::thread thread;
    std::string last_body;
    httplib::Headers last_headers;
    Fake() { port = srv.bind_to_any_port("127.0.0.1"); }
    void start() {
        thread = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
    }
    // Streams `chunks` as the response body, one write per chunk.
    void serve(const std::string& path, std::vector<std::string> chunks, int status = 200) {
        srv.Post(path, [this, chunks, status](const httplib::Request& req, httplib::Response& res) {
            last_body = req.body;
            last_headers = req.headers;
            res.status = status;
            res.set_chunked_content_provider("text/event-stream", [chunks](size_t, httplib::DataSink& sink) {
                for (const auto& c : chunks) sink.write(c.data(), c.size());
                sink.done();
                return true;
            });
        });
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    ~Fake() {
        srv.stop();
        if (thread.joinable()) thread.join();
    }
};

std::atomic<bool> no_cancel{false};

Message run(const Provider& p, const std::vector<Message>& msgs, std::string* streamed = nullptr,
            const std::atomic<bool>& cancel = no_cancel, const json& tools = json::array()) {
    return chat(p, {"test-model"}, msgs, tools, [&](std::string_view d, bool) { if (streamed) *streamed += d; }, cancel);
}

std::string sse(const json& j) {
    return "event: " + j.value("type", "x") + "\ndata: " + j.dump() + "\n\n";
}

const json kTools = json::array({{{"type", "function"},
                                   {"function", {{"name", "read_file"}, {"description", "Read"},
                                                 {"parameters", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}}}}}}});

}  // namespace

int main() {
    std::vector<Message> hello = {{"system", "be brief"}, {"user", "hi"}};

    section("model strings");
    auto provs = default_providers();
    auto [p1, m1] = resolve_model(provs, "anthropic/claude-opus-5-5");
    expect(p1.name == "anthropic" && m1 == "claude-opus-5-5", "anthropic/claude-opus-5-5 -> anthropic, claude-opus-5-5");
    auto [p2, m2] = resolve_model(provs, "hf.co/org/some-model:Q4");
    expect(p2.name == "ollama" && m2 == "hf.co/org/some-model:Q4", "an unknown prefix stays a whole Ollama model name");
    expect(!provs[0].remote() && provs[1].remote(), "ollama is local, anthropic is remote");

    section("ollama");
    {
        Fake f;
        f.serve("/api/chat", {R"({"message":{"content":"Hel)", R"(lo"}})" "\n" R"({"message":{"content":" there"}})" "\n",
                              "not json at all\n", R"({"done":true,"prompt_eval_count":120,"eval_count":7})" "\n"});
        f.start();
        std::string streamed;
        auto m = run({"ollama", "ollama", f.url()}, hello, &streamed);
        expect(m.content == "Hello there" && streamed == "Hello there", "lines split across chunks are reassembled; junk lines skipped");
        expect(m.usage.input == 120 && m.usage.output == 7 && m.usage.context == 16384, "Ollama token counts and the context size are reported");
    }
    {
        Fake f;
        f.serve("/api/chat", {R"({"message":{"tool_calls":[{"function":{"arguments":{"x":1}}},{"id":"c1","function":{"name":"list_dir","arguments":"oops"}}]}})" "\n"});
        f.start();
        auto m = run({"ollama", "ollama", f.url()}, hello);
        expect(m.tool_calls.size() == 1 && m.tool_calls[0].id == "c1" && m.tool_calls[0].arguments.is_object(),
               "nameless tool calls dropped, ids kept, non-object arguments become {}");
    }
    {
        Fake f;
        f.serve("/api/chat", {R"({"error":"model 'x' not found"})"}, 404);
        f.start();
        std::string msg;
        try { run({"ollama", "ollama", f.url()}, hello); } catch (const std::exception& e) { msg = e.what(); }
        expect(msg.find("not found") != std::string::npos, "an Ollama error is reported with its message");
    }
    {
        std::string msg;
        try { run({"ollama", "ollama", "http://127.0.0.1:1"}, hello); } catch (const std::exception& e) { msg = e.what(); }
        expect(msg.find("can't reach") != std::string::npos, "no server -> clear error");
    }

    section("anthropic");
    setenv("MAIC_TEST_KEY", "sk-test", 1);
    Provider anth{"anthropic", "anthropic", "", "MAIC_TEST_KEY", "", {{"max_tokens", 1000}, {"effort", "high"}, {"fallbacks", "default"}}};
    {
        Fake f;
        f.serve("/v1/messages", {
            sse({{"type", "message_start"}, {"message", {{"model", "test-model"}, {"usage", {{"input_tokens", 900}, {"cache_read_input_tokens", 100}}}}}}),
            sse({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "thinking"}, {"thinking", ""}}}}),
            sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "thinking_delta"}, {"thinking", "hmm"}}}}),
            sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "signature_delta"}, {"signature", "SIG"}}}}),
            sse({{"type", "content_block_start"}, {"index", 1}, {"content_block", {{"type", "text"}, {"text", ""}}}}),
            sse({{"type", "content_block_delta"}, {"index", 1}, {"delta", {{"type", "text_delta"}, {"text", "Reading "}}}}),
            sse({{"type", "content_block_start"}, {"index", 2}, {"content_block", {{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "read_file"}, {"input", json::object()}}}}),
            sse({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"pa"}}}}),
            sse({{"type", "content_block_delta"}, {"index", 2}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "th\": \"a.txt\"}"}}}}),
            sse({{"type", "content_block_stop"}, {"index", 2}}),
            sse({{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}, {"usage", {{"output_tokens", 42}}}}),
            sse({{"type", "message_stop"}}),
        });
        f.start();
        anth.base_url = f.url();
        std::string streamed;
        auto m = run(anth, hello, &streamed, no_cancel, kTools);
        expect(m.content == "Reading " && m.tool_calls.size() == 1 && m.tool_calls[0].id == "toolu_1" &&
               m.tool_calls[0].arguments.value("path", "") == "a.txt", "text, and a tool call whose input arrives in pieces");
        expect(m.usage.input == 1000 && m.usage.output == 42 && m.usage.context == 1000000, "Anthropic usage: input incl. cache reads, output from message_delta, 1M window");
        expect(m.raw_kind == "anthropic" && m.raw.size() == 3 && m.raw[0].value("signature", "") == "SIG",
               "the raw turn keeps the thinking block and its signature for replay");
        auto body = json::parse(f.last_body);
        expect(body["system"] == "be brief" && body["messages"].size() == 1, "the first system message becomes `system`");
        expect(body["tools"][0].contains("input_schema") && body["tools"][0].value("eager_input_streaming", false),
               "tools are converted to input_schema with eager input streaming");
        expect(body["output_config"]["effort"] == "high" && body["fallbacks"] == "default", "effort and refusal fallbacks are sent");
        expect(f.last_headers.find("x-api-key")->second == "sk-test" &&
               f.last_headers.find("anthropic-beta")->second.find("server-side-fallback-2026-07-01") != std::string::npos,
               "api key and beta headers are sent");

        // Replay: the assistant turn goes back byte-for-byte, results in one user message, append-only.
        std::vector<Message> hist = hello;
        hist.push_back(m);
        hist.push_back({"tool", "file text", {}, "read_file", "toolu_1"});
        hist.push_back({"system", "The mode is now auto."});
        run(anth, hist, nullptr, no_cancel, kTools);
        auto b2 = json::parse(f.last_body);
        const auto& msgs = b2["messages"];
        expect(msgs[1]["content"] == m.raw, "the assistant turn is replayed exactly as received");
        expect(msgs[2]["content"][0]["type"] == "tool_result" && msgs[2]["content"][0]["tool_use_id"] == "toolu_1",
               "tool results reference the tool_use id");
        expect(msgs[3]["role"] == "system", "a later system message is appended mid-conversation, not merged into `system`");
    }
    {
        Fake f;
        f.serve("/v1/messages", {
            sse({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "tool_use"}, {"id", "toolu_2"}, {"name", "read_file"}, {"input", json::object()}}}}),
            sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"path\": \"a.t"}}}}),
            sse({{"type", "content_block_stop"}, {"index", 0}}),
            sse({{"type", "message_delta"}, {"delta", {{"stop_reason", "max_tokens"}}}}),
        });
        f.start();
        anth.base_url = f.url();
        auto m = run(anth, hello, nullptr, no_cancel, kTools);
        expect(m.tool_calls.empty() && m.content.find("max_tokens") != std::string::npos, "a tool call cut off at max_tokens is never run");
    }
    {
        Fake f;
        f.serve("/v1/messages", {
            sse({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "tool_use"}, {"id", "toolu_3"}, {"name", "read_file"}, {"input", json::object()}}}}),
            sse({{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"path\": oops}"}}}}),
            sse({{"type", "content_block_stop"}, {"index", 0}}),
            sse({{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}}),
        });
        f.start();
        anth.base_url = f.url();
        auto m = run(anth, hello, nullptr, no_cancel, kTools);
        expect(m.tool_calls.size() == 1 && m.tool_calls[0].arguments.contains("_maic_invalid_input"),
               "invalid streamed tool JSON is flagged, not run with a half-parsed input");
    }
    {
        Fake f;
        f.serve("/v1/messages", {sse({{"type", "message_delta"}, {"delta", {{"stop_reason", "refusal"}}}})});
        f.start();
        anth.base_url = f.url();
        auto m = run(anth, hello);
        expect(m.content.find("declined") != std::string::npos, "a refusal is reported");
    }
    {
        Fake f;
        f.serve("/v1/messages", {R"({"type":"error","error":{"type":"invalid_request_error","message":"bad thing"}})"}, 400);
        f.start();
        anth.base_url = f.url();
        std::string msg;
        try { run(anth, hello); } catch (const std::exception& e) { msg = e.what(); }
        expect(msg.find("400") != std::string::npos && msg.find("bad thing") != std::string::npos, "an API error shows status and message");
    }
    {
        Provider nokey = anth;
        nokey.api_key_env = "MAIC_TEST_NO_SUCH_KEY";
        std::string msg;
        try { run(nokey, hello); } catch (const std::exception& e) { msg = e.what(); }
        expect(msg.find("MAIC_TEST_NO_SUCH_KEY") != std::string::npos, "a missing key names the variable to set");
        nokey.api_key_command = "echo sk-from-command";
        expect(nokey.api_key() == "sk-from-command", "api_key_command supplies the key");
    }

    section("openai-compatible");
    Provider oai{"deepseek", "openai", "", "MAIC_TEST_KEY", "", json::object()};
    {
        Fake f;
        f.serve("/v1/chat/completions", {
            "data: " + json{{"choices", {{{"delta", {{"reasoning_content", "think"}}}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", {{{"delta", {{"content", "Hi "}}}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", {{{"delta", {{"tool_calls", {{{"index", 0}, {"id", "call_9"}, {"function", {{"name", "read_file"}, {"arguments", "{\"pa"}}}}}}}}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", {{{"delta", {{"tool_calls", {{{"index", 0}, {"function", {{"arguments", "th\":\"b\"}"}}}}}}}}, {"finish_reason", "tool_calls"}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", json::array()}, {"usage", {{"prompt_tokens", 55}, {"completion_tokens", 9}}}}.dump() + "\n\n",
            "data: [DONE]\n\n",
        });
        f.start();
        oai.base_url = f.url() + "/v1";  // a path prefix, like https://openrouter.ai/api/v1
        std::string streamed;
        auto m = run(oai, hello, &streamed, no_cancel, kTools);
        expect(m.content == "Hi " && streamed == "thinkHi ", "content and reasoning stream");
        expect(m.usage.input == 55 && m.usage.output == 9 && json::parse(f.last_body)["stream_options"]["include_usage"] == true,
               "OpenAI-style usage is requested and parsed from the final chunk");
        expect(m.tool_calls.size() == 1 && m.tool_calls[0].id == "call_9" && m.tool_calls[0].arguments.value("path", "") == "b",
               "a tool call assembled from pieces");
        expect(f.last_headers.find("Authorization")->second == "Bearer sk-test", "bearer auth is sent");

        std::vector<Message> hist = hello;
        hist.push_back(m);
        hist.push_back({"tool", "b's text", {}, "read_file", "call_9"});
        run(oai, hist, nullptr, no_cancel, kTools);
        auto b = json::parse(f.last_body);
        const auto& msgs = b["messages"];
        expect(msgs[2]["tool_calls"][0]["function"]["arguments"].is_string() && msgs[3]["tool_call_id"] == "call_9",
               "replay sends arguments as a JSON string and results by tool_call_id");
    }
    {
        // A conversation that switches from Ollama (no call ids) to an OpenAI-style provider mid-way.
        Fake f;
        f.serve("/chat/completions", {"data: " + json{{"choices", {{{"delta", {{"content", "ok"}}}}}}}.dump() + "\n\ndata: [DONE]\n\n"});
        f.start();
        Provider local{"lmstudio", "openai", f.url(), "", "", json::object()};
        std::vector<Message> hist = hello;
        hist.push_back({"assistant", "", {{"", "read_file", {{"path", "a"}}}}});
        hist.push_back({"tool", "text", {}, "read_file", ""});
        auto m = run(local, hist);
        auto b = json::parse(f.last_body);
        std::string id = b["messages"][2]["tool_calls"][0]["id"];
        expect(m.content == "ok" && !id.empty() && b["messages"][3]["tool_call_id"] == id,
               "missing ids from another provider are generated and matched");
        expect(f.last_headers.find("Authorization") == f.last_headers.end(), "no key configured -> no auth header (local servers)");
    }

    section("cancel");
    {
        Fake f;
        f.srv.Post("/api/chat", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::seconds(5));  // like a model still loading
            res.set_content("{}\n", "application/x-ndjson");
        });
        f.start();
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel = true;
        });
        auto t0 = std::chrono::steady_clock::now();
        bool cancelled = false;
        try {
            run({"ollama", "ollama", f.url()}, hello, nullptr, cancel);
        } catch (const Cancelled&) {
            cancelled = true;
        } catch (const std::exception&) {
        }
        canceller.join();
        expect(cancelled && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2),
               "cancel works while the server hasn't sent anything yet");
    }

    section("request encoding");
    {
        Fake f;
        f.serve("/api/chat", {R"({"message":{"content":"ok"}})" "\n"});
        f.start();
        std::vector<Message> msgs = {{"user", "hi"}, {"tool", std::string("binary \xff\xfe\x80 output"), {}, "read_file"}};
        bool ok = false;
        try { ok = run({"ollama", "ollama", f.url()}, msgs).content == "ok"; } catch (const std::exception&) {}
        expect(ok && json::parse(f.last_body, nullptr, false).is_object(), "invalid UTF-8 in history is sent as valid JSON");
    }

    return finish();
}
