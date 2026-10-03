#pragma once

#include "maic/settings.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace maic::server {

struct ServerOptions {
    std::string listen = "127.0.0.1:7373";  // ADDR:PORT; port 0 picks a free one (tests)
    Settings settings;                      // model, mode, providers, compaction: the defaults for new sessions
    std::vector<std::filesystem::path> workspaces;  // roots a remote session may open; the first is the default workspace
    std::filesystem::path state;  // tokens.json, audit.log and the generated TLS pair live here
    std::filesystem::path web;    // the single-file client served at /
    std::filesystem::path artifacts;  // <state>/artifacts: one folder per artifact, served at /a/<id>/
    std::filesystem::path vue;        // vendor/vue, served at /a/_vendor/vue/
};

// The HTTP face of MAIC: a client of the core like the CLI, never a bypass. Every tool call it causes is
// Origin::Remote, so the harness asks whatever the mode. It cannot reset the tripwire: there is no route for it.
class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();

    // Binds the listen address; off loopback this needs TLS (settings.server.cert/key, or a self-signed pair
    // made under `state`). Returns the port. Throws when the address is taken.
    int bind();
    void run();  // serves until stop()
    void stop();

    bool tls() const { return tls_; }
    const std::string& fingerprint() const { return fingerprint_; }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool tls_ = false;
    std::string fingerprint_;
};

// `maic server start|token|status ...`, also main() of maic-server. Returns the exit code.
int run_server_command(const std::vector<std::string>& args);

// `maic artifact list|add|open|allow-insecure ...` (docs/artifacts.md). Returns the exit code.
int run_artifact_command(const std::vector<std::string>& args, bool text_base = false);

}  // namespace maic::server
