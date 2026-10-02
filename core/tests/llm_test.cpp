// Every model provider against a fake server that misbehaves on purpose. No network, no API keys.
#include "check.hpp"

#include "maic/llm.hpp"

#include "maic/http.hpp"

#include "fake_claude.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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
    expect(p2.name == "llamacpp" && m2 == "hf.co/org/some-model:Q4", "an unknown prefix stays a whole model name on the first provider");
    auto by_name = [&](const std::string& n) { return *std::find_if(provs.begin(), provs.end(), [&](const Provider& p) { return p.name == n; }); };
    expect(!by_name("llamacpp").remote() && by_name("anthropic").remote(), "llamacpp is local, anthropic is remote");
    {
        // Local means the URL's host, parsed, is loopback; text that only looks local anywhere else is remote.
        auto remote = [](const std::string& url) { return Provider{"p", "openai", url, "", "", json::object()}.remote(); };
        for (const char* url : {"http://127.0.0.1:8081/v1", "http://localhost:8082", "http://[::1]:8081/v1", "http://127.1.2.3/v1", "HTTP://LocalHost:9/x",
                                "http://user@127.0.0.1:8081/v1", "unix:/run/user/1000/llama.sock"}) {
            expect(!remote(url), std::string("local: ") + url);
        }
        for (const char* url : {"https://api.anthropic.com", "https://example.com/x?y=://127.0.0.1", "http://127.0.0.1.attacker.example/",
                                "http://127.0.0.1@evil.example/", "http://localhost.evil.example/", "http://evil.example/://localhost", "http://evil.example#://[::1]",
                                "http://0177.0.0.1/", "http://2130706433/", "http://127.0.0.1\\@evil.example/", "ftp://127.0.0.1/", "http://[::1]evil/", ""}) {
            expect(remote(url), std::string("remote: ") + url);
        }
        expect(loopback_host("127.0.0.1") && loopback_host("[::1]") && !loopback_host("127.0.0.1.attacker.example") && !loopback_host("128.0.0.1"),
               "loopback_host: a dotted quad in 127.0.0.0/8, ::1 or localhost only");
    }
    {
        const Provider& side = by_name("llamacpp-2");
        expect(side.kind == "openai" && side.base_url == "http://127.0.0.1:8082/v1" && !side.remote() && side.options.value("thinking_controls", false) && side.options.value("context_window", 0) == 8192,
               "llamacpp-2 is shipped: OpenAI-compatible on 8082, local, thinking controls, an 8k window");
        auto [ps, ms] = resolve_model(provs, "llamacpp-2/Qwen3.5-4B-Q4_K_M");
        expect(ps.name == "llamacpp-2" && ms == "Qwen3.5-4B-Q4_K_M", "llamacpp-2/NAME reaches the side server");
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
        // llama-server b11284's stream as written in the fixture (jsonschema_test checks each chunk against OpenAI's
        // schema): the opening role chunk with null content, reasoning_content, content, a tool call in two pieces,
        // finish_reason tool_calls, then usage and timings on a chunk with no choices.
        std::ifstream in(std::string(MAIC_FIXTURES) + "/llamacpp-b11284-chat.sse");
        std::stringstream stream;
        stream << in.rdbuf();
        Fake f;
        f.serve("/v1/chat/completions", {stream.str()});
        f.start();
        Provider llama{"llamacpp", "openai", f.url() + "/v1", "", "", json::object()};
        std::string streamed;
        auto m = run(llama, hello, &streamed, no_cancel, kTools);
        expect(m.content == "Her ears stay a third of her height." && streamed == "The user wants the fennec ear sizing note." + m.content,
               "llama.cpp's recorded stream: reasoning and content");
        expect(m.tool_calls.size() == 1 && m.tool_calls[0].name == "read_file" && m.tool_calls[0].arguments.value("path", "") == "docs/mascot.md",
               "llama.cpp's recorded stream: the tool call");
        expect(m.usage.input == 412 && m.usage.output == 38, "llama.cpp's recorded stream: usage from the empty-choices chunk");
    }
    {
        // The shapes llama.cpp sends that OpenAI's do not allow: the adapter rules rewrite them and the caller hears
        // each rule once per call, also when the call ends in the error.
        std::ifstream in(std::string(MAIC_FIXTURES) + "/llamacpp-b11284-oddities.sse");
        std::stringstream stream;
        stream << in.rdbuf();
        Fake f;
        f.serve("/v1/chat/completions", {stream.str()});
        f.serve("/v2/chat/completions", {R"({"error":{"code":400,"message":"the request exceeds the available context size","type":"exceed_context_size_error","n_prompt_tokens":20000,"n_ctx":16384}})"}, 400);
        f.start();
        std::map<std::string, int> heard;
        ChatOptions opt{"test-model"};
        opt.normalized = [&](const std::string& rule, int n) { heard[rule] += n; };
        std::string streamed, what;
        try {
            chat({"llamacpp", "openai", f.url() + "/v1", "", "", json::object()}, opt, hello, json::array(), [&](std::string_view d, bool) { streamed += d; }, no_cancel);
        } catch (const std::runtime_error& e) {
            what = e.what();
        }
        expect(streamed == "Her" && what == "llamacpp: the model crashed", "llama.cpp's logprobs chunk is read and its mid-stream error reported: " + what);
        expect(heard == std::map<std::string, int>{{"error_code_string", 1}, {"error_param_null", 1}, {"logprobs_refusal_null", 1}}, "each adapter rule is heard, with its count");
        heard.clear();
        opt.retries = 0;
        int status = 0;
        try {
            chat({"llamacpp", "openai", f.url() + "/v2", "", "", json::object()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel);
        } catch (const ApiError& e) {
            status = e.status;
            what = e.what();
        }
        expect(status == 400 && what.find("exceeds the available context size") != std::string::npos, "an error status still reads as before: " + what);
        expect(heard == std::map<std::string, int>{{"error_code_string", 1}, {"error_param_null", 1}}, "the error body of a failed request goes through the rules too");
    }
    {
        // The same server captured live (2026-10-02): a tool call with thinking, a logprobs chunk, and the router's
        // proxy error when the model went away mid-stream, which must end the call as an error, not a short reply.
        auto fixture = [](const std::string& name) {
            std::ifstream in(std::string(MAIC_FIXTURES) + "/" + name);
            std::stringstream stream;
            stream << in.rdbuf();
            return stream.str();
        };
        Fake f;
        f.serve("/a/chat/completions", {fixture("llamacpp-b11284-captured-chat.sse")});
        f.serve("/b/chat/completions", {fixture("llamacpp-b11284-captured-logprobs.sse")});
        f.serve("/c/chat/completions", {fixture("llamacpp-b11284-captured-proxy-error.sse")});
        f.start();
        std::map<std::string, int> heard;
        ChatOptions opt{"test-model"};
        opt.normalized = [&](const std::string& rule, int n) { heard[rule] += n; };
        opt.retries = 0;
        std::string streamed;
        auto m = chat({"llamacpp", "openai", f.url() + "/a", "", "", json::object()}, opt, hello, kTools, [&](std::string_view d, bool) { streamed += d; }, no_cancel);
        expect(m.content.empty() && streamed.rfind("The user wants me to list the files", 0) == 0 && m.tool_calls.size() == 1 &&
                   m.tool_calls[0].name == "list_dir" && m.tool_calls[0].arguments == json{{"path", "src"}} && m.usage.input == 293 && m.usage.output == 63,
               "the captured stream: thinking, then the tool call, and usage");
        expect(heard.empty(), "no adapter rule touches the captured tool-call stream");
        m = chat({"llamacpp", "openai", f.url() + "/b", "", "", json::object()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel);
        expect(m.content == "Red" && heard == std::map<std::string, int>{{"logprobs_refusal_null", 1}}, "the captured logprobs chunk is read and normalized once");
        heard.clear();
        streamed.clear();
        std::string what;
        try {
            chat({"llamacpp", "openai", f.url() + "/c", "", "", json::object()}, opt, hello, json::array(), [&](std::string_view d, bool) { streamed += d; }, no_cancel);
        } catch (const std::runtime_error& e) {
            what = e.what();
        }
        expect(!streamed.empty() && what == "llamacpp: proxy error: Failed to read connection", "the router's proxy error ends the call as an error: " + what);
    }
    {
        // The code names the software behind the provider, not what it is called here: both shipped llama servers
        // say maic_llamacpp_500; a provider without `upstream` falls back to its own name.
        auto code_for = [](const Provider& p) {
            json body = {{"error", {{"code", 500}, {"message", "the model crashed"}, {"type", "server_error"}}}};
            normalize_openai(body, p.upstream_name());
            return body["error"]["code"].get<std::string>() + " " + body["error"]["maic"]["upstream"]["provider"].get<std::string>();
        };
        std::map<std::string, std::string> shipped;
        for (const auto& p : default_providers()) shipped[p.name] = code_for(p);
        expect(shipped["llamacpp"] == "maic_llamacpp_500 llamacpp" && shipped["llamacpp-2"] == "maic_llamacpp_500 llamacpp",
               "llamacpp and llamacpp-2 both give maic_llamacpp_500, upstream llamacpp: " + shipped["llamacpp-2"]);
        Provider lab{"lab", "openai", "http://127.0.0.1:9/v1", "", "", json::object()};
        expect(code_for(lab) == "maic_lab_500 lab", "a provider without upstream falls back to its name");
        lab.upstream = "vllm";
        expect(code_for(lab) == "maic_vllm_500 vllm", "a provider with upstream names that instead");
    }
    {
        Fake f;
        f.serve("/v1/chat/completions", {"data: {\"choices\":[{\"delta\":{\"content\":\"Hel", "lo\"}}]}\n\ndata: {\"choices\":[{\"delta\":{\"content\":\" there\"}}]}\n\n",
                                         "not an event at all\n", "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{}\"}},"
                                         "{\"index\":1,\"id\":\"c1\",\"function\":{\"name\":\"list_dir\",\"arguments\":\"oops\"}}]}}]}\n\n",
                                         "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":7}}\n\ndata: [DONE]\n\n"});
        f.start();
        Provider local{"lab", "openai", f.url() + "/v1", "", "", {{"context_window", 16384}}};
        std::string streamed;
        auto m = run(local, hello, &streamed);
        expect(m.content == "Hello there" && streamed == "Hello there", "events split across chunks are reassembled; junk lines skipped");
        expect(m.usage.input == 120 && m.usage.output == 7 && m.usage.context == 16384, "token counts come from the usage chunk, the context size from the provider's context_window");
        expect(m.tool_calls.size() == 1 && m.tool_calls[0].id == "c1" && m.tool_calls[0].arguments.contains("_maic_invalid_input"),
               "nameless tool calls dropped, ids kept, unparseable arguments flagged");
    }
    {
        // A history whose tool calls carry no ids (an older transcript) replayed to an OpenAI-style provider.
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
    {
        // DeepSeek's thinking models reject a tool-carrying request whose earlier assistant turns lack their
        // reasoning_content; other OpenAI-style servers get plain turns.
        Fake f;
        f.serve("/chat/completions", {
            "data: " + json{{"choices", {{{"delta", {{"reasoning_content", "think "}}}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", {{{"delta", {{"reasoning_content", "hard"}}}}}}}.dump() + "\n\n",
            "data: " + json{{"choices", {{{"delta", {{"content", "ok"}}}}}}}.dump() + "\n\ndata: [DONE]\n\n",
        });
        f.start();
        Provider ds{"deepseek", "openai", f.url(), "", "", json::object()};
        auto sink = [](std::string_view, bool) {};
        Message m = chat(ds, {"deepseek-reasoner"}, hello, kTools, sink, no_cancel);
        expect(m.raw_kind == "openai" && m.raw.value("reasoning_content", "") == "think hard", "a deepseek reply keeps its reasoning as raw provider content");
        std::vector<Message> hist = hello;
        hist.push_back(m);
        hist.push_back({"user", "and then?"});
        chat(ds, {"deepseek-reasoner"}, hist, kTools, sink, no_cancel);
        auto b = json::parse(f.last_body);
        expect(b["messages"][2]["reasoning_content"] == "think hard" && b["messages"][2]["content"] == "ok", "and replays it to deepseek models");
        chat(ds, {"some-other-model"}, hist, kTools, sink, no_cancel);
        b = json::parse(f.last_body);
        expect(!b["messages"][2].contains("reasoning_content"), "but not to other models on the same kind of provider");
        Message other = chat(ds, {"some-other-model"}, hello, kTools, sink, no_cancel);
        expect(other.raw_kind.empty() && other.raw.is_null(), "other models' reasoning is streamed, not stored");
    }

    section("title generation");
    {
        Fake f;
        f.serve("/v1/chat/completions", {
            "data: " + json{{"choices", {{{"delta", {{"content", "<think>short</think>\"Fennec ear sizing.\"\n"}}}}}}}.dump() + "\n\n",
            "data: [DONE]\n\n",
        });
        f.start();
        Provider p{"local", "openai", f.url() + "/v1", "", "", json::object()};
        expect(generate_title(p, "tiny", "How big should the ears be?") == "Fennec ear sizing", "thinking, quotes, a trailing stop and newline are stripped");
        auto b = json::parse(f.last_body);
        expect(b["model"] == "tiny" && b["messages"][0]["role"] == "system" && b["messages"][1]["content"] == "How big should the ears be?",
               "the first prompt goes as the user message under a titling instruction");
    }
    {
        Fake f;
        f.serve("/v1/chat/completions", {"data: " + json{{"choices", {{{"delta", {{"content", "one\ntwo"}}}}}}}.dump() + "\n\n", "data: [DONE]\n\n"});
        f.start();
        Provider p{"local", "openai", f.url() + "/v1", "", "", json::object()};
        expect(generate_title(p, "tiny", "x").empty(), "a reply over several lines is not a title");
    }

    section("retry with backoff");
    {
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            if (++calls < 3) {
                res.status = 503;
                res.set_header("Retry-After", "0");
                res.set_content("{\"error\":\"overloaded\"}", "application/json");
                return;
            }
            res.set_content("data: {\"choices\":[{\"delta\":{\"content\":\"finally\"}}]}\n\ndata: [DONE]\n\n", "text/event-stream");
        });
        f.start();
        std::vector<std::string> notices;
        ChatOptions opt{"test-model"};
        opt.retry_base_ms = 10;
        opt.notice = [&](const std::string& n) { notices.push_back(n); };
        auto m = chat({"lab", "openai", f.url()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel);
        expect(m.content == "finally" && calls == 3 && notices.size() == 2 && notices[0].find("HTTP 503") != std::string::npos,
               "503 is retried with a notice each time, then succeeds");
    }
    {
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 400;
            res.set_content("{\"error\":\"bad request\"}", "application/json");
        });
        f.start();
        ChatOptions opt{"test-model"};
        opt.retry_base_ms = 10;
        std::string msg;
        try { chat({"lab", "openai", f.url()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const ApiError& e) { msg = e.what(); }
        expect(calls == 1 && msg.find("400") != std::string::npos, "a 400 is not retried and comes back as ApiError");
    }
    {
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 500;
            res.set_content("{}", "application/json");
        });
        f.start();
        ChatOptions opt{"test-model"};
        opt.retries = 2;
        opt.retry_base_ms = 10;
        bool threw = false;
        try { chat({"lab", "openai", f.url()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const ApiError&) { threw = true; }
        expect(threw && calls == 3, "gives up after the configured retries (1 try + 2 retries)");
    }
    {
        // A failure after output started is not retried (it would duplicate what the user saw).
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.set_chunked_content_provider("text/event-stream", [](size_t, httplib::DataSink& sink) {
                std::string line = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
                sink.write(line.data(), line.size());
                return false;  // drop the connection
            });
        });
        f.start();
        ChatOptions opt{"test-model"};
        opt.retry_base_ms = 10;
        std::string streamed;
        try { chat({"lab", "openai", f.url()}, opt, hello, json::array(), [&](std::string_view d, bool) { streamed += d; }, no_cancel); } catch (const std::exception&) {}
        expect(calls == 1, "no retry once output has streamed");
    }
    {
        ChatOptions opt{"test-model"};
        opt.retries = 2;
        opt.retry_base_ms = 10;
        std::vector<std::string> notices;
        opt.notice = [&](const std::string& n) { notices.push_back(n); };
        std::string msg;
        try { chat({"lab", "openai", "http://127.0.0.1:1"}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const TransportError& e) { msg = e.what(); }
        expect(notices.size() == 2 && msg.find("can't reach") != std::string::npos, "connection failures are retried too, then reported as TransportError");
    }

    section("usage limits");
    {
        // Bodies as the providers send them. Anthropic's Fable cap is the exact message seen on 2026-10-01.
        auto limit = [](int status, const char* body, const char* type) { return is_usage_limit(ApiError(status, std::string("x returned HTTP ") + std::to_string(status) + ": " + body, 0, type)); };
        expect(limit(429, "You've reached your Fable limit. Run /usage-credits to continue or switch models with /model.", "rate_limit_error"), "Anthropic's Fable limit (rate_limit_error, 429) is a usage limit");
        expect(limit(429, "You've reached your Fable limit. Run /usage-credits to continue or switch models with /model.", "rate_limit"), "also with the type spelled rate_limit");
        expect(limit(429, "You have reached your specified workspace API usage limits. You will regain access on 2026-11-01 at 00:00 UTC.", "rate_limit_error"), "Anthropic's workspace usage limit is one");
        expect(limit(400, "Your credit balance is too low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits.", "invalid_request_error"), "Anthropic's credit balance is one");
        expect(limit(429, "You exceeded your current quota, please check your plan and billing details.", "insufficient_quota"), "OpenAI's insufficient_quota is one");
        expect(limit(402, "Insufficient credits. Add more using https://openrouter.ai/settings/credits", ""), "OpenRouter's 402 is one");
        expect(limit(402, "Insufficient Balance", ""), "DeepSeek's 402 is one");
        expect(!limit(429, "This request would exceed the rate limit for your organization of 30,000 input tokens per minute. Please reduce the prompt length or try again later.", "rate_limit_error"),
               "a per-minute rate limit is not");
        expect(!limit(429, "slow down", ""), "nor a plain 429 slow down");
        expect(!limit(529, "Overloaded", "overloaded_error") && !limit(500, "quota service unavailable", ""), "nor an overload or a server error");
    }
    {
        // Through a provider: the type reaches ApiError, and a usage limit is not retried.
        Fake f;
        int calls = 0;
        f.srv.Post("/v1/messages", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 429;
            res.set_content(R"({"type":"error","error":{"type":"rate_limit_error","message":"You've reached your Fable limit. Run /usage-credits to continue or switch models with /model."}})", "application/json");
        });
        f.start();
        Provider p = anth;
        p.base_url = f.url();
        ChatOptions opt{"claude-fable-5-1"};
        opt.retry_base_ms = 10;
        std::string type;
        bool usage = false;
        try { chat(p, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const ApiError& e) { type = e.type, usage = is_usage_limit(e); }
        expect(calls == 1 && type == "rate_limit_error" && usage, "the Fable limit comes back at once as a usage limit with Anthropic's error type (" + std::to_string(calls) + " calls)");
    }
    {
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 429;
            res.set_content(R"({"error":{"message":"You exceeded your current quota, please check your plan and billing details.","type":"insufficient_quota","param":null,"code":"insufficient_quota"}})", "application/json");
        });
        f.start();
        ChatOptions opt{"test-model"};
        opt.retry_base_ms = 10;
        std::string type;
        try { chat({"lab", "openai", f.url()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const ApiError& e) { type = e.type; }
        expect(calls == 1 && type == "insufficient_quota", "OpenAI's insufficient_quota likewise, typed");
    }
    {
        Fake f;
        int calls = 0;
        f.srv.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 429;
            res.set_content(R"({"error":{"message":"slow down","type":"rate_limit_error"}})", "application/json");
        });
        f.start();
        ChatOptions opt{"test-model"};
        opt.retries = 1;
        opt.retry_base_ms = 10;
        try { chat({"lab", "openai", f.url()}, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel); } catch (const ApiError&) {}
        expect(calls == 2, "a plain 429 is still retried");
    }

    section("generation controls");
    {
        Fake f;
        f.serve("/v1/chat/completions", {"data: " + json{{"choices", {{{"delta", {{"content", "ok"}}}}}}}.dump() + "\n\ndata: [DONE]\n\n"});
        f.start();
        Provider p{"lab", "openai", f.url() + "/v1", "", "", json::object()};
        ChatOptions opt{"test-model"};
        opt.stop = {"STOP"};
        opt.logit_bias = {{"1234", -100}, {"▲", -100}};
        opt.sampling = {{"temperature", 0.2}, {"xtc_probability", 0.5}};
        chat(p, opt, hello, json::array(), [](std::string_view, bool) {}, no_cancel);
        auto body = json::parse(f.last_body);
        expect(body["stop"][0] == "STOP" && body["logit_bias"]["1234"] == -100 && body["logit_bias"]["▲"] == -100 && body["temperature"] == 0.2 && body["xtc_probability"] == 0.5,
               "OpenAI-compatible requests carry stop, logit_bias and sampler keys at the top level");
    }

    section("images reach the OpenAI-compatible shape");
    {
        Fake f;
        f.serve("/v1/chat/completions", {"data: {\"choices\":[{\"delta\":{\"content\":\"a cat\"}}]}\n\n", "data: [DONE]\n\n"});
        f.start();
        Message u{"user", "what is in the picture"};
        u.images.push_back({"image/png", "QUJD", "p.png"});
        ChatOptions o;
        o.model = "x";
        o.retries = 0;
        chat(Provider{"lab", "openai", f.url() + "/v1"}, o, {u}, json::array(), [](std::string_view, bool) {}, no_cancel);
        auto content = json::parse(f.last_body)["messages"][0]["content"];
        expect(content.is_array() && content.size() == 2 && content[0]["type"] == "text" && content[1]["type"] == "image_url" && content[1]["image_url"]["url"] == "data:image/png;base64,QUJD",
               "an OpenAI-compatible request carries the picture as an image_url part beside the text");
    }

    section("bare model names and listing");
    {
        auto ps = default_providers();
        expect(ps.front().name == "llamacpp", "llamacpp is the first provider");
        auto [p, name] = resolve_model(ps, "Qwen3.5-9B-Q4_K_M");
        expect(p.name == "llamacpp" && name == "Qwen3.5-9B-Q4_K_M", "a bare name goes to llamacpp");
        // A user may still run an OpenAI-compatible server under the old name; its tags look like bare names, so
        // a bare name never lands there by accident.
        std::vector<Provider> only_ollama = {{"ollama", "openai", "http://127.0.0.1:11434/v1"}, {"MyOllama", "openai", "http://127.0.0.1:1"}};
        auto [po, no] = resolve_model(only_ollama, "ollama/qwen3.5:9b");
        expect(po.name == "ollama" && no == "qwen3.5:9b", "a provider called ollama is reached by prefix");
        bool threw = false;
        try { resolve_model(only_ollama, "x"); } catch (const std::exception& e) { threw = std::string(e.what()).find("ollama/x") != std::string::npos; }
        expect(threw, "with only [Oo]llama providers a bare name is refused with the fix spelled out");
        Fake f;
        f.srv.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) { res.set_content(R"({"data":[{"id":"Qwen3.5-9B-Q4_K_M"},{"id":"Qwen3.5-4B-Q4_K_M"}]})", "application/json"); });
        f.start();
        auto ids = list_openai_models({"lab", "openai", f.url() + "/v1"});
        expect(ids == std::vector<std::string>{"Qwen3.5-4B-Q4_K_M", "Qwen3.5-9B-Q4_K_M"}, "an OpenAI-compatible server's models are listed by id, sorted");
        expect(server_answers({"lab", "openai", f.url() + "/v1"}), "a server that is up answers (any reply to GET /health counts)");
        expect(!server_answers({"lab", "openai", "http://127.0.0.1:9/v1"}), "a closed port does not");
    }

    section("cancel");
    {
        Fake f;
        f.srv.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::seconds(5));  // like a model still loading
            res.set_content("data: [DONE]\n\n", "text/event-stream");
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
            run({"lab", "openai", f.url()}, hello, nullptr, cancel);
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
        f.serve("/chat/completions", {"data: " + json{{"choices", {{{"delta", {{"content", "ok"}}}}}}}.dump() + "\n\ndata: [DONE]\n\n"});
        f.start();
        std::vector<Message> msgs = {{"user", "hi"}, {"tool", std::string("binary \xff\xfe\x80 output"), {}, "read_file"}};
        bool ok = false;
        try { ok = run({"lab", "openai", f.url()}, msgs).content == "ok"; } catch (const std::exception&) {}
        expect(ok && json::parse(f.last_body, nullptr, false).is_object(), "invalid UTF-8 in history is sent as valid JSON");
    }

    section("claude-cli: Claude Code as a text-only provider (a fake claude on PATH)");
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / ("maic-llm-test-cli-" + std::to_string(getpid()));
        fs::remove_all(dir);
        std::string saved_path = std::getenv("PATH") ? std::getenv("PATH") : "";
        setenv("XDG_STATE_HOME", (dir / "state").c_str(), 1);  // the CLI's directory and log, never ~/.local/state/maic
        setenv("ANTHROPIC_API_KEY", "sk-not-for-the-cli", 1);
        fake_claude::install(dir);
        Provider cli = by_name("claude-cli");
        expect(cli.kind == "cli" && cli.options.value("command", "") == "claude" && cli.remote(), "claude-cli is shipped: kind cli, command claude, remote");
        auto ask = [&](const Provider& p, const std::string& model, const std::string& sys, const std::string& text, std::string* streamed = nullptr, int* pieces = nullptr,
                       const std::atomic<bool>& cancel = no_cancel) {
            return chat(p, {model}, {{"system", sys}, {"user", text}}, json::array(), [&](std::string_view d, bool) {
                if (streamed) *streamed += d;
                if (pieces) ++*pieces;
            }, cancel);
        };
        auto pid_of = [](const Message& m) { return m.content.rfind("pid ", 0) == 0 ? std::atoi(m.content.c_str() + 4) : 0; };

        std::string streamed;
        int pieces = 0;
        Message a = ask(cli, "haiku", "be brief", "hello there", &streamed, &pieces);
        int pid = pid_of(a);
        expect(pid > 0 && a.content.find("call 1: hello there") != std::string::npos && streamed == a.content && pieces > 1,
               "the reply streams through the callback in pieces and comes back whole: " + a.content);
        expect(a.usage.input == 10 && a.usage.output == 5 && a.usage.context == 200000, "usage from the result line: input with cache reads, output, the window");
        auto starts = fake_claude::spawns(dir);
        json argv = starts.empty() ? json::array() : starts[0]["argv"];
        auto after = [&](const std::string& flag) {
            for (size_t i = 0; i + 1 < argv.size(); ++i) if (argv[i] == flag) return argv[i + 1].get<std::string>();
            return std::string("(absent)");
        };
        auto has = [&](const std::string& flag) { return std::find(argv.begin(), argv.end(), flag) != argv.end(); };
        expect(starts.size() == 1 && after("--tools").empty() && has("--strict-mcp-config") && after("--mcp-config") == R"({"mcpServers":{}})" && after("--permission-mode") == "dontAsk",
               "its own tools are off: --tools \"\", strict and empty MCP, the dontAsk permission mode: " + argv.dump());
        expect(has("--setting-sources") && after("--setting-sources").empty(), "no settings files by default: --setting-sources \"\"");
        expect(has("-p") && after("--input-format") == "stream-json" && after("--output-format") == "stream-json" && has("--verbose") && has("--include-partial-messages") &&
                   has("--no-session-persistence") && after("--system-prompt") == "be brief" && after("--model") == "haiku",
               "headless, stream-json both ways, no session persistence, the system prompt and --model from the model name");
        expect(!starts.empty() && !starts[0].value("api_key", true) && starts[0].value("cwd", "") == fs::weakly_canonical(dir / "state" / "maic" / "cli" / "claude-cli").string(),
               "the user's plan, not the API: ANTHROPIC_API_KEY is kept from it, and it runs in a directory of its own");

        Message b = ask(cli, "haiku", "be brief", "again");
        expect(pid_of(b) == pid && b.content.find("call 2: again") != std::string::npos && fake_claude::spawns(dir).size() == 1, "the next request reuses the process: " + b.content);
        Message other = ask(cli, "haiku", "another purpose", "x");
        expect(pid_of(other) != pid && pid_of(other) > 0 && fake_claude::spawns(dir).size() == 2, "another system prompt (another purpose) has a process of its own");

        Provider short_lived = cli;
        short_lived.options["max_requests"] = 2;
        int first = pid_of(ask(short_lived, "haiku", "recycled", "1"));
        int second = pid_of(ask(short_lived, "haiku", "recycled", "2"));
        Message third = ask(short_lived, "haiku", "recycled", "3");
        expect(first == second && pid_of(third) != first && third.content.find("call 1: 3") != std::string::npos, "after max_requests a new process takes over: " + third.content);

        kill(pid, SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Message c = ask(cli, "haiku", "be brief", "after the kill");
        expect(pid_of(c) != pid && pid_of(c) > 0 && c.content.find("call 1: after the kill") != std::string::npos, "a dead process is reaped and a new one started: " + c.content);
        ask(cli, "haiku", "be brief", "EXIT now");
        Message d = ask(cli, "haiku", "be brief", "after it exited");
        expect(pid_of(d) != pid_of(c) && d.content.find("call 1: after it exited") != std::string::npos, "one that exits between requests is replaced too: " + d.content);

        bool usage = false;
        int status = 0;
        std::string what;
        try { ask(cli, "haiku", "be brief", "LIMIT please"); } catch (const ApiError& e) { usage = is_usage_limit(e), status = e.status, what = e.what(); }
        expect(usage && status == 429 && what == "claude-cli/haiku: You've reached your Fable limit. Run /usage-credits to continue or switch models with /model.",
               "a usage limit in the result line is an ApiError that is_usage_limit knows: " + what);
        expect(ask(cli, "haiku", "be brief", "still there").content.find("still there") != std::string::npos, "and the process stays usable after it");

        size_t before = fake_claude::spawns(dir).size();
        std::string refused;
        try { chat(cli, {"sonnet"}, hello, kTools, [](std::string_view, bool) {}, no_cancel); } catch (const std::exception& e) { refused = e.what(); }
        expect(refused == "claude-cli is a text-only provider here; use it for small_model, the reviewer, titles or compaction; agent use arrives with level 2" &&
                   fake_claude::spawns(dir).size() == before,
               "tool schemas are refused before anything starts: " + refused);

        expect(generate_title(cli, "haiku", "How big should the ears be?") == "Title from haiku", "titles work through it");

        Provider slow = cli;
        slow.options["timeout"] = 1;
        std::string timed_out;
        auto t0 = std::chrono::steady_clock::now();
        try { ask(slow, "haiku", "slow", "HANG"); } catch (const std::exception& e) { timed_out = e.what(); }
        expect(timed_out.find("claude-cli/haiku: no reply in 1 s") == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4), "a request has a timeout: " + timed_out);
        expect(ask(slow, "haiku", "slow", "back").content.find("call 1: back") != std::string::npos, "and the next request starts a new process");
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel = true;
        });
        bool cancelled = false;
        try { ask(cli, "haiku", "be brief", "HANG", nullptr, nullptr, cancel); } catch (const Cancelled&) { cancelled = true; }
        canceller.join();
        expect(cancelled, "cancel stops a request in flight");

        Provider opted = cli;
        opted.options["setting_sources"] = "user,project";
        Message with_settings = ask(opted, "haiku", "with settings", "x");
        json last_argv = fake_claude::spawns(dir).back()["argv"];
        bool user_project = false;
        for (size_t i = 0; i + 1 < last_argv.size(); ++i) user_project = user_project || (last_argv[i] == "--setting-sources" && last_argv[i + 1] == "user,project");
        expect(pid_of(with_settings) > 0 && user_project, "setting_sources opts back into settings files: " + last_argv.dump());
        opted.options["setting_sources"] = "user,global";
        std::string bad_sources;
        try { ask(opted, "haiku", "bad settings", "x"); } catch (const std::exception& e) { bad_sources = e.what(); }
        expect(bad_sources == "providers.claude-cli.options.setting_sources: 'global' is not user, project or local", "an unknown setting source is an error: " + bad_sources);

        Provider widened = cli;
        widened.options["args"] = json::array({"--tools", "default"});
        std::string args_err;
        try { ask(widened, "haiku", "be brief", "x"); } catch (const std::exception& e) { args_err = e.what(); }
        expect(args_err.find("may not contain --tools") != std::string::npos, "options.args cannot turn its tools back on: " + args_err);
        widened.options["args"] = json::array({"--setting-sources", "user"});
        args_err.clear();
        try { ask(widened, "haiku", "be brief", "x"); } catch (const std::exception& e) { args_err = e.what(); }
        expect(args_err.find("may not contain --setting-sources") != std::string::npos, "nor set --setting-sources (setting_sources does): " + args_err);

        fs::create_directories(dir / "empty");
        setenv("PATH", (dir / "empty").c_str(), 1);
        std::string missing;
        try { ask(cli, "haiku", "a new purpose", "x"); } catch (const std::exception& e) { missing = e.what(); }
        expect(missing.find("claude-cli/haiku (the claude-haiku-cli preset): `claude` is not on PATH") == 0 && missing.find("the API preset haiku-4.5") != std::string::npos,
               "no claude on PATH: the error names the preset and the API alternative: " + missing);
        setenv("PATH", saved_path.c_str(), 1);
        unsetenv("ANTHROPIC_API_KEY");
    }

    return finish();
}
