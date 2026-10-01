#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace maic::server {

struct HomeLinkOptions {
    std::string relay;                      // https://host:port of the maic-relay
    std::filesystem::path relay_cert;       // PEM to pin the relay's certificate; empty = the system CA store
    std::filesystem::path pairs_file;       // <state>/server/pairs.json
    std::filesystem::path status_file;      // <state>/server/relay.json, read by `maic server status`
    std::string loopback;                   // this server's own address for the requests that come out of the tunnel
};

// The workstation's end of the relay: an outbound connection to `relay` held open and reopened with
// backoff, the handshake with a paired phone, and each request that comes out of the tunnel replayed
// against this server's own listener so the token check, the audit line and every route stay exactly what
// they are on the LAN. Nothing inbound ever reaches the workstation; it only ever dials out.
class HomeLink {
public:
    explicit HomeLink(HomeLinkOptions options);
    ~HomeLink();

    void start();
    void stop();

    struct State {
        bool connected = false;
        std::string since;           // when the current connection attached
        std::string last_connected;  // when a connection last was, for the status line
        std::string error;           // the last failure, for the status line
    };
    State state() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace maic::server
