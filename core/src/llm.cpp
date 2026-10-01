#include "llm_http.hpp"

#include "maic/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace maic {

namespace fs = std::filesystem;

nlohmann::json message_to_json(const Message& m) {
    nlohmann::json j = {{"role", m.role}, {"content", m.content}};
    if (!m.tool_calls.empty()) {
        j["tool_calls"] = nlohmann::json::array();
        for (const auto& c : m.tool_calls) j["tool_calls"].push_back({{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
    }
    if (!m.tool_name.empty()) j["tool_name"] = m.tool_name;
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    if (m.is_error) j["is_error"] = true;
    if (!m.raw_kind.empty()) {
        j["raw_kind"] = m.raw_kind;
        j["raw"] = m.raw;
    }
    if (!m.images.empty()) {
        j["images"] = nlohmann::json::array();
        for (const auto& im : m.images) j["images"].push_back({{"mime", im.mime}, {"name", im.name}, {"data", im.base64}});
    }
    return j;
}

Message message_from_json(const nlohmann::json& j) {
    Message m;
    m.role = j.value("role", "user");
    m.content = j.value("content", "");
    for (const auto& c : j.value("tool_calls", nlohmann::json::array())) {
        auto args = c.value("arguments", nlohmann::json::object());
        m.tool_calls.push_back({c.value("id", ""), c.value("name", ""), args.is_object() ? args : nlohmann::json::object()});
    }
    m.tool_name = j.value("tool_name", "");
    m.tool_call_id = j.value("tool_call_id", "");
    m.is_error = j.value("is_error", false);
    m.raw_kind = j.value("raw_kind", "");
    if (j.contains("raw")) m.raw = j["raw"];
    for (const auto& im : j.value("images", nlohmann::json::array())) {
        if (im.is_object()) m.images.push_back({im.value("mime", "image/png"), im.value("data", ""), im.value("name", "")});
    }
    return m;
}

bool Provider::remote() const {
    for (const char* local : {"://127.", "://localhost", "://[::1]"}) {
        if (base_url.find(local) != std::string::npos) return false;
    }
    return true;
}

std::string Provider::api_key() const {
    if (!api_key_env.empty()) {
        if (const char* v = std::getenv(api_key_env.c_str()); v && *v) return v;
    }
    if (!api_key_command.empty()) {
        FILE* p = popen(api_key_command.c_str(), "r");
        std::string out;
        if (p) {
            char buf[4096];
            while (fgets(buf, sizeof(buf), p)) out += buf;
            pclose(p);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        if (!out.empty()) return out;
        throw std::runtime_error(name + ": api_key_command printed nothing: " + api_key_command);
    }
    throw std::runtime_error(name + ": no API key. Set " + (api_key_env.empty() ? std::string("api_key_env") : api_key_env) +
                             " in your environment, or api_key_command in settings.");
}

std::vector<Provider> default_providers() {
    return {
        // llama.cpp's OpenAI-compatible endpoint (services/llamacpp.json). Everything in `sampling` is merged into
        // the request, so logit_bias, xtc_probability, xtc_threshold, dry_multiplier, grammar and json_schema all reach it.
        // llama.cpp's OpenAI-compatible server: thinking is switched per request, the context matches
        // services/llamacpp.json, and later system messages go as user notes (the default for this kind).
        {"llamacpp", "openai", "http://127.0.0.1:8081/v1", "", "", {{"thinking_controls", true}, {"context_window", 16384}}},
        // Kept as an option; never chosen for a bare model name (see resolve_model).
        {"ollama", "ollama", "http://127.0.0.1:11434", "", "", nlohmann::json::object()},
        {"anthropic", "anthropic", "https://api.anthropic.com", "ANTHROPIC_API_KEY", "",
         {{"max_tokens", 64000}, {"effort", "high"}, {"think_effort", "xhigh"}, {"fallbacks", "default"}}},
        {"deepseek", "openai", "https://api.deepseek.com", "DEEPSEEK_API_KEY", "", nlohmann::json::object()},
        {"openrouter", "openai", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "", nlohmann::json::object()},
    };
}

std::pair<Provider, std::string> resolve_model(const std::vector<Provider>& providers, const std::string& model) {
    if (providers.empty()) throw std::runtime_error("no model providers configured");
    if (size_t slash = model.find('/'); slash != std::string::npos) {
        std::string prefix = model.substr(0, slash);
        for (const auto& p : providers) {
            if (p.name == prefix) return {p, model.substr(slash + 1)};
        }
    }
    // A bare name never lands on Ollama by accident: it goes to the first provider whose name does not
    // match [Oo]llama. Ollama is used only when the model is written as ollama/NAME.
    for (const auto& p : providers) {
        std::string lower = p.name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("ollama") == std::string::npos) return {p, model};
    }
    throw std::runtime_error("model '" + model + "' names no provider, and only Ollama providers are configured; write it as ollama/" + model + " to use Ollama on purpose");
}

std::vector<std::string> list_openai_models(const Provider& provider) {
    size_t root = provider.base_url.find("://");
    size_t slash = root == std::string::npos ? std::string::npos : provider.base_url.find('/', root + 3);
    std::string host = slash == std::string::npos ? provider.base_url : provider.base_url.substr(0, slash);
    std::string prefix = slash == std::string::npos ? "" : provider.base_url.substr(slash);
    httplib::Client client(host);
    client.set_connection_timeout(5);
    httplib::Headers headers;
    if (!provider.api_key_env.empty() || !provider.api_key_command.empty()) headers.emplace("Authorization", "Bearer " + provider.api_key());
    auto res = client.Get(prefix + "/models", headers);
    if (!res || res->status != 200) throw std::runtime_error("can't list models on " + provider.base_url + " (is it running?)");
    std::vector<std::string> out;
    for (const auto& m : nlohmann::json::parse(res->body, nullptr, false).value("data", nlohmann::json::array())) {
        if (m.contains("id") && m["id"].is_string()) out.push_back(m["id"].get<std::string>());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> list_ollama_models(const Provider& provider) {
    httplib::Client client(provider.base_url);
    client.set_connection_timeout(5);
    auto res = client.Get("/api/tags");
    if (!res || res->status != 200) throw std::runtime_error("can't list models on " + provider.base_url + " (is it running? `maic up ollama`)");
    std::vector<std::string> out;
    for (const auto& m : nlohmann::json::parse(res->body, nullptr, false).value("models", nlohmann::json::array())) {
        if (m.contains("name") && m["name"].is_string()) out.push_back(m["name"].get<std::string>());
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

Message chat_once(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                  const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    if (provider.kind == "ollama") return detail::chat_ollama(provider, options, messages, tools, on_text, cancel);
    if (provider.kind == "anthropic") return detail::chat_anthropic(provider, options, messages, tools, on_text, cancel);
    if (provider.kind == "openai") return detail::chat_openai(provider, options, messages, tools, on_text, cancel);
    throw std::runtime_error(provider.name + ": unknown provider kind '" + provider.kind + "' (ollama, anthropic, openai)");
}

bool sleep_unless_cancelled(int ms, const std::atomic<bool>& cancel) {
    for (int waited = 0; waited < ms; waited += 50) {
        if (cancel.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - waited)));
    }
    return !cancel.load();
}

}  // namespace

Message chat(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
             const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    std::mt19937 rng{std::random_device{}()};
    for (int attempt = 0;; ++attempt) {
        bool streamed = false;
        auto sink = [&](std::string_view d, bool t) {
            streamed = true;
            on_text(d, t);
        };
        int wait_ms = 0;
        std::string why;
        try {
            return chat_once(provider, options, messages, tools, sink, cancel);
        } catch (const ApiError& e) {
            bool retryable = e.status == 429 || e.status == 408 || e.status == 409 || e.status >= 500;
            if (!retryable || streamed || attempt >= options.retries) throw;
            wait_ms = e.retry_after_ms;
            why = "HTTP " + std::to_string(e.status);
        } catch (const TransportError& e) {
            if (streamed || attempt >= options.retries) throw;
            why = e.what();
        }
        if (wait_ms <= 0) {
            int base = options.retry_base_ms << attempt;  // 2 s, 4 s, 8 s
            std::uniform_int_distribution<int> jitter(-base / 4, base / 4);
            wait_ms = std::min(30000, base + jitter(rng));
        }
        if (options.notice) {
            options.notice(provider.name + ": " + why + "; retrying in " + std::to_string((wait_ms + 500) / 1000) + " s (" + std::to_string(attempt + 1) +
                           "/" + std::to_string(options.retries) + ")");
        }
        if (!sleep_unless_cancelled(wait_ms, cancel)) throw Cancelled();
    }
}

namespace detail {

std::string dump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string api_error(const std::string& provider, const HttpResult& r) {
    auto j = nlohmann::json::parse(r.error_body, nullptr, false);
    std::string msg;
    if (j.is_object()) {
        const auto& e = j.contains("error") ? j["error"] : j;
        if (e.is_string()) msg = e.get<std::string>();
        else if (e.is_object() && e.contains("message") && e["message"].is_string()) msg = e["message"].get<std::string>();
    }
    if (msg.empty()) msg = r.error_body.substr(0, 300);
    return provider + " returned HTTP " + std::to_string(r.status) + (msg.empty() ? "" : ": " + msg);
}

void throw_api_error(const std::string& provider, const HttpResult& r) {
    throw ApiError(r.status, api_error(provider, r), r.retry_after_ms);
}

HttpResult stream_post(const std::string& base_url, const std::string& path,
                       const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                       const std::function<void(std::string_view)>& on_data, const std::atomic<bool>& cancel) {
    // base_url may carry a path prefix (https://openrouter.ai/api/v1); httplib wants scheme://host:port.
    size_t scheme_end = base_url.find("://");
    size_t path_start = base_url.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
    std::string origin = path_start == std::string::npos ? base_url : base_url.substr(0, path_start);
    std::string prefix = path_start == std::string::npos ? "" : base_url.substr(path_start);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    httplib::Client client(origin);
    if (!client.is_valid()) throw std::runtime_error("bad base_url (https needs OpenSSL support): " + base_url);
    client.set_connection_timeout(10);
    client.set_read_timeout(600);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // vcpkg's OpenSSL doesn't know the system's certificate location.
    if (const char* f = std::getenv("SSL_CERT_FILE"); f && *f) {
        client.set_ca_cert_path(f);
    } else {
        for (const char* bundle : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
            if (fs::exists(bundle)) {
                client.set_ca_cert_path(bundle);
                break;
            }
        }
    }
    client.enable_server_certificate_verification(true);
#endif

    httplib::Request req;
    req.method = "POST";
    req.path = prefix + path;
    req.set_header("Content-Type", "application/json");
    for (const auto& [k, v] : headers) req.set_header(k, v);
    req.body = body;

    HttpResult result;
    httplib::Response res;
    req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
        if (res.status != 0 && res.status != 200) {
            if (result.error_body.size() < 4096) result.error_body.append(data, std::min<size_t>(len, 4096));
        } else {
            on_data(std::string_view(data, len));
        }
        return !cancel.load();
    };
    // The receiver only sees cancel when data arrives. While a model loads or reads a long prompt nothing does,
    // so a watcher closes the socket instead.
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
    httplib::Error err = httplib::Error::Success;
    bool ok = client.send(req, res, err);
    finished = true;
    watcher.join();
    if (cancel.load()) throw Cancelled();
    if (!ok) {
        throw TransportError("can't reach " + base_url + " (" + httplib::to_string(err) + ")");
    }
    result.status = res.status;
    if (result.status != 200 && result.error_body.empty()) result.error_body = res.body.substr(0, 4096);
    if (res.has_header("Retry-After")) {
        std::string ra = res.get_header_value("Retry-After");
        if (!ra.empty() && std::isdigit(static_cast<unsigned char>(ra[0]))) result.retry_after_ms = std::atoi(ra.c_str()) * 1000;
    }
    return result;
}

}  // namespace detail

}  // namespace maic
