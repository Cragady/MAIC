// `maic server ...`: start, token new|list|revoke, status.
#include "auth.hpp"
#include "server.hpp"

#include "maic/harness.hpp"
#include "maic/paths.hpp"
#include "maic/settings.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace maic::server {

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop = true;
}

void usage(std::ostream& out) {
    out << "usage: maic server start [--listen ADDR:PORT] [--model M] [--mode MODE]   serve the API and the web client\n"
           "       maic server token new NAME     a bearer token for one device, printed once\n"
           "       maic server token list\n"
           "       maic server token revoke NAME\n"
           "       maic server status             the configuration, and whether a server answers\n"
           "\n"
           "Loopback is the default. Any other address needs TLS: a self-signed certificate is made on first use\n"
           "(pin its fingerprint on the phone), or set server.cert and server.key in settings. docs/remote.md\n";
}

std::vector<fs::path> workspace_roots(const Settings& settings) {
    if (!settings.server.workspaces.empty()) return settings.server.workspaces;
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path dev2 = fs::path(home) / "dev2";
        if (fs::is_directory(dev2)) return {dev2};
    }
    return {fs::current_path()};
}

int cmd_token(const std::vector<std::string>& args, const fs::path& state) {
    TokenStore store(state / "tokens.json");
    if (args.size() == 2 && args[0] == "new") {
        std::string token = store.create(args[1]);
        std::cout << "token for " << args[1] << " (shown once; only its hash is kept in " << (state / "tokens.json").string() << "):\n\n    " << token << "\n\n"
                  << "On the device: open the server's address, paste it in the token box.\n";
        return 0;
    }
    if (args.size() == 1 && args[0] == "list") {
        auto all = store.list();
        if (all.empty()) std::cout << "no tokens yet: maic server token new NAME\n";
        for (const auto& t : all) std::cout << t.name << "  created " << t.created << "\n";
        return 0;
    }
    if (args.size() == 2 && args[0] == "revoke") {
        if (!store.revoke(args[1])) {
            std::cerr << "maic server: no token named " << args[1] << "\n";
            return 1;
        }
        std::cout << "revoked " << args[1] << "; the server picks that up at its next request\n";
        return 0;
    }
    usage(std::cerr);
    return 2;
}

int cmd_status(const Settings& settings, const fs::path& state) {
    std::string listen = settings.server.listen;
    size_t colon = listen.rfind(':');
    std::string host = colon == std::string::npos ? listen : listen.substr(0, colon);
    std::string port = colon == std::string::npos ? "7373" : listen.substr(colon + 1);
    bool loopback = host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
    TokenStore store(state / "tokens.json");
    std::cout << "listen:     " << listen << (loopback ? "  (loopback, plain HTTP)" : "  (TLS)") << "\n";
    if (!loopback) {
        fs::path cert = settings.server.cert.empty() ? state / "cert.pem" : settings.server.cert;
        std::cout << "cert:       " << cert.string() << (fs::exists(cert) ? "" : "  (made at first start)") << "\n";
    }
    std::cout << "workspaces:";
    for (const auto& w : workspace_roots(settings)) std::cout << " " << w.string();
    std::cout << "\ntokens:     " << store.list().size() << " (maic server token list)\n"
              << "audit log:  " << (state / "audit.log").string() << "\n";
    // Any answer, even the 401 an unauthenticated probe gets, means a server is up.
    std::string probe_host = host == "0.0.0.0" || host == "::" || host.empty() ? "127.0.0.1" : host;
    httplib::Client client((loopback ? "http://" : "https://") + probe_host + ":" + port);
    client.set_connection_timeout(2);
    client.enable_server_certificate_verification(false);
    auto res = client.Get("/api/status");
    std::cout << "server:     " << (res ? "running (HTTP " + std::to_string(res->status) + " to a probe without a token)" : "not running") << "\n";
    return 0;
}

int cmd_start(const std::vector<std::string>& args) {
    Settings settings = load_settings();
    std::string listen = settings.server.listen;
    for (size_t i = 0; i < args.size(); ++i) {
        auto value = [&](const char* flag) {
            if (i + 1 >= args.size()) throw std::runtime_error(std::string(flag) + " needs a value");
            return args[++i];
        };
        if (args[i] == "--listen" || args[i] == "-l") listen = value("--listen");
        else if (args[i] == "--model" || args[i] == "-m") settings.model = value("--model");
        else if (args[i] == "--mode") settings.mode = value("--mode");
        else throw std::runtime_error("unknown option " + args[i]);
    }
    if (!parse_mode(settings.mode)) throw std::runtime_error("unknown mode '" + settings.mode + "' (manual, auto-read, edit, auto, plan)");

    ServerOptions o;
    o.listen = listen;
    o.settings = settings;
    o.workspaces = workspace_roots(settings);
    o.state = state_dir() / "server";
    o.web = root_dir() / "server" / "web" / "index.html";
    Server server(std::move(o));
    int port = server.bind();
    TokenStore tokens(state_dir() / "server" / "tokens.json");
    std::cout << "maic server on " << (server.tls() ? "https://" : "http://") << listen.substr(0, listen.rfind(':')) << ":" << port
              << "  model " << settings.model << "  mode " << settings.mode << "\n";
    if (server.tls()) std::cout << "certificate SHA-256: " << server.fingerprint() << "  (compare on the phone when it warns)\n";
    std::cout << "workspaces:";
    for (const auto& w : workspace_roots(settings)) std::cout << " " << w.string();
    std::cout << "\n";
    if (tokens.empty()) std::cout << "no tokens yet, so every request gets 401: maic server token new NAME\n";
    std::cout << "Ctrl-C stops it. Every tool call from here is asked about, whatever the mode; the tripwire can be tripped\n"
                 "from a client but never reset.\n";

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::thread serving([&] { server.run(); });
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "\nstopping\n";
    server.stop();
    serving.join();
    return 0;
}

}  // namespace

int run_server_command(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "start" : args[0];
    std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    fs::path state = state_dir() / "server";
    if (sub == "start") return cmd_start(rest);
    if (sub == "token") return cmd_token(rest, state);
    if (sub == "status") return cmd_status(load_settings(), state);
    if (sub == "help" || sub == "-h" || sub == "--help") {
        usage(std::cout);
        return 0;
    }
    usage(std::cerr);
    return 2;
}

}  // namespace maic::server
