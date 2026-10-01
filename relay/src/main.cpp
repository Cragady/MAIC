// maic-relay: the rendezvous point a workstation's maic-server dials out to and a phone connects to. It
// carries encrypted frames between the two and keeps nothing. docs/remote.md.
#include "relay.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop = true;
}

void usage(std::ostream& out) {
    out << "usage: maic-relay [--listen ADDR:PORT] [--cert FILE --key FILE] [--state DIR] [--web FILE]\n"
           "                  [--idle SECONDS] [--max-pairs N] [--rate BYTES_PER_SECOND] [--log FILE]\n"
           "\n"
           "  --listen     default 127.0.0.1:7474; any other address needs TLS: --cert/--key, or a self-signed\n"
           "               pair made under --state (default ~/.local/state/maic-relay) whose fingerprint is printed\n"
           "  --web        the MAIC web client to serve at /, so a phone can open the relay's address\n"
           "               (default: share/maic/server/web/index.html beside this binary, when it exists)\n"
           "  --idle       drop a side silent this long, and its pair (default 60)\n"
           "  --max-pairs  pairing ids held at once (default 64)\n"
           "  --rate       bytes per second per pairing id (default 4194304)\n"
           "  --log        where the one line per attach and detach goes (default stdout); only pairing ids,\n"
           "               byte counts and times are ever written\n";
}

std::filesystem::path default_state() {
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) return std::filesystem::path(x) / "maic-relay";
    if (const char* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".local" / "state" / "maic-relay";
    return std::filesystem::current_path() / "maic-relay-state";
}

std::filesystem::path default_web() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    auto web = exe.parent_path().parent_path() / "share" / "maic" / "server" / "web" / "index.html";
    return std::filesystem::exists(web, ec) ? web : std::filesystem::path{};
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    maic::relay::RelayOptions o;
    o.state = default_state();
    o.web = default_web();
    std::ofstream log_file;
    o.log = &std::cout;
    try {
        for (size_t i = 0; i < args.size(); ++i) {
            auto value = [&](const char* flag) {
                if (i + 1 >= args.size()) throw std::runtime_error(std::string(flag) + " needs a value");
                return args[++i];
            };
            if (args[i] == "--listen" || args[i] == "-l") o.listen = value("--listen");
            else if (args[i] == "--cert") o.cert = value("--cert");
            else if (args[i] == "--key") o.key = value("--key");
            else if (args[i] == "--state") o.state = value("--state");
            else if (args[i] == "--web") o.web = value("--web");
            else if (args[i] == "--idle") o.idle_seconds = std::stoi(value("--idle"));
            else if (args[i] == "--max-pairs") o.max_pairs = std::stoul(value("--max-pairs"));
            else if (args[i] == "--rate") o.rate = std::stoul(value("--rate"));
            else if (args[i] == "--log") {
                log_file.open(value("--log"), std::ios::app);
                if (!log_file) throw std::runtime_error("can't open the log file");
                o.log = &log_file;
            } else if (args[i] == "-h" || args[i] == "--help") {
                usage(std::cout);
                return 0;
            } else throw std::runtime_error("unknown option " + args[i]);
        }
        maic::relay::Relay relay(o);
        int port = relay.bind();
        std::cout << "maic-relay on " << (relay.tls() ? "https://" : "http://") << o.listen.substr(0, o.listen.rfind(':')) << ":" << port
                  << "  idle " << o.idle_seconds << "s  max-pairs " << o.max_pairs << "  rate " << o.rate << " B/s\n";
        if (relay.tls()) std::cout << "certificate SHA-256: " << relay.fingerprint() << "  (server.relay_cert on the workstation pins it; the phone compares it when its browser warns)\n";
        std::cout << (o.web.empty() ? "no web client at / (start with --web to serve one)\n" : "web client at /: " + o.web.string() + "\n")
                  << "It forwards encrypted frames by pairing id and stores nothing. Ctrl-C stops it.\n";
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::thread serving([&] { relay.run(); });
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "\nstopping\n";
        relay.stop();
        serving.join();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "maic-relay: " << e.what() << "\n";
        return 1;
    }
}
