#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

namespace maic::relay {

struct RelayOptions {
    std::string listen = "127.0.0.1:7474";  // ADDR:PORT; port 0 picks a free one (tests)
    std::filesystem::path state;            // the self-signed pair goes here when TLS is needed and no pair is given
    std::filesystem::path cert;             // PEM pair; empty = self-signed under `state`
    std::filesystem::path key;
    std::filesystem::path web;              // the web client served at /; empty = nothing at /
    int idle_seconds = 60;                  // a side that sent nothing (not even a keepalive) this long is dropped, and the pair with it
    int keepalive_seconds = 20;             // a zero-length frame goes down each stream after this much silence
    size_t max_pairs = 64;                  // pairing ids held at once; more get 503
    size_t rate = 4u << 20;                 // bytes per second per pairing id, both directions together; more is delayed
    size_t max_frame = 1u << 20;            // a frame longer than this ends the pair
    std::ostream* log = nullptr;            // one line per attach, detach and refusal: pairing id, side, byte counts, times; never content
};

// The rendezvous point between a workstation and its phone. Two sides, `home` and `phone`, each hold one
// down stream open (GET /r/ID/SIDE, chunked, frames flow out of it) and send frames up (POST /r/ID/SIDE, a
// long chunked request or one request per batch). The relay joins the two by the pairing id and copies
// whole frames across. It reads the 4-byte length prefix and nothing else: the frames are encrypted end to
// end between the phone and the workstation, so what passes here is sizes and timing. When either side's
// down stream ends, the other's ends too. There is no route that reaches a tripwire anywhere.
class Relay {
public:
    explicit Relay(RelayOptions options);
    ~Relay();

    // Binds the listen address; off loopback TLS is required (cert/key, or a self-signed pair under state).
    // Returns the port.
    int bind();
    void run();  // serves until stop()
    void stop();

    bool tls() const { return tls_; }
    const std::string& fingerprint() const { return fingerprint_; }
    size_t pairs() const;  // pairing ids with at least one side attached

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool tls_ = false;
    std::string fingerprint_;
};

}  // namespace maic::relay
