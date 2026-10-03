#include "server.hpp"

#include "artifacts.hpp"
#include "auth.hpp"
#include "home_link.hpp"
#include "tls.hpp"
#include "tunnel.hpp"

#include "maic/engine.hpp"
#include "maic/full_output.hpp"
#include "maic/paths.hpp"
#include "maic/session.hpp"
#include "maic/trust.hpp"

#include "maic/http.hpp"
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>

namespace maic::server {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A handler throws one of these for a client error; anything else is a 500.
struct HttpError {
    int status;
    std::string message;
};

// The engine's refusal as an HTTP status: its JSON-RPC code, then OpenAI's or MAIC's code in data.
HttpError http_error(const json& err) {
    int code = err.value("code", -32603);
    std::string data = err.contains("data") && err["data"].value("code", json()).is_string() ? err["data"]["code"].get<std::string>() : "";
    std::string message = err.value("message", "the engine refused the request");
    if (data == "maic_not_found") return {404, message};
    if (data == "maic_forbidden_remote" || data == "maic_step_up_required") return {403, message};
    if (data == "maic_already_answered" || data == "maic_resync" || data == "maic_busy" || data == "response_not_active") return {409, message};
    if (code == -32601) return {404, message};
    if (code == -32602 || code == -32600) return {400, message};
    return {500, message};
}

// One HTTP request's connection to the engine: remote, named by the request's token, said hello, and gone with the request.
struct Conn {
    Engine& engine;
    std::string id;
    int next = 1;

    Conn(Engine& e, const std::string& name) : engine(e), id(e.connect(Origin::Remote, name, "http")) {
        call("maic.hello", {{"protocol", 1}, {"client", {{"name", name}}}, {"capabilities", {"tool_output"}}});
    }
    ~Conn() { engine.disconnect(id); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    json call(const std::string& method, json params = json::object()) {
        json reply = engine.call(id, {{"jsonrpc", "2.0"}, {"id", next++}, {"method", method}, {"params", std::move(params)}});
        if (reply.contains("error")) throw http_error(reply["error"]);
        return reply["result"];
    }
};

// The session as the HTTP API has shown it: the index entry's fields, the stream position, the waiting approval.
json session_json(const json& snapshot) {
    const json& e = snapshot["entry"];
    return {{"id", e["id"]},
            {"title", e["title"]},
            {"workspace", e["workspace"]},
            {"model", e["model"]},
            {"mode", e["mode"]},
            {"remote_model", e.value("remote_model", false)},
            {"running", e["activity"] != "idle"},
            {"turns", e.value("turns", 0)},
            {"created", e.value("created", "")},
            {"sequence_number", snapshot["sequence_number"]},
            {"pending_approval", snapshot["pending"].empty() ? json() : snapshot["pending"][0]}};
}

// The transcript folded for display, from the session file: the entries the web client renders above the stream.
json transcript_json(const fs::path& file) {
    json out = json::array();
    for (const auto& t : load_session(file).transcript) {
        if (t.type == "assistant") out.push_back({{"type", "text"}, {"text", t.text}, {"thinking", false}});
        else if (t.type == "tool_call") out.push_back({{"type", "tool_call"}, {"summary", t.text}});
        else if (t.type == "tool_result") {
            json r = {{"type", "tool_result"}, {"text", t.text}, {"ok", t.ok}};
            out.push_back(r);
        } else {
            out.push_back({{"type", t.type}, {"text", t.text}});
        }
    }
    return out;
}

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The policy every response under /a/ starts with, errors and redirects included: a URL opened directly has no
// iframe around it. An artifact's own files get artifact_csp's in its place.
constexpr const char* kSealedCsp = "sandbox; default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
constexpr const char* kArtifactCookie = "maic_artifacts";

void set_csp(httplib::Response& res, const std::string& csp) {
    res.headers.erase("Content-Security-Policy");
    res.set_header("Content-Security-Policy", csp);
}

std::string cookie(const httplib::Request& req, const std::string& name) {
    std::string all = req.get_header_value("Cookie");
    for (size_t pos = 0; pos < all.size();) {
        size_t end = all.find(';', pos);
        std::string part = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        part.erase(0, part.find_first_not_of(' '));
        if (part.rfind(name + "=", 0) == 0) return part.substr(name.size() + 1);
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

// data/<name>.json: the name, or a 404 for anything else under data/.
std::string data_name(const std::string& file) {
    std::string name = file.size() > 5 && file.ends_with(".json") ? file.substr(0, file.size() - 5) : "";
    if (!artifact_name_ok(name)) throw HttpError{404, "data documents are data/NAME.json, NAME of letters, digits, '_' or '-'"};
    return name;
}

}  // namespace

struct Server::Impl {
    explicit Impl(ServerOptions o) : options(std::move(o)), tokens(options.state / "tokens.json"), pairs(options.state / "pairs.json") {}

    ServerOptions options;
    std::unique_ptr<httplib::Server> srv;
    std::string host;
    int port = 0;
    bool tls = false;
    std::string fingerprint;
    std::vector<fs::path> roots;
    std::atomic<bool> stopping{false};

    std::mutex tokens_mu;
    TokenStore tokens;
    RateLimit rate;
    std::mutex audit_mu;
    std::mutex pairs_mu;
    PairStore pairs;
    std::unique_ptr<HomeLink> home;  // the outbound connection to server.relay, when one is set
    ArtifactGrants grants;
    std::mutex data_mu;  // one artifact data write at a time

    // The sessions, their streams and approvals: every HTTP request is one connection to it, remote, named by its token.
    std::unique_ptr<Engine> engine;

    std::optional<std::string> token_name(const httplib::Request& req) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.rfind("Bearer ", 0) != 0) return std::nullopt;
        std::lock_guard lock(tokens_mu);
        tokens.refresh();
        return tokens.verify(auth.substr(7));
    }

    void audit(const httplib::Request& req, const httplib::Response& res) {
        // A request out of the relay tunnel arrives from our own HomeLink on loopback; it names the phone.
        std::string source = req.remote_addr;
        if (req.has_header("X-Maic-Via") && (source == "127.0.0.1" || source == "::1")) source = req.get_header_value("X-Maic-Via");
        // An artifact capability in a path is a credential: the log keeps where it was used, never what it was.
        std::string path = req.path;
        if (size_t t = path.find('~'); path.rfind("/a/", 0) == 0 && t != std::string::npos) path.replace(t + 1, path.find('/', t) - t - 1, "*");
        std::string line = utc_now() + " " + source + " " + token_name(req).value_or("-") + " " + req.method + " " + path + " " +
                           std::to_string(res.status) + "\n";
        std::lock_guard lock(audit_mu);
        int fd = open((options.state / "audit.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd < 0) return;
        ssize_t n = write(fd, line.data(), line.size());
        (void)n;
        close(fd);
    }

    static void fail(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(dump({{"error", message}}), "application/json");
    }

    static void reply(httplib::Response& res, const json& body, int status = 200) {
        res.status = status;
        res.set_content(dump(body), "application/json");
    }

    static json body_of(const httplib::Request& req) {
        if (req.body.empty()) return json::object();
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object()) throw HttpError{400, "the request body must be a JSON object"};
        return j;
    }

    Conn conn(const httplib::Request& req) { return Conn(*engine, token_name(req).value_or("-")); }

    // attach, for what the session looks like now; the connection goes with the request.
    json snapshot(Conn& c, const std::string& id) { return c.call("maic.session.attach", {{"session", id}}); }

    // The session's file, for its history and kept outputs; only a session the engine has loaded.
    fs::path transcript(Conn& c, const std::string& id) {
        c.call("getConversation", {{"conversation_id", id}});
        auto info = find_session(id);
        if (!info) throw HttpError{404, "no session " + id};
        return info->path;
    }

    json status_json(Conn& c) {
        json st = c.call("maic.engine.status");
        json relay;
        if (home) {
            HomeLink::State hs = home->state();
            relay = {{"url", options.settings.server.relay}, {"connected", hs.connected}, {"since", hs.since}, {"last_connected", hs.last_connected}, {"error", hs.error}};
        }
        st["relay"] = relay;
        st["listen"] = host + ":" + std::to_string(port);
        st["tls"] = tls;
        st["fingerprint"] = fingerprint;
        return st;
    }

    // The session's events as server-sent events, each one a maic.event's params as the engine sent it, until the
    // turn that was running (or that this request started) is over: an idle maic.session.state numbered after
    // `idle_after`, or, when nothing ran, everything up to `replay_to`. A comment line every 15 s keeps the
    // connection open through silence.
    void stream(std::shared_ptr<Conn> c, long idle_after, long replay_to, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, c, idle_after, replay_to](size_t, httplib::DataSink& sink) {
                if (stopping.load()) return false;
                std::string out;
                bool finished = false;
                for (const auto& m : engine->take(c->id, std::chrono::seconds(15))) {
                    if (m.value("method", "") != "maic.event") continue;
                    const json& e = m["params"];
                    out += "data: " + dump(e) + "\n\n";
                    long n = e.value("sequence_number", -1L);
                    if (e["type"] == "maic.session.state" && e["activity"] == "idle" && n > idle_after) finished = true;
                    if (replay_to >= 0 && n >= replay_to) finished = true;
                }
                if (!engine->closed(c->id).empty()) finished = true;
                if (out.empty() && !finished) out = ": keepalive\n\n";
                if (!out.empty() && !sink.write(out.data(), out.size())) return false;
                if (finished) sink.done();
                return true;
            },
            [c](bool) {});
    }

    // An artifact's files: scripts from its own files and /a/_vendor/ only, no network but its own data routes, and
    // an opaque origin, so it never acts with the server's cookies or storage. The sources are absolute, built from
    // Host, because 'self' in a sandboxed page is not dependable across browsers.
    std::string artifact_csp(const httplib::Request& req, const std::string& prefix) {
        std::string host = req.get_header_value("Host");
        bool plain = !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'; });
        if (!plain) throw HttpError{400, "an artifact request needs a plain Host header"};
        std::string origin = (tls ? "https://" : "http://") + host, own = origin + prefix;
        return "sandbox allow-scripts allow-forms allow-modals allow-downloads; default-src 'none'; script-src " + own + " " + origin + "/a/_vendor/; style-src " + own +
               " 'unsafe-inline'; img-src " + own + " data: blob:; font-src " + own + " data:; media-src " + own + " blob:; connect-src " + own +
               "data/; form-action 'none'; base-uri 'none'; frame-ancestors 'none'";
    }

    // A person: a bearer token (a device, or the web client through the relay) or the login cookie from
    // `maic artifact open`. The cookie counts only on the server's own pages, never from a sandboxed page (Origin
    // null) or another site; SameSite=Strict keeps it from those already, this says so twice.
    void require_person(const httplib::Request& req) {
        std::string site = req.get_header_value("Sec-Fetch-Site");
        bool own = req.get_header_value("Origin") != "null" && site != "cross-site" && site != "same-site";
        if (token_name(req) || (own && grants.logged_in(cookie(req, kArtifactCookie)))) {
            rate.succeeded(req.remote_addr);
            return;
        }
        rate.failed(req.remote_addr);
        throw HttpError{401, "open an artifact with `maic artifact open ID` on the workstation, or send a bearer token"};
    }

    fs::path artifact_dir(const std::string& id) {
        std::error_code ec;
        if (!artifact_name_ok(id) || !fs::is_directory(options.artifacts / id, ec)) throw HttpError{404, "no artifact " + id};
        return options.artifacts / id;
    }

    // One of an artifact's files, or a data document under data/, under the artifact's policy. Under a capability,
    // index.html carries it in a meta tag for a page that sends it back as X-Maic-Artifact-Token.
    void serve_artifact(const httplib::Request& req, httplib::Response& res, const std::string& id, const std::string& prefix, const std::string& rel,
                        const std::string& cap) {
        fs::path dir = artifact_dir(id);
        set_csp(res, artifact_csp(req, prefix));
        if (rel.rfind("data/", 0) == 0) {
            std::string name = data_name(rel.substr(5));
            auto file = artifact_file(dir, "data/" + name + ".json");
            if (!file) throw HttpError{404, "no data document " + name + " yet"};
            std::string body = read_file(*file);
            res.set_header("ETag", "\"" + data_rev(body) + "\"");
            res.set_header("X-Rev", data_rev(body));
            res.set_content(body, "application/json");
            return;
        }
        auto file = artifact_file(dir, rel);
        if (!file) throw HttpError{404, "no file " + rel + " in artifact " + id};
        std::string body = read_file(*file);
        if (!cap.empty() && (rel.empty() || rel == "index.html")) body = page_with_token(std::move(body), cap);
        res.set_content(body, artifact_content_type(*file));
    }

    // A data document written whole: If-Match with the revision last read (or If-None-Match: * to create it), the
    // body checked as JSON, then a temporary file renamed over it. A stale revision is a 409 naming the current one.
    // The body is read only once the request is authorized, and never more than the cap of it is kept.
    void put_data(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& read, const fs::path& dir, const std::string& file) {
        std::string name = data_name(file), body;
        bool over = false;
        read([&](const char* data, size_t n) {
            over = over || body.size() + n > kArtifactDataMax;
            if (!over) body.append(data, n);
            return true;
        });
        if (over) throw HttpError{413, "a data document is at most 1 MiB"};
        if (json::parse(body, nullptr, false).is_discarded()) throw HttpError{400, "the body is not JSON"};
        std::string match = req.get_header_value("If-Match");
        if (match.rfind("W/", 0) == 0) match = match.substr(2);
        if (match.size() >= 2 && match.front() == '"' && match.back() == '"') match = match.substr(1, match.size() - 2);
        std::lock_guard lock(data_mu);
        std::optional<std::string> current;
        try {
            current = data_revision(dir, name);
        } catch (const std::runtime_error& e) {
            throw HttpError{403, e.what()};
        }
        bool fresh;
        if (!match.empty()) fresh = current && (match == "*" || match == *current);
        else if (req.get_header_value("If-None-Match") == "*") fresh = !current;
        else throw HttpError{428, "send If-Match with the revision you read (its ETag), or If-None-Match: * to create the document"};
        std::string rev = fresh ? data_rev(body) : current.value_or("");
        if (!rev.empty()) {
            res.set_header("ETag", "\"" + rev + "\"");
            res.set_header("X-Rev", rev);
        }
        if (!fresh) {
            reply(res, {{"error", "stale: data/" + name + ".json changed since that revision"}, {"rev", current ? json(*current) : json()}}, 409);
            return;
        }
        write_data(dir, name, body);
        reply(res, {{"rev", rev}}, current ? 200 : 201);
    }

    void artifact_routes() {
        // Vendored libraries for every artifact: public code, so no login. A module import from a sandboxed page is
        // a CORS request.
        srv->Get(R"(/a/_vendor/vue/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto file = options.vue.empty() ? std::nullopt : artifact_file(options.vue, req.matches[1].str());
            if (!file) throw HttpError{404, "no vendored file " + req.matches[1].str()};
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(read_file(*file), artifact_content_type(*file));
        });

        // The link `maic artifact open` prints: its one-time code becomes the browser's login (an HttpOnly cookie for
        // /a/ only) and a capability for the artifact it names, so the first visit needs no cookie.
        srv->Get("/a/_login", [this](const httplib::Request& req, httplib::Response& res) {
            if (!claim_artifact_login(options.state, req.get_param_value("code"))) {
                rate.failed(req.remote_addr);
                throw HttpError{401, "this link was used already or has expired; run `maic artifact open ID` again"};
            }
            rate.succeeded(req.remote_addr);
            res.set_header("Set-Cookie", std::string(kArtifactCookie) + "=" + grants.login() + "; Path=/a/; HttpOnly; SameSite=Strict; Max-Age=43200" + (tls ? "; Secure" : ""));
            std::string id = req.get_param_value("to");
            artifact_dir(id);
            res.set_redirect("/a/" + id + "~" + grants.grant(id) + "/", 303);
        });

        srv->Get(R"(/a/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
            if (!artifact_name_ok(req.matches[1].str())) throw HttpError{404, "no artifact " + req.matches[1].str()};
            res.set_redirect("/a/" + req.matches[1].str() + "/", 301);
        });

        // Under a capability, /a/<id>~<capability>/: the artifact's files and data, for the page itself. Its origin is
        // opaque, so these are CORS requests without cookies, and the capability in the path is their only
        // credential: for this artifact, nothing else. One path segment, so ../_vendor/ from the page is /a/_vendor/.
        auto capability = [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1], cap = req.matches[2], header = req.get_header_value("X-Maic-Artifact-Token");
            if (!grants.granted(cap, id) || (!header.empty() && header != cap)) {
                rate.failed(req.remote_addr);
                return false;
            }
            rate.succeeded(req.remote_addr);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Expose-Headers", "ETag, X-Rev");
            return true;
        };
        srv->Get(R"(/a/([^/~]+)~([^/]+)/(.*))", [this, capability](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1], cap = req.matches[2], rel = req.matches[3];
            if (!capability(req, res)) {
                // An old tab, after a restart or 12 hours: the login cookie, when the browser has one, grants a new one.
                if (rel.empty() && artifact_name_ok(id)) {
                    res.set_redirect("/a/" + id + "/", 303);
                    return;
                }
                throw HttpError{401, "this page's capability is not valid (expired, or for another artifact); open the artifact again"};
            }
            serve_artifact(req, res, id, "/a/" + id + "~" + cap + "/", rel, cap);
        });
        srv->Put(R"(/a/([^/~]+)~([^/]+)/data/([^/]+))", [this, capability](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& read) {
            if (!capability(req, res)) throw HttpError{401, "this page's capability is not valid (expired, or for another artifact); open the artifact again"};
            put_data(req, res, read, artifact_dir(req.matches[1]), req.matches[3]);
        });
        srv->Options(R"(/a/([^/~]+)~([^/]+)/data/([^/]+))", [capability](const httplib::Request& req, httplib::Response& res) {
            if (!capability(req, res)) throw HttpError{401, "this page's capability is not valid"};
            res.set_header("Access-Control-Allow-Methods", "GET, PUT");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, If-Match, If-None-Match, X-Maic-Artifact-Token");
            res.set_header("Access-Control-Max-Age", "600");
            res.status = 204;
        });

        // A person's own requests. The page itself is never served here: it is sent on to a fresh capability, so the
        // requests it makes carry one.
        srv->Get(R"(/a/([^/]+)/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
            require_person(req);
            std::string id = req.matches[1], rel = req.matches[2];
            artifact_dir(id);
            if (rel.empty() || rel == "index.html") {
                res.set_redirect("/a/" + id + "~" + grants.grant(id) + "/", 303);
                return;
            }
            serve_artifact(req, res, id, "/a/" + id + "/", rel, "");
        });
        srv->Put(R"(/a/([^/]+)/data/([^/]+))", [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& read) {
            require_person(req);
            put_data(req, res, read, artifact_dir(req.matches[1]), req.matches[2]);
        });
    }

    void routes() {
        srv->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            bool artifact = req.path.rfind("/a/", 0) == 0;
            if (artifact) {
                set_csp(res, kSealedCsp);
                res.set_header("X-Content-Type-Options", "nosniff");
                res.set_header("X-Frame-Options", "DENY");
                res.set_header("Referrer-Policy", "no-referrer");
                res.set_header("Cache-Control", "no-store");
            } else if (req.get_header_value("Origin") == "null" || req.has_header("X-Maic-Artifact-Token")) {
                // A sandboxed artifact page (its origin is opaque) or its capability: never anything outside /a/.
                fail(res, 403, "an artifact page has no access outside /a/");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.path == "/") return httplib::Server::HandlerResponse::Unhandled;
            if (rate.blocked(req.remote_addr)) {
                fail(res, 429, "too many failed attempts from this address; try again in a minute");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (artifact) return httplib::Server::HandlerResponse::Unhandled;  // each artifact route checks its own credential
            if (!token_name(req)) {
                rate.failed(req.remote_addr);
                res.set_header("WWW-Authenticate", "Bearer");
                fail(res, 401, "a bearer token from `maic server token new` is required");
                return httplib::Server::HandlerResponse::Handled;
            }
            rate.succeeded(req.remote_addr);
            return httplib::Server::HandlerResponse::Unhandled;
        });
        srv->set_logger([this](const httplib::Request& req, const httplib::Response& res) { audit(req, res); });
        // A refusal httplib makes before routing (a bad Range, say) is sandboxed under /a/ too.
        srv->set_error_handler(httplib::Server::HandlerWithResponse([](const httplib::Request& req, httplib::Response& res) {
            if (req.path.rfind("/a/", 0) == 0 && !res.has_header("Content-Security-Policy")) res.set_header("Content-Security-Policy", kSealedCsp);
            return httplib::Server::HandlerResponse::Unhandled;
        }));
        srv->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const HttpError& e) {
                fail(res, e.status, e.message);
            } catch (const std::exception& e) {
                fail(res, 500, e.what());
            }
        });

        srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream in(options.web);
            if (!in) {
                res.status = 404;
                res.set_content("no web client at " + options.web.string() + "\n", "text/plain");
                return;
            }
            res.set_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "text/html; charset=utf-8");
        });

        artifact_routes();

        srv->Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            reply(res, status_json(c));
        });

        // The loaded sessions, from the engine's index.
        srv->Get("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            json index = c.call("maic.index.get");
            json out = json::array();
            for (const auto& e : index["entries"]) {
                if (e["state"] != "live" && e["state"] != "background") continue;
                out.push_back({{"id", e["id"]}, {"title", e["title"]}, {"workspace", e["workspace"]}, {"model", e["model"]}, {"mode", e["mode"]},
                               {"remote_model", e.value("remote_model", false)}, {"running", e["activity"] != "idle"}, {"turns", e.value("turns", 0)},
                               {"created", e.value("created", "")}});
            }
            reply(res, {{"sessions", out}});
        });

        srv->Post("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            Conn c = conn(req);
            std::string id;
            if (body.contains("resume")) {
                id = c.call("maic.session.resume", {{"session", body["resume"].get<std::string>()}})["id"];
                if (body.contains("mode")) c.call("maic.session.set", {{"session", id}, {"mode", body["mode"]}});
            } else {
                json m = json::object();  // no mode: the engine starts the settings' own, held at manual where auto waits
                if (body.contains("mode")) m["mode"] = body["mode"];
                if (body.contains("workspace")) m["workspace"] = body["workspace"];
                if (body.contains("model")) m["model"] = body["model"];
                id = c.call("createConversation", {{"maic", m}})["id"];
            }
            reply(res, session_json(snapshot(c, id)), 201);
        });

        srv->Get(R"(/api/sessions/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            std::string id = req.matches[1];
            json snap = snapshot(c, id);
            json j = session_json(snap);
            const json& u = snap["usage"];
            j["usage"] = {{"input", u["total"]["input"]}, {"output", u["total"]["output"]}, {"calls", u["calls"]}, {"context", u["context"]}, {"last_input", u["last_input"]}};
            j["entries"] = transcript_json(transcript(c, id));
            reply(res, j);
        });

        // The session's events after starting_after (`after=N`, the older spelling, meant from N on).
        srv->Get(R"(/api/sessions/([^/]+)/events)", [this](const httplib::Request& req, httplib::Response& res) {
            auto c = std::make_shared<Conn>(*engine, token_name(req).value_or("-"));
            long after = -1;
            if (req.has_param("starting_after")) after = std::stol(req.get_param_value("starting_after"));
            else if (req.has_param("after")) after = std::stol(req.get_param_value("after")) - 1;
            json sub = c->call("maic.session.subscribe", {{"session", req.matches[1].str()}, {"starting_after", after}});
            long at = sub["sequence_number"];
            if (sub["activity"] == "idle") {
                if (after >= at) {
                    res.set_content("", "text/event-stream");
                    return;
                }
                stream(c, at, at, res);
            } else {
                stream(c, at, -1, res);
            }
        });

        // A command's kept output (docs/sessions.md, Full output), 256 KiB at a time: the session's own, or one of its
        // subagents' (`session`). Display only, and labelled so.
        srv->Get(R"(/api/sessions/([^/]+)/output/([A-Za-z0-9_-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            std::string id = req.matches[1];
            fs::path owner = transcript(c, id);
            if (req.has_param("session") && req.get_param_value("session") != id) {
                std::string sub = req.get_param_value("session");
                auto subs = sub_sessions_of(owner);
                auto it = std::find_if(subs.begin(), subs.end(), [&](const fs::path& p) { return p.stem().string() == sub; });
                if (it == subs.end()) throw HttpError{404, "no subagent session " + sub + " of " + id};
                owner = *it;
            }
            fs::path file = side_dir(owner) / (req.matches[2].str() + ".out");
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) throw HttpError{404, "no kept output " + req.matches[2].str()};
            size_t size = fs::file_size(file, ec);
            size_t offset = req.has_param("offset") ? std::stoul(req.get_param_value("offset")) : 0;
            size_t length = std::min<size_t>(req.has_param("length") ? std::stoul(req.get_param_value("length")) : 256 * 1024, 256 * 1024);
            std::ifstream in(file, std::ios::binary);
            std::string data(std::min(length, size > offset ? size - offset : 0), '\0');
            in.seekg(static_cast<std::streamoff>(std::min(offset, size)));
            in.read(data.data(), static_cast<std::streamsize>(data.size()));
            reply(res, {{"label", kFullOutputLabel}, {"bytes", size}, {"offset", offset}, {"data", data}, {"done", offset + data.size() >= size}});
        });

        // response.create, then the session's stream from just before its input until the turn is over. Mid-turn the
        // message is response.steer: it reaches the running response at its next boundary (or resumes a paused turn).
        srv->Post(R"(/api/sessions/([^/]+)/messages)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string text = body_of(req).value("text", "");
            if (text.empty()) throw HttpError{400, "text is empty"};
            auto c = std::make_shared<Conn>(*engine, token_name(req).value_or("-"));
            std::string id = req.matches[1];
            json entry = c->call("getConversation", {{"conversation_id", id}})["maic"]["entry"];
            if (entry["response"].is_string()) {
                json snap = c->call("maic.session.attach", {{"session", id}});
                try {
                    c->call("response.steer", {{"previous_response_id", entry["response"]}, {"input", text}});
                    stream(c, snap["sequence_number"], -1, res);
                    return;
                } catch (const HttpError&) {
                    c->call("maic.session.unsubscribe", {{"session", id}});  // it ended meanwhile: the message starts a turn
                }
            }
            json r = c->call("response.create", {{"conversation", id}, {"input", text}});
            long from = r["maic"]["sequence_number"];
            c->call("maic.session.subscribe", {{"session", id}, {"starting_after", from}});
            stream(c, from, -1, res);
        });

        srv->Post(R"(/api/sessions/([^/]+)/approvals/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            Conn c = conn(req);
            json params = {{"session", req.matches[1].str()}, {"approval", req.matches[2].str()}, {"choice", body.value("choice", "")}};
            if (body.contains("feedback")) params["feedback"] = body["feedback"];
            json r = c.call("maic.approval.answer", params);
            reply(res, {{"id", req.matches[2].str()}, {"choice", r["choice"]}});
        });

        srv->Post(R"(/api/sessions/([^/]+)/interrupt)", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            json entry = c.call("getConversation", {{"conversation_id", req.matches[1].str()}})["maic"]["entry"];
            if (entry["response"].is_null()) {
                reply(res, {{"running", false}});
                return;
            }
            try {
                c.call("cancelResponse", {{"response_id", entry["response"]}});
            } catch (const HttpError& e) {
                if (e.status != 404) throw;
                reply(res, {{"running", false}});  // it ended meanwhile
                return;
            }
            reply(res, {{"running", true}, {"interrupting", true}});
        });

        srv->Post(R"(/api/sessions/([^/]+)/mode)", [this](const httplib::Request& req, httplib::Response& res) {
            Conn c = conn(req);
            std::string id = req.matches[1];
            c.call("maic.session.set", {{"session", id}, {"mode", body_of(req).value("mode", "")}});
            reply(res, session_json(snapshot(c, id)));
        });

        // Trust from a paired device (docs/harness.md, Trust): only with a step-up proof the registered verifier
        // accepts. Until accounts exist none is registered and every request is refused. Each request, done or
        // refused, is a line in <state>/trust-audit.log naming the device.
        srv->Post("/api/trust", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            try {
                std::string done = remote_trust_change(token_name(req).value_or("-"), body.value("step_up", ""), body.value("action", ""), body.value("path", ""), body.value("level", ""),
                                                       body.value("lua", ""));
                reply(res, {{"done", done}});
            } catch (const std::runtime_error& e) {
                throw HttpError{403, e.what()};
            }
        });

        // The pairing exchange, on the LAN only: the phone proves it saw the code `maic server pair` printed and
        // leaves its public key; it gets ours, the pairing id and the relay to use. Nothing here reaches the
        // relay, and the relay has no counterpart to this route.
        srv->Post("/api/pair", [this](const httplib::Request& req, httplib::Response& res) {
            json body = body_of(req);
            std::string code = body.value("code", ""), name = body.value("name", "phone");
            auto pk = key_from_b64url(body.value("public_key", ""));
            if (!pk) throw HttpError{400, "public_key must be 32 bytes of base64url"};
            if (options.settings.server.relay.empty()) throw HttpError{409, "no relay is configured (server.relay in settings)"};
            if (auto why = claim_pairing_offer(options.state / "pairing.json", code)) throw HttpError{403, *why};
            std::lock_guard lock(pairs_mu);
            pairs.refresh();
            pairs.add(name, *pk);
            reply(res, {{"name", name}, {"pairing_id", pairs.pairing_id()}, {"relay", options.settings.server.relay}, {"public_key", b64url(pairs.keypair().pk)}}, 201);
        });

        // The panic button works from a phone; the reset does not exist here (it needs the local sudo password).
        srv->Post("/api/trip", [this](const httplib::Request& req, httplib::Response& res) {
            std::string reason = body_of(req).value("reason", "tripped from a remote client");
            Conn c = conn(req);
            reply(res, c.call("maic.engine.trip", {{"reason", reason}}));
        });
    }
};

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Server::~Server() {
    stop();
    impl_->srv.reset();
    impl_->engine.reset();
}

int Server::bind() {
    Impl& im = *impl_;
    const ServerOptions& o = im.options;
    std::string listen = o.listen;
    size_t colon = listen.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("listen address must be ADDR:PORT: " + listen);
    im.host = listen.substr(0, colon);
    if (im.host.size() > 1 && im.host.front() == '[' && im.host.back() == ']') im.host = im.host.substr(1, im.host.size() - 2);
    int port = std::stoi(listen.substr(colon + 1));
    if (im.host.empty()) im.host = "0.0.0.0";

    fs::create_directories(o.state);
    std::error_code ec;
    fs::permissions(o.state, fs::perms::owner_all, fs::perm_options::replace, ec);
    for (const auto& w : o.workspaces) im.roots.push_back(fs::weakly_canonical(w, ec));
    if (im.roots.empty()) throw std::runtime_error("no allowed workspace root");
    // Every maic-server client is remote; its sessions are kind server. The session index file is the daemon's
    // (step 13), so this engine keeps none.
    EngineOptions eo;
    eo.settings = o.settings;
    eo.tier = o.settings.protocol_tier;
    eo.workspaces = im.roots;
    eo.kind = "server";
    eo.keeps_sessions = true;
    im.engine = std::make_unique<Engine>(std::move(eo));

    bool loopback = loopback_host(im.host);
    fs::path cert = o.settings.server.cert, key = o.settings.server.key;
    if (cert.empty() != key.empty()) throw std::runtime_error("server.cert and server.key go together");
    // Off loopback TLS is not optional; on loopback it is used when a pair is configured.
    if (loopback && cert.empty()) {
        im.srv = std::make_unique<httplib::Server>();
    } else {
        TlsPair pair;
        if (cert.empty()) {
            bool any = im.host == "0.0.0.0" || im.host == "::";
            std::vector<std::string> hosts = any ? interface_addresses() : std::vector<std::string>{im.host};
            char name[256] = {};
            if (gethostname(name, sizeof(name) - 1) == 0 && name[0]) hosts.push_back(name);
            pair = ensure_self_signed(o.state / "cert.pem", o.state / "key.pem", hosts);
        } else {
            pair = {cert, key, cert_fingerprint(cert)};
        }
        auto ssl = std::make_unique<httplib::SSLServer>(pair.cert.c_str(), pair.key.c_str());
        if (!ssl->is_valid()) throw std::runtime_error("can't load the TLS pair " + pair.cert.string() + " / " + pair.key.string());
        im.srv = std::move(ssl);
        im.tls = tls_ = true;
        im.fingerprint = fingerprint_ = pair.fingerprint;
    }
    im.srv->new_task_queue = [] { return new httplib::ThreadPool(16); };
    im.srv->set_keep_alive_max_count(1000);
    im.routes();
    im.port = port == 0 ? im.srv->bind_to_any_port(im.host) : (im.srv->bind_to_port(im.host, port) ? port : -1);
    if (im.port <= 0) throw std::runtime_error("can't listen on " + im.host + ":" + std::to_string(port) + " (in use, or not an address of this machine)");
    return im.port;
}

void Server::run() {
    Impl& im = *impl_;
    const ServerSettings& ss = im.options.settings.server;
    if (!ss.relay.empty()) {
        bool any = im.host == "0.0.0.0" || im.host == "::";
        HomeLinkOptions h;
        h.relay = ss.relay;
        h.relay_cert = ss.relay_cert;
        h.pairs_file = im.options.state / "pairs.json";
        h.status_file = im.options.state / "relay.json";
        h.loopback = std::string(im.tls ? "https://" : "http://") + (any ? "127.0.0.1" : im.host) + ":" + std::to_string(im.port);
        im.home = std::make_unique<HomeLink>(std::move(h));
        im.home->start();
    }
    im.srv->listen_after_bind();
}

void Server::stop() {
    Impl& im = *impl_;
    im.stopping = true;
    if (im.home) im.home->stop();
    if (im.engine) im.engine->shutdown();
    if (im.srv) im.srv->stop();
}

}  // namespace maic::server
