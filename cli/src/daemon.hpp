#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace maid {

class Engine;
struct TuiOptions;

// One connection of JSON-RPC lines to `engine` for `client` (docs/design/engine-protocol.md, section 1): `maid --rpc`
// on stdio and each of the daemon's socket connections. Reads from `in` until it closes, `stop_fd` (when not -1)
// becomes readable or the engine ends the connection; then calls `ended`, waits for the requests still running and
// sends what is queued. `record`, when set, keeps the exchange for `maid protocol check`. True when it ended for a
// fault (a line over 1 MiB, maid_too_slow, a write that failed).
bool serve_lines(Engine& engine, const std::string& client, int in, int out, int stop_fd, const std::filesystem::path& record,
                 const std::function<void()>& ended);

// Where the daemon listens: $XDG_RUNTIME_DIR/maid/engine.sock, or <state>/run/engine.sock without a runtime
// directory (never /tmp), in a 0700 directory.
std::filesystem::path daemon_socket();

// A connected socket to the daemon when one answers there as this user, else -1.
int daemon_connect();

// Why this command line runs its own engine rather than the daemon's: a flag the daemon cannot take from a client
// (`--system`, `--context`, `--no-record`, ...), or "" when everything it asks for travels over the protocol.
// `model_and_mode` says whether --model and --mode travel (the TUI passes them to createConversation).
std::string not_for_daemon(const TuiOptions& options, bool model_and_mode);

// A connection to a running daemon: requests are answered by id, everything else waits for take(). `wake` is
// called from the reader thread each time something arrives unasked.
class DaemonClient {
public:
    // Null when no daemon answers at daemon_socket(), or one answers as another user.
    static std::unique_ptr<DaemonClient> connect(std::function<void()> wake = {});
    ~DaemonClient();

    // Sends one request and waits for its answer; an error answer when the daemon has gone.
    nlohmann::json call(const nlohmann::json& message);
    std::vector<nlohmann::json> take();
    // Ends the connection: the session in this client's focus becomes what leave.quit says (Engine::leave).
    void close();
    bool gone() const { return gone_; }

private:
    explicit DaemonClient(int fd, std::function<void()> wake);
    void read_loop();

    int fd_;
    std::function<void()> wake_;
    std::thread reader_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, nlohmann::json> answers_;  // by the request's id, dumped
    std::deque<nlohmann::json> queue_;
    std::atomic<bool> gone_{false};
    std::mutex write_mu_;
};

// `maid --rpc` with a daemon running: stdin and stdout carried to its socket `sock` unchanged, so maid.nvim's
// interface is one of its clients. Returns the exit code.
int bridge_to_daemon(int sock, int in, int out);

// maid daemon start | stop [--yes] | status [--json] | run | unit [install|remove]
int cmd_daemon(const std::vector<std::string>& args);

// maid liaison send ID (TEXT | --file FILE) [--out FILE] [--timeout SECONDS] [--unattended] | approve ID APPROVAL yes|no | status ID:
// another agent's turns on a session the daemon holds. Exit 3 when no daemon runs; send exits 4 at an approval (never
// with --unattended), 2 when the response fails and 5 at its timeout.
int cmd_liaison(const std::vector<std::string>& args);

}  // namespace maid
