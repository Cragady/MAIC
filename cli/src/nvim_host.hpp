#pragma once

#include "maic/nvim_host.hpp"
#include "msgpack.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace maic {

// Why MAIC may not use the nvim socket at `path`, "" when it may; then `fd` is connected to it. The rule
// (docs/nvim.md): a Unix socket owned by this user, whose listening process (SO_PEERCRED, after connecting) runs
// as this user and is one of MAIC's ancestors (the /proc/<pid>/stat parent chain); a socket with nvim's default
// name, nvim.<pid>.<n>, must name that same process. MAIC_NVIM_TRUST_SOCKET=1 skips the ancestry part, for
// tests only, and is ignored unless MAIC_TESTING=1 is set as well.
std::string connect_host_socket(const std::string& path, int& fd);

// The nvim MAIC runs inside, as a msgpack-rpc client of its $NVIM socket (`nvim_set_client_info` name "maic",
// attribute pid). Requests wait with a timeout. The notifications maic.nvim sends (maic_send, maic_command,
// maic_colorscheme) and nvim's error events are handled on a thread of the host's own, never the reader's, so a
// handler may make requests.
class HostNvim : public NvimHost {
public:
    struct Handlers {
        std::function<void(const std::string&)> send;     // maic_send: text for the input
        std::function<void(const std::string&)> command;  // maic_command: a command line, ":" optional
        std::function<void()> colorscheme;                // the host's ColorScheme fired
        std::function<void(const std::string&)> error;    // nvim's error for a notification of ours
        std::function<void()> closed;                     // the connection is gone
    };

    // $NVIM checked and connected. nullptr with `why` empty when $NVIM is not set, with `why` saying what
    // happened when it was refused or failed.
    static std::shared_ptr<HostNvim> from_env(std::string& why);
    static std::shared_ptr<HostNvim> connect(const std::string& socket, std::string& why);
    ~HostNvim() override;
    HostNvim(const HostNvim&) = delete;
    HostNvim& operator=(const HostNvim&) = delete;

    // Replaces the handlers; waits for one that is running. Empty handlers drop whatever arrives (and posts).
    void set_handlers(Handlers h);
    // Runs `task` on the handler thread, unless the handlers are cleared first.
    void post(std::function<void()> task);

    bool connected() const override;
    nlohmann::json exec_lua(const std::string& code, const nlohmann::json& args) override;
    void exec_lua_async(const std::string& code, const nlohmann::json& args) override;
    msgpack::Value request(const std::string& method, std::vector<msgpack::Value> params,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));
    void notify(const std::string& method, std::vector<msgpack::Value> params);

    int64_t channel() const { return channel_; }
    const std::string& socket() const { return socket_; }

private:
    HostNvim(int fd, std::string socket);
    void read_loop();
    void handler_loop();
    void queue(std::function<void()> task);
    void write_all(const std::string& bytes);

    int fd_;
    std::string socket_;
    int64_t channel_ = 0;
    std::thread reader_, handler_;

    struct Reply {
        bool done = false;
        msgpack::Value error, result;
    };
    mutable std::mutex mu_;  // dead_, next_id_, pending_
    std::condition_variable cv_;
    bool dead_ = false;
    uint32_t next_id_ = 1;
    std::map<uint32_t, Reply> pending_;
    std::mutex write_mu_;

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false;
    std::mutex handlers_mu_;  // held while a handler runs
    Handlers handlers_;
    bool has_handlers_ = false;
};

// JSON and msgpack values, for exec_lua's arguments and results. A handle (Buffer, Window) becomes its number.
msgpack::Value to_msgpack(const nlohmann::json& j);
nlohmann::json from_msgpack(const msgpack::Value& v);

}  // namespace maic
