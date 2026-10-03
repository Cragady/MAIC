// `maic server ...`: start, token new|list|revoke, pair, pairs, unpair, status; and `maic artifact ...`.
#include "artifacts.hpp"
#include "auth.hpp"
#include "server.hpp"
#include "tunnel.hpp"

#include <nlohmann/json.hpp>

#include "maic/harness.hpp"
#include "maic/paths.hpp"
#include "maic/settings.hpp"
#include "maic/theme.hpp"

#include "maic/http.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
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
           "       maic server pair               a one-time code and pairing string for a phone, valid 2 minutes (server.relay set)\n"
           "       maic server pairs              the phones paired for the relay\n"
           "       maic server unpair NAME\n"
           "       maic server status             the configuration, the relay link, and whether a server answers\n"
           "\n"
           "Loopback is the default. Any other address needs TLS: a self-signed certificate is made on first use\n"
           "(pin its fingerprint on the phone), or set server.cert and server.key in settings. With server.relay set\n"
           "the server dials out to a maic-relay so a paired phone reaches it from anywhere, end-to-end encrypted.\n"
           "docs/remote.md\n";
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

int cmd_pair(const std::vector<std::string>& args, const Settings& settings, const fs::path& state) {
    PairStore store(state / "pairs.json");
    if (args.empty()) {
        if (settings.server.relay.empty()) throw std::runtime_error("set server.relay = \"https://host:port\" (the maic-relay the server dials out to) in settings first; maic help server");
        std::string code = new_pairing_code();
        write_pairing_offer(state / "pairing.json", code);
        std::cout << "pairing code, valid two minutes: " << code.substr(0, 4) << " " << code.substr(4) << "\n\n"
                  << "On the phone, on this LAN, open the web client (the token box first if it has none), then Sessions > Pair with\n"
                     "a relay, and paste this string. The exchange runs over the LAN, straight to this server, never through the relay:\n\n"
                  << "    maic://pair/" << settings.server.relay << "/" << store.pairing_id() << "/" << code << "\n\n"
                  << "The server must be running (maic server start). Three wrong codes void the offer.\n";
        return 0;
    }
    if (args.size() == 1 && args[0] == "list") {
        auto all = store.list();
        if (all.empty()) std::cout << "no phones paired yet: maic server pair\n";
        for (const auto& p : all) std::cout << p.name << "  key " << p.public_key.substr(0, 12) << "...  paired " << p.created << "\n";
        return 0;
    }
    if (args.size() == 2 && args[0] == "remove") {
        if (!store.remove(args[1])) {
            std::cerr << "maic server: no phone named " << args[1] << "\n";
            return 1;
        }
        std::cout << "unpaired " << args[1] << "; its next connection through the relay is refused\n";
        return 0;
    }
    usage(std::cerr);
    return 2;
}

// The relay link as the running server last wrote it (<state>/relay.json), for the status line.
std::string relay_notice(const Settings& settings, const fs::path& state) {
    if (settings.server.relay.empty()) return "none (server.relay in settings)";
    std::ifstream in(state / "relay.json");
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
    std::string out = settings.server.relay;
    if (!j.is_object()) return out + "  (no link yet: start the server)";
    if (j.value("connected", false)) return out + "  connected since " + j.value("since", "");
    std::string last = j.value("last_connected", "");
    out += "  not connected" + (last.empty() ? std::string(", never has been") : ", last " + last);
    if (!j.value("error", "").empty()) out += " (" + j.value("error", "") + ")";
    return out;
}

int cmd_status(const Settings& settings, const fs::path& state) {
    std::string listen = settings.server.listen;
    size_t colon = listen.rfind(':');
    std::string host = colon == std::string::npos ? listen : listen.substr(0, colon);
    std::string port = colon == std::string::npos ? "7373" : listen.substr(colon + 1);
    bool loopback = loopback_host(host);
    TokenStore store(state / "tokens.json");
    std::cout << "listen:     " << listen << (loopback ? "  (loopback, plain HTTP)" : "  (TLS)") << "\n";
    if (!loopback) {
        fs::path cert = settings.server.cert.empty() ? state / "cert.pem" : settings.server.cert;
        std::cout << "cert:       " << cert.string() << (fs::exists(cert) ? "" : "  (made at first start)") << "\n";
    }
    std::cout << "workspaces:";
    for (const auto& w : workspace_roots(settings)) std::cout << " " << w.string();
    std::cout << "\ntokens:     " << store.list().size() << " (maic server token list)\n"
              << "relay:      " << relay_notice(settings, state) << "\n"
              << "paired:     " << PairStore(state / "pairs.json").list().size() << " phone(s) (maic server pairs)\n"
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
    o.artifacts = state_dir() / "artifacts";
    o.vue = root_dir() / "vendor" / "vue";
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
    if (!settings.server.relay.empty()) std::cout << "relay: dialling " << settings.server.relay << " (maic server status shows the link; maic server pair enrols a phone)\n";
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

void artifact_usage(std::ostream& out) {
    out << "usage: maic artifact list              the artifacts maic-server serves, with their trust and data documents\n"
           "       maic artifact add DIR [--id ID]  copy a built page folder in (index.html at its top); again to update it,\n"
           "                                    its saved data/ is kept\n"
           "       maic artifact open ID           a one-time link for a browser on this machine, valid 2 minutes\n"
           "       maic artifact allow-insecure ID [--off]   add 'unsafe-eval' to that artifact's script policy (asks you to type\n"
           "                                    \"allow insecure\" at a terminal); --off removes it without asking\n"
           "       maic artifact watch ID [--doc NAME] [--once]   a line per event an agent acts on (submitted, side_prompt N,\n"
           "                                    after_prompt N, split ID) when the page saves data/NAME.json (answers)\n"
           "       maic artifact protocol ID [--propose FILE | --approve | --verify HASH]   the notify protocol events name;\n"
           "                                    --approve asks you to type \"approve\" at a terminal; --verify re-hashes it\n"
           "\n"
           "Artifacts live in ~/.local/state/maic/artifacts/ and are served sandboxed at /a/ID/ by maic server start.\n"
           "docs/artifacts.md; watch, protocol and maic channel: docs/agent-kit.md\n";
}

// The server's own address as this machine reaches it, from server.listen.
std::string local_server_url(const Settings& settings) {
    std::string listen = settings.server.listen;
    size_t colon = listen.rfind(':');
    std::string host = colon == std::string::npos ? listen : listen.substr(0, colon);
    std::string port = colon == std::string::npos ? "7373" : listen.substr(colon + 1);
    bool tls = !loopback_host(host) || !settings.server.cert.empty();
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") host = "127.0.0.1";
    return (tls ? "https://" : "http://") + host + ":" + port;
}

}  // namespace

int run_artifact_command(const std::vector<std::string>& args, bool text_base) {
    fs::path root = state_dir() / "artifacts";
    std::string sub = args.empty() ? "list" : args[0];
    if (sub == "list" && args.size() <= 1) {
        auto all = list_artifacts(root);
        if (all.empty()) std::cout << "no artifacts yet: maic artifact add DIR\n";
        Settings settings;
        bool color = color_output(text_base, STDOUT_FILENO);
        if (color) {
            try {
                settings = load_settings();
            } catch (const std::exception&) {
                apply_theme(settings, Theme{"default"});  // the built-in styles paint it; the commands that need settings report the error
            }
        }
        for (const auto& a : all) {
            std::cout << a.id << "  " << a.trust << (a.added.empty() ? "" : "  added " + a.added);
            if (a.allow_insecure) std::cout << "  " << (color ? ansi_paint("ALLOW_INSECURE", settings.style("notice"), color_depth(settings.colors)) : "ALLOW_INSECURE");
            for (size_t i = 0; i < a.data.size(); ++i) std::cout << (i ? ", " : "  data: ") << a.data[i];
            std::cout << "\n";
        }
        return 0;
    }
    if (sub == "add" && args.size() >= 2) {
        fs::path src = args[1];
        std::string id = src.lexically_normal().filename().string();
        if (id.empty() || id == ".") id = fs::absolute(src).lexically_normal().parent_path().filename().string();
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = args[++i];
            else throw std::runtime_error("unknown option " + args[i]);
        }
        for (const auto& s : add_artifact(root, src, id)) std::cerr << "maic artifact: skipped " << s << "\n";
        std::cout << "added " << id << " at " << (root / id).string() << " (sandboxed); maic artifact open " << id << "\n";
        return 0;
    }
    if (sub == "open" && args.size() == 2) {
        std::error_code ec;
        if (!artifact_name_ok(args[1]) || !fs::is_directory(root / args[1], ec)) throw std::runtime_error("no artifact " + args[1] + "; maic artifact list");
        std::string code = new_artifact_login(state_dir() / "server");
        std::cout << local_server_url(load_settings()) << "/a/_login?code=" << code << "&to=" << args[1] << "\n\n"
                  << "Open it in a browser on this machine within two minutes; it works once and logs that browser in to\n"
                  << "artifacts for 12 hours (afterwards /a/" << args[1] << "/ opens it directly). maic server start must be running.\n";
        return 0;
    }
    if (sub == "allow-insecure" && (args.size() == 2 || (args.size() == 3 && args[2] == "--off"))) {
        std::error_code ec;
        if (!artifact_name_ok(args[1]) || !fs::is_directory(root / args[1], ec)) throw std::runtime_error("no artifact " + args[1] + "; maic artifact list");
        fs::path dir = root / args[1];
        if (args.size() == 3) {
            set_artifact_allow_insecure(dir, false);
            std::cout << args[1] << ": ALLOW_INSECURE is off; its script policy has no 'unsafe-eval'\n";
            return 0;
        }
        std::cout << "ALLOW_INSECURE for " << args[1] << " adds 'unsafe-eval' to that artifact's script policy only, which lets its page\n"
                  << "compile templates and run strings as code (Vue's in-page template compiler needs it). The sandbox, the opaque\n"
                  << "origin and the network limits stay. Every load of its index.html is written to the server's audit log, and\n"
                  << "maic artifact list shows it. Turn it off with: maic artifact allow-insecure " << args[1] << " --off\n";
        if (!isatty(STDIN_FILENO)) {
            std::cerr << "maic artifact: allowing insecure asks you to type a phrase at a terminal; run it in one. Nothing was changed.\n";
            return 2;
        }
        std::cout << "type \"allow insecure\" to turn it on: " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line != "allow insecure") {
            std::cout << "nothing changed\n";
            return 1;
        }
        set_artifact_allow_insecure(dir, true);
        std::cout << args[1] << ": ALLOW_INSECURE is on\n";
        return 0;
    }
    if (sub == "watch" && args.size() >= 2) return artifact_watch(root, args);
    if (sub == "protocol" && args.size() >= 2) return artifact_protocol(root, args);
    if (sub == "help" || sub == "-h" || sub == "--help") {
        artifact_usage(std::cout);
        return 0;
    }
    artifact_usage(std::cerr);
    return 2;
}

int run_server_command(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "start" : args[0];
    std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    fs::path state = state_dir() / "server";
    if (sub == "start") return cmd_start(rest);
    if (sub == "token") return cmd_token(rest, state);
    if (sub == "pair") return cmd_pair(rest, load_settings(), state);
    if (sub == "pairs") return cmd_pair({"list"}, load_settings(), state);
    if (sub == "unpair" && rest.size() == 1) return cmd_pair({"remove", rest[0]}, load_settings(), state);
    if (sub == "status") return cmd_status(load_settings(), state);
    if (sub == "help" || sub == "-h" || sub == "--help") {
        usage(std::cout);
        return 0;
    }
    usage(std::cerr);
    return 2;
}

}  // namespace maic::server
