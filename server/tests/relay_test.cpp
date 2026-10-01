// The relay, the tunnel crypto, the pairing exchange, and the whole path: a maic-server dialling a loopback
// maic-relay, a fake phone that pairs over the LAN and then asks /api/status through the relay.
#include "check.hpp"

#include "auth.hpp"
#include "maic/paths.hpp"
#include "relay.hpp"
#include "server.hpp"
#include "tunnel.hpp"

#include "maic/http.hpp"

#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace maic;
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::string unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

std::string hex(std::string_view s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

server::Key32 key32(const std::string& h) {
    server::Key32 k;
    std::string raw = unhex(h);
    std::copy(raw.begin(), raw.end(), k.begin());
    return k;
}

// One side of a pairing at the relay, as a client: the down stream read on a thread into `frames`, frames
// sent up with post().
struct Side {
    std::string base, path;
    httplib::Client down;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    std::string buffer;
    std::vector<std::string> frames;  // bodies, keepalives left out
    int status = 0;
    bool ended = false;
    bool closing = false;

    Side(const std::string& base, const std::string& id, const char* side) : base(base), path("/r/" + id + "/" + side), down(base) {
        down.set_read_timeout(30);
        thread = std::thread([this] {
            down.Get(
                path, [this](const httplib::Response& r) {
                    std::lock_guard lock(mu);
                    status = r.status;
                    cv.notify_all();
                    return r.status == 200;
                },
                [this](const char* d, size_t n) {
                    std::lock_guard lock(mu);
                    buffer.append(d, n);
                    std::string body;
                    while (server::next_frame(buffer, body)) {
                        if (!body.empty()) frames.push_back(body);
                    }
                    cv.notify_all();
                    return !closing;
                });
            std::lock_guard lock(mu);
            ended = true;
            cv.notify_all();
        });
        std::unique_lock lock(mu);
        cv.wait_for(lock, 5s, [&] { return status != 0 || ended; });
    }
    ~Side() {
        close();
    }
    void close() {
        {
            std::lock_guard lock(mu);
            closing = true;
        }
        down.stop();
        if (thread.joinable()) thread.join();
    }
    int post(const std::string& body) {
        httplib::Client c(base);
        auto res = c.Post(path, body, "application/octet-stream");
        return res ? res->status : 0;
    }
    // Waits for the n-th frame body.
    std::string frame(size_t n, std::chrono::milliseconds wait = 5s) {
        std::unique_lock lock(mu);
        cv.wait_for(lock, wait, [&] { return frames.size() > n || ended; });
        return frames.size() > n ? frames[n] : "";
    }
    bool wait_ended(std::chrono::milliseconds wait = 5s) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, wait, [&] { return ended; });
    }
};

// The phone's end of the tunnel, as the web client implements it, with the C++ primitives.
struct FakePhone {
    server::KeyPair statik = server::make_keypair();
    server::KeyPair eph = server::make_keypair();
    std::unique_ptr<Side> side;
    std::optional<server::Sealer> sealer;
    std::optional<server::Opener> opener;
    size_t next = 0;  // the next relay frame to read

    // Connects, says hello, and derives the session once the home answers; false when the home drops us first.
    bool connect(const std::string& relay, const std::string& id, const server::Key32& home_static) {
        eph = server::make_keypair();
        side = std::make_unique<Side>(relay, id, "phone");
        if (side->status != 200) return false;
        side->post(server::frame(server::hello_body(statik.pk, eph.pk)));
        std::string hello = side->frame(next++);
        server::Key32 hs, he;
        if (!server::parse_hello(hello, hs, he) || hs != home_static) return false;
        server::SessionKeys keys = server::derive_session(statik, eph, hs, he, false);
        sealer.emplace(keys.to_home);
        opener.emplace(keys.to_phone);
        std::string plain;
        if (!opener->open(side->frame(next++), plain)) return false;
        server::Kind kind;
        uint32_t stream;
        std::string_view payload;
        return server::parse_message(plain, kind, stream, payload) && kind == server::Kind::Ready;
    }
    // One request through the tunnel: status, headers and the whole body.
    struct Answer {
        int status = 0;
        json headers;
        std::string body;
        std::string error;
    };
    Answer request(uint32_t stream, const std::string& method, const std::string& path, const json& headers, const std::string& body = "") {
        json head = {{"method", method}, {"path", path}, {"headers", headers}};
        side->post(server::frame(sealer->seal(server::message(server::Kind::Request, stream, head.dump() + "\n" + body))));
        Answer a;
        for (;;) {
            std::string f = side->frame(next++, 10s);
            if (f.empty()) {
                a.error = "no frame";
                return a;
            }
            std::string plain;
            if (!opener->open(f, plain)) {
                a.error = "frame did not open";
                return a;
            }
            server::Kind kind;
            uint32_t s;
            std::string_view payload;
            server::parse_message(plain, kind, s, payload);
            if (s != stream) continue;
            if (kind == server::Kind::Head) {
                json h = json::parse(payload);
                a.status = h["status"];
                a.headers = h["headers"];
            } else if (kind == server::Kind::Data) a.body.append(payload);
            else if (kind == server::Kind::End) return a;
            else if (kind == server::Kind::Error) {
                a.error = std::string(payload);
                return a;
            }
        }
    }
};

json get(const std::string& base, const std::string& path, const std::string& token, int* status = nullptr) {
    httplib::Client c(base);
    auto res = c.Get(path, {{"Authorization", "Bearer " + token}});
    if (status) *status = res ? res->status : 0;
    return res ? json::parse(res->body, nullptr, false) : json();
}

json post(const std::string& base, const std::string& path, const std::string& token, const json& body, int* status = nullptr) {
    httplib::Client c(base);
    auto res = c.Post(path, {{"Authorization", "Bearer " + token}}, body.dump(), "application/json");
    if (status) *status = res ? res->status : 0;
    return res ? json::parse(res->body, nullptr, false) : json();
}

struct RunningRelay {
    relay::Relay relay;
    int port;
    std::thread thread;
    std::string base;
    explicit RunningRelay(relay::RelayOptions o) : relay(std::move(o)), port(relay.bind()), thread([this] { relay.run(); }), base("http://127.0.0.1:" + std::to_string(port)) {}
    ~RunningRelay() {
        relay.stop();
        thread.join();
    }
};

}  // namespace

int main() {
    fs::path root = fs::temp_directory_path() / "maic-relay-test";
    fs::remove_all(root);
    fs::create_directories(root / "state");
    fs::create_directories(root / "ws");
    setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    fs::path state = state_dir() / "server";

    section("the relay forwards frames between two sides and drops both when one leaves");
    {
        std::ostringstream log;
        relay::RelayOptions o;
        o.listen = "127.0.0.1:0";
        o.log = &log;
        o.keepalive_seconds = 1;
        RunningRelay r(o);
        {
            Side home(r.base, "abcdefgh-pairing", "home");
            expect(home.status == 200, "the home side attaches");
            {
                httplib::Client c(r.base);
                auto res = c.Post("/r/abcdefgh-pairing/phone", server::frame("too early"), "application/octet-stream");
                expect(res && res->status == 409, "sending from a side with no down stream is refused");
            }
            Side phone(r.base, "abcdefgh-pairing", "phone");
            expect(phone.status == 200, "the phone side attaches");
            expect(r.relay.pairs() == 1, "one pairing is held");
            Side again(r.base, "abcdefgh-pairing", "phone");
            expect(again.status == 409, "a second phone on the same id is refused");
            expect(home.post(server::frame("first-xq") + server::frame("second-xq")) == 204 && phone.post(server::frame("back-xq")) == 204, "frames go up");
            expect(phone.frame(0) == "first-xq" && phone.frame(1) == "second-xq", "and come down the other side whole and in order: " + phone.frame(0) + ", " + phone.frame(1));
            expect(home.frame(0) == "back-xq", "both ways");
            std::string half = server::frame("split frame across two posts");
            home.post(half.substr(0, 9));
            home.post(half.substr(9));
            expect(phone.frame(2) == "split frame across two posts", "a frame split across posts is reassembled");
            home.post(server::frame(""));
            std::this_thread::sleep_for(1500ms);
            expect(phone.frames.size() == 3, "keepalives are not forwarded as frames, and the relay's own keepalives are skipped by the reader");
            expect(phone.buffer.empty(), "nothing is left half-read: " + hex(phone.buffer));
            phone.close();
            expect(home.wait_ended(), "when the phone leaves the home's stream ends too");
            expect(home.post(server::frame("late")) != 204, "and nothing more is accepted for it");
        }
        std::this_thread::sleep_for(100ms);
        expect(r.relay.pairs() == 0, "the pairing is forgotten");
        std::string l = log.str();
        expect(l.find("attach abcdefgh-pairing home") != std::string::npos && l.find("close abcdefgh-pairing phone") != std::string::npos, "the log has the id, the sides and the reason");
        expect(l.find("-xq") == std::string::npos && l.find("split frame") == std::string::npos, "and no content");
        httplib::Client c(r.base);
        auto res = c.Get("/");
        expect(res && res->status == 404, "no web client without --web");
        auto bad = c.Get("/r/short/home");
        expect(bad && bad->status == 404, "a pairing id must be 8 to 64 url-safe characters");
    }

    section("expiry, the pair cap and the throughput cap");
    {
        relay::RelayOptions o;
        o.listen = "127.0.0.1:0";
        o.idle_seconds = 1;
        o.keepalive_seconds = 1;
        o.max_pairs = 1;
        RunningRelay r(o);
        Side home(r.base, "idle-pairing-id", "home");
        expect(home.status == 200, "attached");
        Side other(r.base, "another-pairing", "home");
        expect(other.status == 503, "a second pairing id is refused at the cap");
        auto t0 = Clock::now();
        expect(home.wait_ended(4s), "a side that sends nothing is dropped after the idle time");
        auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
        expect(took >= 900ms && took < 3500ms, "about one second in: " + std::to_string(took.count()) + "ms");
        relay::RelayOptions slow = o;
        slow.max_pairs = 4;
        slow.idle_seconds = 30;
        slow.rate = 2000;
        RunningRelay s(slow);
        Side h(s.base, "throttled-pairing", "home");
        Side p(s.base, "throttled-pairing", "phone");
        std::string frames;
        for (int i = 0; i < 6; ++i) frames += server::frame(std::string(996, 'a' + i));
        t0 = Clock::now();
        h.post(frames);
        for (int i = 0; i < 6; ++i) expect(p.frame(i, 10s).size() == 996, "frame " + std::to_string(i) + " arrives intact");
        took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
        expect(took >= 1900ms, "6000 bytes at 2000 B/s take about 2 s after the first second's allowance: " + std::to_string(took.count()) + "ms");
    }

    section("tunnel crypto: known answers");
    {
        std::string okm = server::hkdf_sha256(unhex("000102030405060708090a0b0c"), std::string(22, '\x0b'), unhex("f0f1f2f3f4f5f6f7f8f9"), 42);
        expect(hex(okm) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "HKDF-SHA256, RFC 5869 test case 1");
        std::string okm3 = server::hkdf_sha256("", std::string(22, '\x0b'), "", 42);
        expect(hex(okm3) == "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", "RFC 5869 test case 3 (empty salt and info)");
        server::KeyPair phone_static{key32("b924d2559310192ebe65016cfeec9a2aaeab67def816183ca3beeeea6c0c754b"), key32("8bf52fdf679746bed5c360faabade18348c82763802bf96e6e0c4559470b1ec7")};
        server::KeyPair phone_eph{key32("be68742ca3ca80acd3814309b25c7c41b9b58b96d87a66bcd8454d9e8b8da458"), key32("0972a69e84b538b4d37aeecd9e9cb23d381cebd428a017baa75bca4b372122b7")};
        server::KeyPair home_static{key32("22d18dedc370ccc8a0ddd45e6da7b406fa7287fd073c85a009e5caf192131e32"), key32("8dccfdfbee38c4768e674cbc51a32585dc36af04f159011dca2b77a8324ebf90")};
        server::KeyPair home_eph{key32("b2f66c22a9bf77d235fb089a021c194840fd2008a60f47685251b2cb9630840d"), key32("cb34de36b80c258aa0d14d81ca269ff37e7f9face716799a46feb028474a458a")};
        server::SessionKeys at_phone = server::derive_session(phone_static, phone_eph, home_static.pk, home_eph.pk, false);
        server::SessionKeys at_home = server::derive_session(home_static, home_eph, phone_static.pk, phone_eph.pk, true);
        expect(at_phone.to_home == at_home.to_home && at_phone.to_phone == at_home.to_phone, "both ends derive the same two keys");
        expect(hex(std::string(reinterpret_cast<const char*>(at_phone.to_home.data()), 32)) == "4e9588819afb1555ae99e31bfb04279d0bff0bea31178470fc72ef93f0805c17" &&
                   hex(std::string(reinterpret_cast<const char*>(at_phone.to_phone.data()), 32)) == "4bb80b6df72d6a086ed7769fee3105cae2e39298f563bb7c44b52211edca1eae",
               "and they are the vector the web client's test checks (X25519, RFC 7748 arithmetic; HKDF over the three shared secrets)");
        server::Sealer sealer(at_phone.to_home);
        std::string m0 = server::message(server::Kind::Request, 7, "{\"method\":\"GET\",\"path\":\"/api/status\",\"headers\":{}}\n");
        std::string body0 = sealer.seal(m0), body1 = sealer.seal(server::message(server::Kind::End, 7, ""));
        expect(hex(body0) == "00000000000000000000000000000000000000000000000089af5f3f097bb2e3dfb40f1a891728bb9bcb3f41e849cff2727a9d7398324b3c1da03816178d50b37403149bd5173e66ba556b1dde27654226d4cd633e2d5d86f229e71358d2cefe",
               "XChaCha20-Poly1305 frame 0 matches the vector (nonce = counter 0)");
        expect(hex(body1) == "0000000000000001000000000000000000000000000000001239476723a0194ad68846b9cfc3fced83e59588f2", "frame 1 carries counter 1");
        server::Opener opener(at_home.to_home);
        std::string plain;
        expect(!opener.open(body1, plain), "a frame ahead of the counter is refused");
        expect(opener.open(body0, plain) && plain == m0, "the next frame opens to the message");
        expect(!opener.open(body0, plain), "the same frame again (a replay) is refused");
        std::string tampered = body1;
        tampered[30] ^= 1;
        expect(!opener.open(tampered, plain), "a flipped ciphertext byte is refused");
        expect(opener.open(body1, plain), "then the untouched frame opens");
        server::Kind kind;
        uint32_t stream;
        std::string_view payload;
        expect(server::parse_message(plain, kind, stream, payload) && kind == server::Kind::End && stream == 7 && payload.empty(), "and parses");
        server::Opener wrong(at_home.to_phone);
        expect(!wrong.open(body0, plain), "the other direction's key does not open it");
        server::KeyPair other = server::make_keypair();
        server::SessionKeys mismatched = server::derive_session(other, home_eph, phone_static.pk, phone_eph.pk, true);
        expect(mismatched.to_home != at_home.to_home, "a different static key gives different session keys");
        std::string buf = server::frame(body0) + server::frame("") + server::frame(body1).substr(0, 10);
        std::string b;
        expect(server::next_frame(buf, b) && b == body0 && server::next_frame(buf, b) && b.empty() && !server::next_frame(buf, b) && buf.size() == 10, "frames split at their lengths");
        bool threw = false;
        try {
            std::string huge = "\x7f\xff\xff\xff";
            server::next_frame(huge, b);
        } catch (const std::exception&) {
            threw = true;
        }
        expect(threw, "a frame longer than the limit is refused");
        auto k = server::key_from_b64url(server::b64url(phone_static.pk));
        expect(k && *k == phone_static.pk && !server::key_from_b64url("short") && !server::key_from_b64url("not base64!!"), "keys round-trip through base64url");
    }

    section("pairing: the store and the offer");
    {
        server::PairStore store(state / "pairs.json");
        std::string id = store.pairing_id();
        expect(id.size() == 22 && fs::exists(state / "pairs.json"), "an identity is made on first use: " + id);
        struct stat st {};
        expect(stat((state / "pairs.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "pairs.json is 0600");
        server::KeyPair phone = server::make_keypair();
        store.add("phone", phone.pk);
        store.add("tablet", server::make_keypair().pk);
        expect(store.list().size() == 2 && store.find(phone.pk) && store.find(phone.pk)->name == "phone", "phones are listed and found by key");
        server::PairStore again(state / "pairs.json");
        expect(again.pairing_id() == id && again.find(phone.pk) && again.keypair().pk == store.keypair().pk, "a fresh store reads the same identity and phones");
        expect(store.remove("tablet") && !store.remove("tablet") && store.list().size() == 1, "unpair removes one");
        again.refresh();
        expect(again.list().size() == 1, "another process sees the change on refresh");

        fs::path offer = state / "pairing.json";
        std::string code = server::new_pairing_code();
        expect(code.size() == 8 && code.find_first_not_of("0123456789") == std::string::npos, "a code is 8 digits: " + code);
        expect(server::claim_pairing_offer(offer, code).has_value(), "no offer: refused");
        server::write_pairing_offer(offer, code);
        std::string wrong = code[0] == '0' ? "1" + code.substr(1) : "0" + code.substr(1);
        expect(server::claim_pairing_offer(offer, wrong).value_or("") .find("wrong code") == 0, "a wrong code is refused");
        expect(!server::claim_pairing_offer(offer, code) && !fs::exists(offer), "the right code claims the offer and consumes it");
        server::write_pairing_offer(offer, code);
        for (int i = 0; i < 3; ++i) server::claim_pairing_offer(offer, wrong);
        expect(!fs::exists(offer) && server::claim_pairing_offer(offer, code).has_value(), "three wrong codes void the offer");
        server::write_pairing_offer(offer, code, -1);
        expect(server::claim_pairing_offer(offer, code).value_or("").find("expired") != std::string::npos, "an expired offer is refused");
    }

    section("end to end: a server dialling a loopback relay, a phone pairing over the LAN then asking through the relay");
    {
        std::ostringstream relay_log;
        relay::RelayOptions ro;
        ro.listen = "127.0.0.1:0";
        ro.log = &relay_log;
        ro.keepalive_seconds = 1;
        RunningRelay r(ro);

        std::string token;
        {
            server::TokenStore tokens(state / "tokens.json");
            token = tokens.create("phone");
        }
        server::ServerOptions o;
        o.listen = "127.0.0.1:0";
        o.settings.model = "test";
        o.settings.mode = "manual";
        o.settings.providers = {Provider{"fake", "openai", "http://127.0.0.1:1/v1"}};
        o.settings.server.relay = r.base;
        o.workspaces = {root / "ws"};
        o.state = state;
        o.web = fs::path(__FILE__).parent_path().parent_path() / "web" / "index.html";
        server::Server srv(o);
        int port = srv.bind();
        std::thread serving([&] { srv.run(); });
        std::string lan = "http://127.0.0.1:" + std::to_string(port);

        json st;
        for (int i = 0; i < 100 && !(st.is_object() && st["relay"]["connected"] == true); ++i) {
            std::this_thread::sleep_for(50ms);
            st = get(lan, "/api/status", token);
        }
        expect(st["relay"]["connected"] == true && st["relay"]["url"] == r.base, "the server holds its home connection to the relay open");
        std::string since = st["relay"]["since"];
        expect(r.relay.pairs() == 1, "the relay holds the pairing with only the home side");
        {
            std::ifstream in(state / "relay.json");
            json rj = json::parse(in, nullptr, false);
            expect(rj.is_object() && rj["connected"] == true, "relay.json says so for `maic server status`");
        }

        // The LAN exchange, as the web client's pairing screen does it.
        FakePhone phone;
        std::string code = server::new_pairing_code();
        int status = 0;
        json pair = post(lan, "/api/pair", token, {{"code", code}, {"name", "phone"}, {"public_key", server::b64url(phone.statik.pk)}}, &status);
        expect(status == 403 && pair["error"].get<std::string>().find("no pairing is offered") != std::string::npos, "pairing without an offer is refused: " + pair.dump());
        server::write_pairing_offer(state / "pairing.json", code);
        std::string wrong = code[0] == '0' ? "1" + code.substr(1) : "0" + code.substr(1);
        pair = post(lan, "/api/pair", token, {{"code", wrong}, {"name", "phone"}, {"public_key", server::b64url(phone.statik.pk)}}, &status);
        expect(status == 403 && pair["error"].get<std::string>().find("wrong code") == 0, "a wrong code is refused");
        post(lan, "/api/pair", token, {{"code", code}, {"name", "phone"}, {"public_key", "bad"}}, &status);
        expect(status == 400, "a public key that is not 32 bytes is a 400");
        post(lan, "/api/pair", "", {{"code", code}, {"name", "phone"}, {"public_key", server::b64url(phone.statik.pk)}}, &status);
        expect(status == 401, "the exchange needs the bearer token like everything else");
        pair = post(lan, "/api/pair", token, {{"code", code}, {"name", "phone"}, {"public_key", server::b64url(phone.statik.pk)}}, &status);
        expect(status == 201 && pair["relay"] == r.base && pair["name"] == "phone", "the right code pairs: " + pair.dump());
        std::string pairing_id = pair["pairing_id"];
        auto home_pk = server::key_from_b64url(pair["public_key"].get<std::string>());
        expect(home_pk.has_value() && pairing_id.size() == 22, "the phone gets the pairing id and the workstation's key");
        {
            server::PairStore store(state / "pairs.json");
            expect(store.list().size() == 1 && store.list()[0].name == "phone" && store.find(phone.statik.pk).has_value(), "pairs.json has the phone");
        }

        // Then away from home: through the relay.
        expect(phone.connect(r.base, pairing_id, *home_pk), "the phone connects through the relay and the handshake completes");
        FakePhone::Answer a = phone.request(1, "GET", "/api/status", {{"Authorization", "Bearer " + token}});
        expect(a.status == 200 && a.error.empty(), "GET /api/status through the tunnel: HTTP " + std::to_string(a.status) + " " + a.error);
        json via = json::parse(a.body, nullptr, false);
        expect(via.is_object() && via["model"] == "test" && via["harness"].contains("tripped") && via["relay"]["connected"] == true, "and it is the real answer: " + a.body.substr(0, 80));
        expect(a.headers.value("Content-Type", "").find("application/json") == 0, "with the response headers");
        a = phone.request(2, "GET", "/api/status", json::object());
        expect(a.status == 401, "no token inside the tunnel is still a 401: the relay adds no authority");
        a = phone.request(3, "POST", "/api/sessions", {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}}, json{{"mode", "bogus"}}.dump());
        expect(a.status == 400, "a POST body travels too (an unknown mode is the 400 it is on the LAN)");
        a = phone.request(4, "GET", "/", json::object());
        expect(a.status == 200 && a.body.find("tunnel crypto begin") != std::string::npos, "the web client itself comes through");
        {
            std::ifstream in(state / "audit.log");
            std::string audit((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            expect(audit.find("relay/phone phone GET /api/status 200") != std::string::npos, "the audit line names the phone behind the relay");
        }
        std::string l = relay_log.str();
        expect(l.find(pairing_id) != std::string::npos && l.find(token) == std::string::npos && l.find("Bearer") == std::string::npos && l.find("api/status") == std::string::npos,
               "the relay's log has the pairing id and no token, no path, no content");

        section("no route anywhere unlocks: through the tunnel, at the relay");
        a = phone.request(5, "POST", "/api/unlock", {{"Authorization", "Bearer " + token}});
        expect(a.status == 404, "POST /api/unlock through the tunnel is a 404");
        a = phone.request(6, "POST", "/api/tripwire/reset", {{"Authorization", "Bearer " + token}});
        expect(a.status == 404, "and so is any other reset route");
        httplib::Client rc(r.base);
        auto rr = rc.Post("/api/unlock", "", "application/json");
        expect(rr && rr->status == 404, "the relay has no /api at all");
        rr = rc.Post("/r/" + pairing_id + "/unlock", "", "application/json");
        expect(rr && rr->status == 404, "nor any third side of a pairing");
        rr = rc.Get("/api/status");
        expect(rr && rr->status == 404, "nor status: the relay knows nothing to tell");

        section("the phone leaves: both sides drop, the home comes back; an unpaired phone is refused");
        phone.side->close();
        json st2;
        for (int i = 0; i < 100 && !(st2.is_object() && st2["relay"]["connected"] == true && st2["relay"]["since"] != since); ++i) {
            std::this_thread::sleep_for(50ms);
            st2 = get(lan, "/api/status", token);
        }
        expect(st2["relay"]["connected"] == true && st2["relay"]["since"] != since, "the home reconnected after the phone left");
        FakePhone stranger;
        expect(!stranger.connect(r.base, pairing_id, *home_pk), "a phone that never paired gets no hello back");
        {
            server::PairStore store(state / "pairs.json");
            store.remove("phone");
        }
        for (int i = 0; i < 100 && !(get(lan, "/api/status", token)["relay"]["connected"] == true); ++i) std::this_thread::sleep_for(50ms);
        expect(!phone.connect(r.base, pairing_id, *home_pk), "after `maic server unpair` the phone is refused on its next connection");
        for (int i = 0; i < 100 && !(get(lan, "/api/status", token)["relay"]["connected"] == true); ++i) std::this_thread::sleep_for(50ms);
        st = get(lan, "/api/status", token);
        expect(st["relay"]["connected"] == true, "and the home is back up for the next one");

        srv.stop();
        serving.join();
    }

    fs::remove_all(root);
    return finish();
}
