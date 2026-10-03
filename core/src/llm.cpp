#include "llm_http.hpp"

#include "maic/http.hpp"
#include "maic/paths.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
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

bool loopback_host(std::string host) {
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host == "localhost" || host == "::1") return true;
    int octets = 0, value = -1;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            if (value < 0 || value > 255 || (octets == 0 && value != 127)) return false;
            ++octets;
            value = -1;
        } else if (std::isdigit(static_cast<unsigned char>(host[i])) && (value != 0)) {  // no leading zeros: 0177 is octal to some resolvers
            value = (value < 0 ? 0 : value * 10) + (host[i] - '0');
            if (value > 255) return false;
        } else {
            return false;
        }
    }
    return octets == 4;
}

bool local_url(const std::string& url) {
    if (url.rfind("unix:", 0) == 0) return true;
    size_t sep = url.find("://");
    if (sep == std::string::npos) return false;
    std::string scheme = url.substr(0, sep);
    for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https") return false;
    std::string authority = url.substr(sep + 3, url.find_first_of("/?#", sep + 3) - (sep + 3));
    for (unsigned char c : authority) {
        if (c <= ' ' || c == '\\' || c >= 0x7f) return false;  // nothing a parser could read two ways
    }
    std::string host = authority.substr(authority.rfind('@') == std::string::npos ? 0 : authority.rfind('@') + 1);
    if (!host.empty() && host.front() == '[') {
        size_t close = host.find(']');
        if (close == std::string::npos || (close + 1 < host.size() && host[close + 1] != ':')) return false;
        host = host.substr(0, close + 1);
    } else if (size_t colon = host.find(':'); colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    return loopback_host(host);
}

bool Provider::remote() const {
    if (kind == "cli") return true;  // the CLI sends everything to its own service
    return !local_url(base_url);
}

bool Provider::metered() const {
    return options.value("metered", kind == "openai" && remote() && (!api_key_env.empty() || !api_key_command.empty()));
}

std::string Provider::api_key() const {
    std::string scheme = base_url.substr(0, base_url.find("://"));
    for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "https" && !local_url(base_url)) {
        throw std::runtime_error(name + ": base_url " + base_url + " is not https, and a key goes to a host off this machine only over https");
    }
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

namespace {
std::mutex key_envs_mu;
std::set<std::string, std::less<>> key_envs;
}  // namespace

void add_key_envs(const std::vector<Provider>& providers) {
    std::lock_guard lock(key_envs_mu);
    for (const auto& p : providers) {
        if (!p.api_key_env.empty()) key_envs.insert(p.api_key_env);
    }
}

bool is_key_env(std::string_view name) {
    if (name.size() > 8 && name.substr(name.size() - 8) == "_API_KEY") return true;
    std::lock_guard lock(key_envs_mu);
    return key_envs.count(name) > 0;
}

std::vector<Provider> default_providers() {
    return {
        // llama.cpp's OpenAI-compatible server (services/llamacpp.json): everything in `sampling` is merged into the
        // request, so logit_bias, xtc_probability, dry_multiplier, grammar and json_schema all reach it; thinking is
        // switched per request; the context matches the service; later system messages go as user notes.
        {"llamacpp", "openai", "http://127.0.0.1:8081/v1", "", "", {{"thinking_controls", true}, {"context_window", 16384}}, "llamacpp"},
        // The side server (services/llamacpp-2.json): the same router over the same GGUFs on port 8082, so a second
        // model can stay resident while the first does. No preset by default; `llamacpp-2/NAME` reaches it.
        {"llamacpp-2", "openai", "http://127.0.0.1:8082/v1", "", "", {{"thinking_controls", true}, {"context_window", 8192}}, "llamacpp"},
        // Metered (Micaiah, 2026-10-02): billed per token to the key's account, so no rule moves onto it from another provider.
        {"anthropic", "anthropic", "https://api.anthropic.com", "ANTHROPIC_API_KEY", "",
         {{"max_tokens", 64000}, {"effort", "high"}, {"think_effort", "xhigh"}, {"fallbacks", "default"}, {"metered", true}}},
        // DeepSeek's API (api-docs.deepseek.com, checked 2026-10-02; docs/references/deepseek.md): thinking is on unless
        // the request turns it off; while it is on, temperature and the penalties do nothing and top_p is raised to
        // 0.95, without it top_p is fixed at 1.0, so none of those is sent where it would mislead. Its thinking turns'
        // reasoning_content goes back with every later request that carries tools. Pictures only to deepseek-flash. An
        // empty `stop` reply is sent again. Metered: billed per token to the key's account. GET /models is read at first
        // use for each model's window, output cap and thinking levels. Its 429 counts open requests per account (2,500
        // Flash, 500 V4 Pro): MAIC keeps to a third of each (Micaiah, 2026-10-02).
        {"deepseek", "openai", "https://api.deepseek.com", "DEEPSEEK_API_KEY", "",
         {{"context_window", 1000000},
          {"read_models", true},
          {"max_concurrent", {{"deepseek-flash", 833}, {"deepseek-v4-pro", 166}}},
          {"max_tokens", 65536},
          {"think_on", {{"thinking", {{"type", "enabled"}}}}},
          {"think_off", {{"thinking", {{"type", "disabled"}}}}},
          {"think_sampling", {{"temperature", false}, {"presence_penalty", false}, {"frequency_penalty", false}, {"top_p", {0.95, 1.0}}}},
          {"nothink_sampling", {{"top_p", false}, {"presence_penalty", false}, {"frequency_penalty", false}}},
          {"retry_empty", true},
          {"replay_reasoning", true},
          {"vision", {"deepseek-flash"}}}},
        {"openrouter", "openai", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "", nlohmann::json::object()},
        // Claude Code run headless on the user's own login and plan: text only, or the agent on MAIC's tools (docs/settings.md).
        // Metered as the API is (Micaiah, 2026-10-02): it spends the plan's usage.
        {"claude-cli", "cli", "", "", "", {{"command", "claude"}, {"args", nlohmann::json::array()}, {"metered", true}}},
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
    // A bare name goes to the first provider whose name does not match [Oo]llama. A provider someone
    // configures under that name (its tags look like bare names) is used only when written as NAME/model.
    for (const auto& p : providers) {
        std::string lower = p.name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("ollama") == std::string::npos) return {p, model};
    }
    throw std::runtime_error("model '" + model + "' names no provider, and a bare name never goes to a provider called ollama; write it as " + providers.front().name + "/" + model + " to use that one on purpose");
}

namespace {

// "http://127.0.0.1:8081/v1" -> {"http://127.0.0.1:8081", "/v1"}
std::pair<std::string, std::string> split_base_url(const std::string& base_url) {
    size_t root = base_url.find("://");
    size_t slash = root == std::string::npos ? std::string::npos : base_url.find('/', root + 3);
    if (slash == std::string::npos) return {base_url, ""};
    return {base_url.substr(0, slash), base_url.substr(slash)};
}

// vcpkg's OpenSSL doesn't know the system's certificate location.
void trust_system_cas([[maybe_unused]] httplib::Client& client) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
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
}

}  // namespace

bool server_answers(const Provider& provider) {
    httplib::Client client(split_base_url(provider.base_url).first);
    client.set_connection_timeout(1);
    client.set_read_timeout(2);
    return static_cast<bool>(client.Get("/health"));
}

std::vector<std::string> list_openai_models(const Provider& provider) {
    auto [host, prefix] = split_base_url(provider.base_url);
    httplib::Client client(host);
    trust_system_cas(client);
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

namespace {

// <state>/api-models/<provider>.json: {base_url, read_at, data}, data as GET /models gave it. Only for a provider
// whose name can be a file name.
fs::path facts_path(const Provider& provider) {
    bool plain = !provider.name.empty() && provider.name[0] != '.' &&
                 std::all_of(provider.name.begin(), provider.name.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
    return plain ? state_dir() / "api-models" / (provider.name + ".json") : fs::path();
}

ApiModelFacts facts_from(const nlohmann::json& data) {
    ApiModelFacts f;
    if (!data.is_array()) return f;
    for (const auto& m : data) {
        if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) continue;
        ModelFacts mf;
        if (m.contains("context_window") && m["context_window"].is_number_integer()) mf.context = m["context_window"];
        if (m.contains("max_output_tokens") && m["max_output_tokens"].is_number_integer()) mf.output = m["max_output_tokens"];
        if (m.contains("effort") && m["effort"].is_object()) {
            const auto& e = m["effort"];
            for (const auto& level : e.value("supported_levels", nlohmann::json::array())) {
                if (level.is_string()) mf.efforts.push_back(level);
            }
            if (e.contains("default_level") && e["default_level"].is_string()) mf.default_effort = e["default_level"];
        }
        f.models[m["id"].get<std::string>()] = mf;
    }
    return f;
}

}  // namespace

ApiModelFacts saved_model_facts(const Provider& provider) {
    fs::path path = facts_path(provider);
    std::ifstream in(path);
    if (path.empty() || !in) return {};
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || j.value("base_url", "") != provider.base_url) return {};
    ApiModelFacts f = facts_from(j.value("data", nlohmann::json::array()));
    f.read_at = j.value("read_at", 0L);
    f.saved = true;
    return f;
}

ApiModelFacts api_model_facts(const Provider& provider, bool refresh) {
    // One lock for every provider: a first use waits for the read in flight rather than starting its own.
    static std::mutex mu;
    static std::map<std::string, ApiModelFacts> read;
    std::lock_guard lock(mu);
    std::string key = provider.name + " " + provider.base_url;
    if (auto it = read.find(key); it != read.end() && !refresh) return it->second;
    ApiModelFacts f;
    try {
        auto [host, prefix] = split_base_url(provider.base_url);
        httplib::Client client(host);
        trust_system_cas(client);
        client.set_connection_timeout(2);
        client.set_read_timeout(3);
        httplib::Headers headers;
        if (!provider.api_key_env.empty() || !provider.api_key_command.empty()) headers.emplace("Authorization", "Bearer " + provider.api_key());
        auto res = client.Get(prefix + "/models", headers);
        auto body = res && res->status == 200 ? nlohmann::json::parse(res->body, nullptr, false) : nlohmann::json();
        if (!res) f.error = "can't reach " + provider.base_url + " (" + httplib::to_string(res.error()) + ")";
        else if (res->status != 200) f.error = "GET /models returned HTTP " + std::to_string(res->status);
        else if (!body.is_object() || !body.contains("data") || !body["data"].is_array()) f.error = "GET /models sent no model list";
        else {
            f = facts_from(body["data"]);
            f.read_at = static_cast<long>(std::time(nullptr));
            if (fs::path path = facts_path(provider); !path.empty()) {
                std::error_code ec;
                fs::create_directories(path.parent_path(), ec);
                fs::path tmp = path;
                tmp += ".tmp";
                std::ofstream(tmp, std::ios::trunc) << nlohmann::json{{"base_url", provider.base_url}, {"read_at", f.read_at}, {"data", body["data"]}}.dump(1) << "\n";
                fs::rename(tmp, path, ec);
            }
        }
    } catch (const std::exception& e) {
        f.error = e.what();
    }
    if (!f.error.empty()) {
        if (ApiModelFacts saved = saved_model_facts(provider); !saved.models.empty()) {
            saved.error = f.error;
            f = saved;
        }
    }
    read[key] = f;
    return f;
}

namespace {

Message chat_once(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
                  const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    if (provider.kind == "anthropic") return detail::chat_anthropic(provider, options, messages, tools, on_text, cancel);
    if (provider.kind == "openai") return detail::chat_openai(provider, options, messages, tools, on_text, cancel);
    if (provider.kind == "cli") return detail::chat_cli(provider, options, messages, tools, on_text, cancel);
    throw std::runtime_error(provider.name + ": unknown provider kind '" + provider.kind + "' (anthropic, openai, cli)");
}

bool sleep_unless_cancelled(int ms, const std::atomic<bool>& cancel) {
    for (int waited = 0; waited < ms; waited += 50) {
        if (cancel.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - waited)));
    }
    return !cancel.load();
}

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// What every request to one provider account shares: the requests open per model against `max_concurrent`, the hold
// a 429 puts on all of them, and the 429s the breaker counts.
struct Account {
    std::mutex mu;
    std::condition_variable freed;
    std::map<std::string, int> open;
    std::set<std::string> past_half;  // models told they passed half their cap
    Clock::time_point hold_until{};
    bool breaker = false;  // open until hold_until
    std::deque<Clock::time_point> limited;
};

constexpr size_t kBreakerAfter = 5;

Account& account_of(const Provider& provider) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Account>> all;
    std::lock_guard lock(mu);
    auto& a = all[provider.name + " " + provider.base_url];
    if (!a) a = std::make_unique<Account>();
    return *a;
}

// `max_concurrent`: a number for every model of the provider, or one per model; 0 (or none) for no cap.
int concurrency_cap(const Provider& provider, const std::string& model) {
    const nlohmann::json c = provider.options.value("max_concurrent", nlohmann::json());
    if (c.is_number_integer()) return c.get<int>();
    if (c.is_object() && c.contains(model) && c[model].is_number_integer()) return c[model].get<int>();
    return 0;
}

std::string in_seconds(Clock::duration d) {
    return std::to_string((std::chrono::duration_cast<milliseconds>(d).count() + 999) / 1000) + " s";
}

// Before each attempt: waits out the account's hold (an open breaker fails at once, nothing sent), then for a slot
// under the cap. Notices are told outside the lock.
void enter(Account& a, const Provider& provider, const ChatOptions& options, int cap, const std::atomic<bool>& cancel) {
    std::unique_lock lock(a.mu);
    bool told_hold = false, told_queue = false;
    auto tell = [&](const std::string& text) {
        if (!options.notice) return;
        lock.unlock();
        options.notice(text);
        lock.lock();
    };
    for (;;) {
        if (cancel.load()) throw Cancelled();
        auto now = Clock::now();
        if (a.hold_until > now) {
            if (a.breaker) {
                throw ApiError(429, provider.name + ": MAIC is sending nothing to it for " + in_seconds(a.hold_until - now) + " more, after " + std::to_string(kBreakerAfter) +
                                        " rate limits in a short time (the circuit breaker); this request was not sent",
                               0, "maic_breaker");
            }
            if (!told_hold && a.hold_until - now >= std::chrono::seconds(1)) {
                told_hold = true;
                tell(provider.name + ": waiting " + in_seconds(a.hold_until - now) + " after a rate limit, with every other request to it");
                continue;
            }
            a.freed.wait_until(lock, std::min(a.hold_until, now + milliseconds(50)));
            continue;
        }
        a.breaker = false;
        if (cap <= 0) return;
        int& n = a.open[options.model];
        if (n >= cap) {
            if (!told_queue) {
                told_queue = true;
                tell(provider.name + "/" + options.model + ": all " + std::to_string(cap) + " concurrent requests MAIC allows are open (max_concurrent); this one waits for a slot");
                continue;
            }
            a.freed.wait_for(lock, milliseconds(50));
            continue;
        }
        ++n;
        if (2 * n > cap && a.past_half.insert(options.model).second) {
            tell(provider.name + "/" + options.model + ": " + std::to_string(n) + " of the " + std::to_string(cap) +
                 " concurrent requests MAIC allows are open (max_concurrent); past that, requests wait");
        }
        return;
    }
}

void leave(Account& a, const std::string& model, int cap) {
    if (cap <= 0) return;
    std::lock_guard lock(a.mu);
    int n = --a.open[model];
    if (4 * n <= cap) a.past_half.erase(model);  // told again only after falling to a quarter, so a count that hovers at half is told once
    a.freed.notify_all();
}

// A rate limit: every request to the account holds for `wait_ms`, and the fifth within `window_ms` opens the
// breaker for the window instead. True when this one opened it.
bool rate_limited(Account& a, int wait_ms, int window_ms) {
    std::lock_guard lock(a.mu);
    auto now = Clock::now();
    a.limited.push_back(now);
    while (a.limited.front() < now - milliseconds(window_ms)) a.limited.pop_front();
    if (a.limited.size() >= kBreakerAfter) {
        a.limited.clear();
        a.breaker = true;
        a.hold_until = now + milliseconds(window_ms);
        return true;
    }
    a.hold_until = std::max(a.hold_until, now + milliseconds(wait_ms));
    return false;
}

}  // namespace

int retry_wait_ms(int attempt, int base_ms) {
    thread_local std::mt19937 rng{std::random_device{}()};
    long long ceiling = std::min<long long>(60LL * base_ms, static_cast<long long>(base_ms) << std::min(attempt, 30));
    return std::uniform_int_distribution<int>(0, static_cast<int>(ceiling))(rng);
}

Message chat(const Provider& provider, const ChatOptions& options, const std::vector<Message>& messages,
             const nlohmann::json& tools, const TextSink& on_text, const std::atomic<bool>& cancel) {
    Account& account = account_of(provider);
    const int cap = concurrency_cap(provider, options.model);
    const int window_ms = 60 * options.retry_base_ms;
    for (int attempt = 0;; ++attempt) {
        bool streamed = false;
        auto sink = [&](std::string_view d, bool t) {
            streamed = true;
            on_text(d, t);
        };
        int wait_ms = retry_wait_ms(attempt, options.retry_base_ms);
        std::string why;
        enter(account, provider, options, cap, cancel);
        try {
            Message reply = chat_once(provider, options, messages, tools, sink, cancel);
            leave(account, options.model, cap);
            return reply;
        } catch (const ApiError& e) {
            leave(account, options.model, cap);
            bool limit = e.status == 429 && !is_usage_limit(e);
            wait_ms = std::max(wait_ms, e.retry_after_ms);
            if (limit && rate_limited(account, wait_ms, window_ms)) {
                throw ApiError(429, std::string(e.what()) + "; that is " + std::to_string(kBreakerAfter) + " rate limits in a short time, so MAIC sends nothing to " + provider.name +
                                        " for " + in_seconds(milliseconds(window_ms)) + " (the circuit breaker)",
                               e.retry_after_ms, e.type);
            }
            bool retryable = limit || e.status == 408 || e.status == 409 || e.status >= 500;
            if (!retryable || streamed || attempt >= options.retries) throw;
            why = "HTTP " + std::to_string(e.status);
        } catch (const TransportError& e) {
            leave(account, options.model, cap);
            // An answer lost while being read may mean the request ran to the end; on a metered provider that is billed again.
            if (streamed || attempt >= options.retries || (provider.metered() && !e.retry_safe)) throw;
            why = e.what();
        } catch (...) {
            leave(account, options.model, cap);
            throw;
        }
        if (options.notice) {
            options.notice(provider.name + ": " + why + "; retrying in " + std::to_string((wait_ms + 500) / 1000) + " s (" + std::to_string(attempt + 1) +
                           "/" + std::to_string(options.retries) + ")");
        }
        if (!sleep_unless_cancelled(wait_ms, cancel)) throw Cancelled();
    }
}

bool is_usage_limit(const ApiError& e) {
    std::string m = e.what(), t = e.type;
    for (auto* s : {&m, &t}) {
        for (auto& c : *s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto has = [&](const char* w) { return m.find(w) != std::string::npos; };
    if (e.status == 402 || t == "insufficient_quota" || has("insufficient_quota")) return true;
    if (e.status == 400 && has("credit balance is too low")) return true;  // Anthropic, prepaid credit gone
    if (e.status != 429) return false;
    // A 429 is a usage limit when it says the allowance is gone; "rate limit ... per minute" and "slow down"
    // are the kind that clears by itself.
    for (const char* w : {"usage limit", "reached your", "usage-credits", "quota", "credits", "exhausted", "billing"}) {
        if (has(w)) return true;
    }
    return false;
}

std::string generate_title(const Provider& provider, const std::string& model, const std::string& first_prompt) {
    std::vector<Message> req = {
        {"system", "Write a title for a conversation that starts with the message below: one line, at most 50 characters, no quotes, "
                   "keep exact filenames and technical terms, drop articles. Output only the title."},
        {"user", first_prompt.substr(0, 2000)},
    };
    std::atomic<bool> no{false};
    std::string t = chat(provider, ChatOptions{model, false}, req, nlohmann::json::array(), [](std::string_view, bool) {}, no).content;
    if (auto p = t.find("</think>"); p != std::string::npos) t = t.substr(p + 8);
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ' || t.back() == '.' || t.back() == '"')) t.pop_back();
    while (!t.empty() && (t.front() == '\n' || t.front() == ' ' || t.front() == '"')) t.erase(0, 1);
    if (t.size() > 80 || t.find('\n') != std::string::npos) return "";
    return t;
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

void throw_api_error(const std::string& provider, const HttpResult& r, const std::string& hint) {
    // Anthropic: {"error": {"type": "rate_limit_error", ...}}; OpenAI: {"error": {"type"/"code": "insufficient_quota", ...}}.
    std::string type;
    auto j = nlohmann::json::parse(r.error_body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        const auto& e = j["error"];
        if (e.contains("type") && e["type"].is_string()) type = e["type"];
        if ((type.empty() || type == "error") && e.contains("code") && e["code"].is_string()) type = e["code"];
    }
    throw ApiError(r.status, api_error(provider, r) + hint, r.retry_after_ms, type);
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
    trust_system_cas(client);

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
        throw TransportError("can't reach " + base_url + " (" + httplib::to_string(err) + ")", err != httplib::Error::Read);
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
