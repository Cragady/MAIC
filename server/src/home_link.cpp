#include "home_link.hpp"

#include "tunnel.hpp"

#include "maic/http.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

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

// One connection to the relay: the cipher state once a phone has said hello, the frames waiting to go up,
// and the requests in flight.
struct Link {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> outbox;
    bool closing = false;
    std::string inbuf;
    std::optional<Sealer> sealer;
    std::optional<Opener> opener;
    std::string phone;        // the paired phone's name once it has said hello
    std::string fail_reason;  // why this connection was ended from here, for the status line

    struct Stream {
        std::atomic<bool> cancel{false};
        httplib::Client* client = nullptr;  // to cut a request short from another thread
    };
    std::map<uint32_t, std::shared_ptr<Stream>> streams;
    int active = 0;  // request threads not yet finished

    void send_raw(std::string frame_bytes) {
        std::lock_guard lock(mu);
        outbox.push_back(std::move(frame_bytes));
        cv.notify_all();
    }
    // Sealing and queueing happen under one lock so the counters leave in order.
    void send(Kind kind, uint32_t stream, std::string_view payload) {
        std::lock_guard lock(mu);
        if (closing || !sealer) return;
        outbox.push_back(frame(sealer->seal(message(kind, stream, payload))));
        cv.notify_all();
    }
    void close() {
        std::lock_guard lock(mu);
        closing = true;
        for (auto& [id, s] : streams) {
            s->cancel = true;
            if (s->client) s->client->stop();
        }
        cv.notify_all();
    }
};

}  // namespace

struct HomeLink::Impl {
    explicit Impl(HomeLinkOptions o) : options(std::move(o)), pairs(options.pairs_file) {}

    HomeLinkOptions options;
    PairStore pairs;
    std::mutex pairs_mu;
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::mutex wake_mu;
    std::condition_variable wake;
    mutable std::mutex state_mu;
    State st;
    std::mutex conn_mu;  // the connection stop() has to cut short
    Link* current_link = nullptr;
    httplib::Client* current_down = nullptr;

    void set_state(bool connected, const std::string& error) {
        std::lock_guard lock(state_mu);
        std::string now = utc_now();
        if (connected && !st.connected) st.since = now;
        if (!connected && st.connected) st.last_connected = now;
        st.connected = connected;
        st.error = error;
        json j = {{"relay", options.relay}, {"connected", st.connected}, {"since", st.since}, {"last_connected", st.last_connected}, {"error", st.error}};
        std::error_code ec;
        fs::create_directories(options.status_file.parent_path(), ec);
        std::ofstream out(options.status_file.string() + ".tmp");
        out << j.dump() << "\n";
        out.close();
        fs::rename(options.status_file.string() + ".tmp", options.status_file, ec);
    }

    std::unique_ptr<httplib::Client> client() {
        auto c = std::make_unique<httplib::Client>(options.relay);
        if (!options.relay_cert.empty()) c->set_ca_cert_path(options.relay_cert.string());
        c->set_connection_timeout(10);
        c->set_read_timeout(60);  // the relay sends a keepalive every 20 s
        c->set_write_timeout(30);
        return c;
    }

    std::string path() {
        std::lock_guard lock(pairs_mu);
        return "/r/" + pairs.pairing_id() + "/home";
    }

    // The phone's hello: look it up, make our ephemeral key, derive the session, answer with our hello and
    // the first sealed message. False for a phone this workstation has not paired with.
    bool handshake(const std::shared_ptr<Link>& link, const std::string& body) {
        Key32 phone_static, phone_eph;
        if (!parse_hello(body, phone_static, phone_eph)) return false;
        std::optional<PhoneInfo> phone;
        KeyPair ours;
        {
            std::lock_guard lock(pairs_mu);
            pairs.refresh();
            phone = pairs.find(phone_static);
            ours = pairs.keypair();
        }
        if (!phone) return false;
        KeyPair eph = make_keypair();
        SessionKeys keys = derive_session(ours, eph, phone_static, phone_eph, true);
        {
            std::lock_guard lock(link->mu);
            link->phone = phone->name;
            link->sealer.emplace(keys.to_phone);
            link->opener.emplace(keys.to_home);
            link->outbox.push_back(frame(hello_body(ours.pk, eph.pk)));
            link->outbox.push_back(frame(link->sealer->seal(message(Kind::Ready, 0, ""))));
            link->cv.notify_all();
        }
        return true;
    }

    // One request out of the tunnel, replayed against this server's own listener; the answer goes back as
    // Head, Data frames and End.
    void serve(const std::shared_ptr<Link>& link, uint32_t id, std::string payload, const std::shared_ptr<Link::Stream>& stream) {
        size_t nl = payload.find('\n');
        json head = json::parse(payload.substr(0, nl), nullptr, false);
        if (!head.is_object()) {
            link->send(Kind::Error, id, "bad request head");
            return;
        }
        httplib::Client c(options.loopback);
        c.enable_server_certificate_verification(false);  // our own listener, possibly with our own self-signed certificate
        c.set_connection_timeout(5);
        c.set_read_timeout(300);
        {
            std::lock_guard lock(link->mu);
            stream->client = &c;
        }
        httplib::Request req;
        req.method = head.value("method", "GET");
        req.path = head.value("path", "/");
        json head_headers = head.value("headers", json::object());  // a named copy: iterating a temporary dangles
        for (const auto& [k, v] : head_headers.items()) {
            std::string lower = k;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower == "host" || lower == "content-length" || lower == "connection" || lower == "transfer-encoding") continue;
            if (v.is_string()) req.set_header(k, v.get<std::string>());
        }
        req.set_header("X-Maic-Via", "relay/" + link->phone);
        if (nl != std::string::npos) req.body = payload.substr(nl + 1);
        req.response_handler = [&](const httplib::Response& r) {
            json headers = json::object();
            for (const auto& [k, v] : r.headers) {
                std::string lower = k;
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lower == "content-length" || lower == "transfer-encoding" || lower == "connection" || lower == "keep-alive") continue;
                headers[k] = v;
            }
            link->send(Kind::Head, id, json{{"status", r.status}, {"headers", headers}}.dump());
            return !stream->cancel.load();
        };
        req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
            link->send(Kind::Data, id, std::string_view(data, len));
            return !stream->cancel.load();
        };
        httplib::Response res;
        httplib::Error err = httplib::Error::Success;
        bool ok = c.send(req, res, err);
        {
            std::lock_guard lock(link->mu);
            stream->client = nullptr;
            link->streams.erase(id);
        }
        if (stream->cancel.load()) return;
        if (!ok) link->send(Kind::Error, id, "the server did not answer: " + httplib::to_string(err));
        else link->send(Kind::End, id, "");
    }

    // Bytes from the relay's down stream. False ends the connection: an unknown phone, a frame that does
    // not open (a replay, a reorder, a wrong key), or garbage.
    bool on_bytes(const std::shared_ptr<Link>& link, const char* data, size_t len) {
        link->inbuf.append(data, len);
        std::string body;
        while (next_frame(link->inbuf, body)) {
            if (body.empty()) continue;  // keepalive
            if (!link->opener) {
                if (!handshake(link, body)) {
                    link->fail_reason = "a phone that is not paired with this workstation tried to connect";
                    return false;
                }
                continue;
            }
            std::string plain;
            if (!link->opener->open(body, plain)) {
                link->fail_reason = "a frame from the phone did not open (replayed, reordered or wrongly keyed)";
                return false;
            }
            Kind kind;
            uint32_t id;
            std::string_view payload;
            if (!parse_message(plain, kind, id, payload)) return false;
            if (kind == Kind::Request) {
                auto stream = std::make_shared<Link::Stream>();
                {
                    std::lock_guard lock(link->mu);
                    if (link->closing) return false;
                    link->streams[id] = stream;
                    ++link->active;
                }
                std::thread([this, link, id, p = std::string(payload), stream] {
                    serve(link, id, std::move(p), stream);
                    std::lock_guard lock(link->mu);
                    --link->active;
                    link->cv.notify_all();
                }).detach();
            } else if (kind == Kind::End) {
                std::lock_guard lock(link->mu);
                auto it = link->streams.find(id);
                if (it != link->streams.end()) {
                    it->second->cancel = true;
                    if (it->second->client) it->second->client->stop();
                }
            }
        }
        return true;
    }

    // One connection: the down stream attaches, the up stream runs until the connection is over. Returns
    // whether it attached at all, which decides the backoff.
    bool attempt() {
        auto link = std::make_shared<Link>();
        auto down = client(), up = client();
        std::string p = path();
        httplib::Headers headers = {{"Cache-Control", "no-store"}};
        std::mutex m;
        std::condition_variable c;
        bool attached = false, reader_done = false;
        {
            std::lock_guard lock(conn_mu);
            current_link = link.get();
            current_down = down.get();
        }
        std::thread reader([&] {
            auto res = down->Get(
                p, headers,
                [&](const httplib::Response& r) {
                    if (r.status != 200) {
                        set_state(false, "the relay answered HTTP " + std::to_string(r.status) + " to " + p);
                        return false;
                    }
                    set_state(true, "");
                    std::lock_guard lock(m);
                    attached = true;
                    c.notify_all();
                    return true;
                },
                [&](const char* data, size_t len) { return on_bytes(link, data, len) && !stopping.load(); });
            if (!attached && res.error() != httplib::Error::Success) set_state(false, "can't reach the relay: " + httplib::to_string(res.error()));
            link->close();
            std::lock_guard lock(m);
            reader_done = true;
            c.notify_all();
        });
        {
            std::unique_lock lock(m);
            c.wait(lock, [&] { return attached || reader_done; });
        }
        if (attached) {
            up->Post(
                p, headers,
                [&](size_t, httplib::DataSink& sink) {
                    std::string out;
                    {
                        std::unique_lock lock(link->mu);
                        link->cv.wait_for(lock, 20s, [&] { return !link->outbox.empty() || link->closing || stopping.load(); });
                        if (link->closing || stopping.load()) {
                            sink.done();
                            return true;
                        }
                        for (const auto& f : link->outbox) out += f;
                        link->outbox.clear();
                    }
                    if (out.empty()) out = frame("");  // keepalive for the relay's read timeout
                    return sink.write(out.data(), out.size());
                },
                "application/octet-stream");
        }
        link->close();
        down->stop();
        reader.join();
        up->stop();
        {
            std::lock_guard lock(conn_mu);
            current_link = nullptr;
            current_down = nullptr;
        }
        {
            std::unique_lock lock(link->mu);
            link->cv.wait_for(lock, 5s, [&] { return link->active == 0; });
        }
        if (attached) {
            bool was;
            {
                std::lock_guard lock(state_mu);
                was = st.connected;
            }
            if (was) set_state(false, stopping ? "" : (link->fail_reason.empty() ? "the connection through the relay ended" : link->fail_reason));
        }
        return attached;
    }

    void run() {
        int backoff = 1;
        while (!stopping) {
            bool attached = attempt();
            backoff = attached ? 1 : std::min(backoff * 2, 60);
            std::unique_lock lock(wake_mu);
            wake.wait_for(lock, std::chrono::seconds(attached ? 1 : backoff), [&] { return stopping.load(); });
        }
    }
};

HomeLink::HomeLink(HomeLinkOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

HomeLink::~HomeLink() {
    stop();
}

void HomeLink::start() {
    impl_->set_state(false, "");
    impl_->thread = std::thread([this] { impl_->run(); });
}

void HomeLink::stop() {
    Impl& im = *impl_;
    im.stopping = true;
    {
        std::lock_guard lock(im.wake_mu);
        im.wake.notify_all();
    }
    {
        std::lock_guard lock(im.conn_mu);
        if (im.current_link) im.current_link->close();
        if (im.current_down) im.current_down->stop();
    }
    if (im.thread.joinable()) im.thread.join();
}

HomeLink::State HomeLink::state() const {
    std::lock_guard lock(impl_->state_mu);
    return impl_->st;
}

}  // namespace maic::server
