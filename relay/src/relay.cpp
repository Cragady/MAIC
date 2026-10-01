#include "relay.hpp"

#include "tls.hpp"

#include "maic/http.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace maic::relay {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

constexpr int kHome = 0, kPhone = 1;
const char* side_name(int side) { return side == kHome ? "home" : "phone"; }

int parse_side(const std::string& s) {
    if (s == "home") return kHome;
    if (s == "phone") return kPhone;
    return -1;
}

// One side of a pair: its down stream, the frames waiting to go down it, and the bytes it has sent up that
// are not yet a whole frame.
struct Side {
    bool attached = false;
    std::deque<std::string> inbox;
    size_t inbox_bytes = 0;
    std::string partial;
    Clock::time_point last_seen;
    uint64_t sent = 0;  // bytes it sent up
};

struct Pair {
    std::string id;
    std::mutex mu;
    std::condition_variable cv;
    Side side[2];
    bool closed = false;
    Clock::time_point created = Clock::now();
    // The throughput cap: a token bucket holding one second of allowance; below zero it is a debt slept off.
    double allowance;
    Clock::time_point bucket_at = created;
};

}  // namespace

struct Relay::Impl {
    explicit Impl(RelayOptions o) : options(std::move(o)) {}

    RelayOptions options;
    std::unique_ptr<httplib::Server> srv;
    std::string host;
    int port = 0;
    std::atomic<bool> stopping{false};

    std::mutex mu;
    std::map<std::string, std::shared_ptr<Pair>> pairs;
    std::mutex log_mu;

    void log(const std::string& line) {
        if (!options.log) return;
        std::lock_guard lock(log_mu);
        *options.log << utc_now() << " " << line << "\n" << std::flush;
    }

    static void fail(httplib::Response& res, int status, const char* message) {
        res.status = status;
        res.set_content(std::string(message) + "\n", "text/plain");
    }

    // The pair for an id, made when there is none or when the old one is on its way out.
    std::shared_ptr<Pair> pair_for(const std::string& id, bool create) {
        std::lock_guard lock(mu);
        auto it = pairs.find(id);
        if (it != pairs.end()) {
            std::lock_guard plock(it->second->mu);
            if (!it->second->closed) return it->second;
            if (!create) return nullptr;
        } else if (!create) {
            return nullptr;
        }
        if (it == pairs.end() && pairs.size() >= options.max_pairs) return nullptr;
        auto p = std::make_shared<Pair>();
        p->id = id;
        p->allowance = static_cast<double>(options.rate);
        pairs[id] = p;
        return p;
    }

    // Ends both sides. Called with p->mu held.
    void close_locked(Pair& p, const char* why) {
        if (p.closed) return;
        p.closed = true;
        p.cv.notify_all();
        log("close " + p.id + " " + why + " home_sent=" + std::to_string(p.side[kHome].sent) + " phone_sent=" + std::to_string(p.side[kPhone].sent) +
            " after=" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - p.created).count()) + "s");
    }

    void detach(const std::shared_ptr<Pair>& p, int side) {
        bool gone;
        {
            std::lock_guard lock(p->mu);
            p->side[side].attached = false;
            close_locked(*p, side == kHome ? "home left" : "phone left");
            gone = !p->side[kHome].attached && !p->side[kPhone].attached;
        }
        log("detach " + p->id + " " + side_name(side));
        if (!gone) return;
        std::lock_guard lock(mu);
        auto it = pairs.find(p->id);
        if (it != pairs.end() && it->second == p) pairs.erase(it);
    }

    // How long `n` bytes must wait under the cap. Called with p->mu held.
    double delay_for(Pair& p, size_t n) {
        auto now = Clock::now();
        double rate = static_cast<double>(options.rate);
        p.allowance = std::min(rate, p.allowance + rate * std::chrono::duration<double>(now - p.bucket_at).count());
        p.bucket_at = now;
        p.allowance -= static_cast<double>(n);
        return p.allowance < 0 ? -p.allowance / rate : 0;
    }

    // Bytes sent up by `side`: whole frames go to the other side's inbox, a zero-length frame is a keepalive
    // and goes no further. Returns false when the pair is over.
    bool feed(const std::shared_ptr<Pair>& p, int side, const char* data, size_t len) {
        double wait = 0;
        {
            std::lock_guard lock(p->mu);
            if (p->closed) return false;
            Side& from = p->side[side];
            Side& to = p->side[1 - side];
            from.last_seen = Clock::now();
            from.sent += len;
            from.partial.append(data, len);
            wait = delay_for(*p, len);
            while (from.partial.size() >= 4) {
                const auto* b = reinterpret_cast<const unsigned char*>(from.partial.data());
                size_t n = (size_t(b[0]) << 24) | (size_t(b[1]) << 16) | (size_t(b[2]) << 8) | b[3];
                if (n > options.max_frame) {
                    close_locked(*p, "frame too long");
                    return false;
                }
                if (from.partial.size() < 4 + n) break;
                if (n > 0) {
                    to.inbox.push_back(from.partial.substr(0, 4 + n));
                    to.inbox_bytes += 4 + n;
                    if (to.inbox_bytes > 8 * options.max_frame) {
                        close_locked(*p, "inbox overflow");
                        return false;
                    }
                }
                from.partial.erase(0, 4 + n);
            }
            p->cv.notify_all();
        }
        if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        return true;
    }

    void routes() {
        srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
            std::ifstream in(options.web);
            if (options.web.empty() || !in) {
                fail(res, 404, "this is a maic-relay; it serves no web client (start it with --web)");
                return;
            }
            res.set_content(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), "text/html; charset=utf-8");
        });

        // The down stream: frames from the other side, a zero-length frame as keepalive through silence.
        srv->Get(R"(/r/([A-Za-z0-9_-]{8,64})/(home|phone))", [this](const httplib::Request& req, httplib::Response& res) {
            int side = parse_side(req.matches[2]);
            auto p = pair_for(req.matches[1], true);
            if (!p) {
                fail(res, 503, "this relay holds as many pairings as it is allowed to");
                log("refuse " + req.matches[1].str() + " full");
                return;
            }
            {
                std::lock_guard lock(p->mu);
                if (p->side[side].attached) {
                    fail(res, 409, "that side of this pairing id is already connected");
                    log("refuse " + p->id + " " + side_name(side) + " duplicate");
                    return;
                }
                p->side[side].attached = true;
                p->side[side].last_seen = Clock::now();
            }
            log("attach " + p->id + " " + side_name(side));
            res.set_header("Cache-Control", "no-store");
            res.set_chunked_content_provider(
                "application/octet-stream",
                [this, p, side](size_t, httplib::DataSink& sink) {
                    std::string out;
                    {
                        std::unique_lock lock(p->mu);
                        Side& me = p->side[side];
                        p->cv.wait_for(lock, std::chrono::seconds(options.keepalive_seconds), [&] { return !me.inbox.empty() || p->closed || stopping.load(); });
                        if (p->closed || stopping.load()) {
                            sink.done();
                            return true;
                        }
                        auto now = Clock::now();
                        for (int s : {kHome, kPhone}) {
                            if (p->side[s].attached && now - p->side[s].last_seen > std::chrono::seconds(options.idle_seconds)) {
                                close_locked(*p, s == kHome ? "home idle" : "phone idle");
                                sink.done();
                                return true;
                            }
                        }
                        for (const auto& f : me.inbox) out += f;
                        me.inbox.clear();
                        me.inbox_bytes = 0;
                    }
                    if (out.empty()) out.assign(4, '\0');
                    if (sink.write(out.data(), out.size())) return true;
                    std::lock_guard lock(p->mu);
                    close_locked(*p, side == kHome ? "home unreachable" : "phone unreachable");
                    return false;
                },
                [this, p, side](bool) { detach(p, side); });
        });

        // The up direction: frames for the other side, as one long chunked request or one request per batch.
        srv->Post(R"(/r/([A-Za-z0-9_-]{8,64})/(home|phone))", [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
            int side = parse_side(req.matches[2]);
            auto p = pair_for(req.matches[1], false);
            if (!p) {
                fail(res, 409, "open the down stream (GET) before sending");
                return;
            }
            {
                std::lock_guard lock(p->mu);
                if (!p->side[side].attached) {
                    fail(res, 409, "open the down stream (GET) before sending");
                    return;
                }
            }
            bool ok = true;
            reader([&](const char* data, size_t len) {
                ok = feed(p, side, data, len);
                return ok && !stopping.load();
            });
            std::lock_guard lock(p->mu);
            if (p->closed || !ok) {
                fail(res, 410, "this pairing is over");
                return;
            }
            res.status = 204;
        });
    }
};

Relay::Relay(RelayOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Relay::~Relay() {
    stop();
}

int Relay::bind() {
    Impl& im = *impl_;
    const RelayOptions& o = im.options;
    std::string listen = o.listen;
    size_t colon = listen.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("listen address must be ADDR:PORT: " + listen);
    im.host = listen.substr(0, colon);
    if (im.host.size() > 1 && im.host.front() == '[' && im.host.back() == ']') im.host = im.host.substr(1, im.host.size() - 2);
    int port = std::stoi(listen.substr(colon + 1));
    if (im.host.empty()) im.host = "0.0.0.0";

    bool loopback = im.host == "localhost" || im.host == "::1" || im.host.rfind("127.", 0) == 0;
    if (o.cert.empty() != o.key.empty()) throw std::runtime_error("--cert and --key go together");
    if (loopback && o.cert.empty()) {
        im.srv = std::make_unique<httplib::Server>();
    } else {
        server::TlsPair pair;
        if (o.cert.empty()) {
            if (o.state.empty()) throw std::runtime_error("TLS off loopback needs --cert and --key, or --state for a self-signed pair");
            bool any = im.host == "0.0.0.0" || im.host == "::";
            std::vector<std::string> hosts = any ? server::interface_addresses() : std::vector<std::string>{im.host};
            char name[256] = {};
            if (gethostname(name, sizeof(name) - 1) == 0 && name[0]) hosts.push_back(name);
            pair = server::ensure_self_signed(o.state / "cert.pem", o.state / "key.pem", hosts);
        } else {
            pair = {o.cert, o.key, server::cert_fingerprint(o.cert)};
        }
        auto ssl = std::make_unique<httplib::SSLServer>(pair.cert.c_str(), pair.key.c_str());
        if (!ssl->is_valid()) throw std::runtime_error("can't load the TLS pair " + pair.cert.string() + " / " + pair.key.string());
        im.srv = std::move(ssl);
        tls_ = true;
        fingerprint_ = pair.fingerprint;
    }
    // Every down stream and every long up stream holds a thread for as long as it lives.
    size_t threads = o.max_pairs * 3 + 8;
    im.srv->new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
    im.srv->set_keep_alive_max_count(10000);
    im.srv->set_read_timeout(o.idle_seconds * 2);
    im.srv->set_write_timeout(30);
    im.routes();
    im.port = port == 0 ? im.srv->bind_to_any_port(im.host) : (im.srv->bind_to_port(im.host, port) ? port : -1);
    if (im.port <= 0) throw std::runtime_error("can't listen on " + im.host + ":" + std::to_string(port));
    return im.port;
}

void Relay::run() {
    impl_->srv->listen_after_bind();
}

void Relay::stop() {
    Impl& im = *impl_;
    im.stopping = true;
    std::vector<std::shared_ptr<Pair>> all;
    {
        std::lock_guard lock(im.mu);
        for (const auto& [id, p] : im.pairs) all.push_back(p);
    }
    for (const auto& p : all) {
        std::lock_guard lock(p->mu);
        p->cv.notify_all();
    }
    if (im.srv) im.srv->stop();
}

size_t Relay::pairs() const {
    std::lock_guard lock(impl_->mu);
    size_t n = 0;
    for (const auto& [id, p] : impl_->pairs) {
        std::lock_guard plock(p->mu);
        if (!p->closed) ++n;
    }
    return n;
}

}  // namespace maic::relay
