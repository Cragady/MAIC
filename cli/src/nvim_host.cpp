#include "nvim_host.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>

#ifndef MAID_VERSION
#define MAID_VERSION "dev"
#endif

namespace maid {

using msgpack::Value;
using nlohmann::json;

namespace {

// The parent of `pid` from /proc/<pid>/stat (the field after the command name, which may hold spaces and
// parentheses itself); 0 when it cannot be read.
pid_t parent_of(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t close = stat.rfind(')');
    if (close == std::string::npos) return 0;
    char state = 0;
    long ppid = 0;
    if (sscanf(stat.c_str() + close + 1, " %c %ld", &state, &ppid) != 2) return 0;
    return static_cast<pid_t>(ppid);
}

bool is_ancestor(pid_t pid) {
    if (pid <= 1) return false;
    pid_t p = getppid();
    for (int i = 0; i < 128 && p > 1; ++i, p = parent_of(p)) {
        if (p == pid) return true;
    }
    return false;
}

bool env_is(const char* name, const char* value) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, value) == 0;
}

Value float_value(double d) {
    Value v;
    v.kind = Value::Kind::Float;
    v.f = d;
    return v;
}

std::string error_text(const Value& err) {
    if (err.is_array() && err.array.size() == 2 && err.array[1].is_str()) return err.array[1].s;
    if (err.is_str()) return err.s;
    return "error";
}

}  // namespace

std::string connect_host_socket(const std::string& path, int& fd) {
    fd = -1;
    if (path.empty() || path[0] != '/') return "not a Unix socket path";
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return std::string("cannot stat it: ") + std::strerror(errno);
    if (!S_ISSOCK(st.st_mode)) return "not a socket";
    if (st.st_uid != getuid()) return "owned by another user";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return "the path is too long for a Unix socket";
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return std::string("socket: ") + std::strerror(errno);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::string why = std::string("cannot connect: ") + std::strerror(errno);
        close(s);
        return why;
    }
    ucred cred{};
    socklen_t len = sizeof cred;
    std::string why;
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) why = std::string("cannot read its credentials: ") + std::strerror(errno);
    else if (cred.uid != getuid()) why = "the process behind it runs as another user";
    if (why.empty()) {
        std::smatch m;
        std::string name = std::filesystem::path(path).filename().string();
        if (std::regex_match(name, m, std::regex(R"(nvim\.(\d+)\.\d+)")) && std::stol(m[1]) != cred.pid) {
            why = "its name says nvim " + m[1].str() + " but nvim " + std::to_string(cred.pid) + " is behind it";
        }
    }
    bool trust = env_is("MAID_TESTING", "1") && env_is("MAID_NVIM_TRUST_SOCKET", "1");
    if (why.empty() && !trust && !is_ancestor(cred.pid)) why = "nvim " + std::to_string(cred.pid) + " behind it is not one of this MAID's parent processes";
    if (!why.empty()) {
        close(s);
        return why;
    }
    fd = s;
    return "";
}

Value to_msgpack(const json& j) {
    switch (j.type()) {
        case json::value_t::null: return Value::nil();
        case json::value_t::boolean: return Value::boolean(j.get<bool>());
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: return Value::integer(j.get<int64_t>());
        case json::value_t::number_float: return float_value(j.get<double>());
        case json::value_t::string: return Value::str(j.get<std::string>());
        case json::value_t::array: {
            std::vector<Value> items;
            for (const auto& e : j) items.push_back(to_msgpack(e));
            return Value::arr(std::move(items));
        }
        case json::value_t::object: {
            Value m;
            m.kind = Value::Kind::Map;
            for (const auto& [k, v] : j.items()) m.map.push_back({Value::str(k), to_msgpack(v)});
            return m;
        }
        default: return Value::nil();
    }
}

json from_msgpack(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Nil: return nullptr;
        case Value::Kind::Bool: return v.b;
        case Value::Kind::Int: return v.i;
        case Value::Kind::Float: return v.f;
        case Value::Kind::Str: return v.s;
        case Value::Kind::Ext: return v.i;
        case Value::Kind::Array: {
            json out = json::array();
            for (const auto& e : v.array) out.push_back(from_msgpack(e));
            return out;
        }
        case Value::Kind::Map: {
            json out = json::object();
            for (const auto& [k, val] : v.map) out[k.is_str() ? k.s : std::to_string(k.i)] = from_msgpack(val);
            return out;
        }
    }
    return nullptr;
}

std::shared_ptr<HostNvim> HostNvim::from_env(std::string& why) {
    why.clear();
    const char* sock = std::getenv("NVIM");
    if (!sock || !*sock) return nullptr;
    return connect(sock, why);
}

std::shared_ptr<HostNvim> HostNvim::connect(const std::string& socket, std::string& why) {
    int fd = -1;
    why = connect_host_socket(socket, fd);
    if (!why.empty()) return nullptr;
    std::shared_ptr<HostNvim> host(new HostNvim(fd, socket));
    try {
        Value info = host->request("nvim_get_api_info", {});
        if (!info.is_array() || info.array.empty() || !info.array[0].is_int()) throw std::runtime_error("nvim_get_api_info gave no channel");
        host->channel_ = info.array[0].i;
        json version = {{"major", 0}, {"minor", 0}, {"patch", 0}, {"prerelease", MAID_VERSION}};
        json attributes = {{"pid", std::to_string(getpid())}};
        host->request("nvim_set_client_info", {Value::str("maid"), to_msgpack(version), Value::str("remote"), to_msgpack(json::object()), to_msgpack(attributes)});
    } catch (const std::exception& e) {
        why = e.what();
        return nullptr;
    }
    return host;
}

HostNvim::HostNvim(int fd, std::string socket) : fd_(fd), socket_(std::move(socket)) {
    reader_ = std::thread([this] { read_loop(); });
    handler_ = std::thread([this] { handler_loop(); });
}

HostNvim::~HostNvim() {
    {
        std::lock_guard lock(queue_mu_);
        stop_ = true;
    }
    queue_cv_.notify_all();
    if (handler_.joinable()) handler_.join();
    shutdown(fd_, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    close(fd_);
}

void HostNvim::set_handlers(Handlers h) {
    std::lock_guard lock(handlers_mu_);
    has_handlers_ = h.send || h.command || h.colorscheme || h.interrupt || h.error || h.closed;
    handlers_ = std::move(h);
}

void HostNvim::queue(std::function<void()> task) {
    {
        std::lock_guard lock(queue_mu_);
        tasks_.push_back(std::move(task));
    }
    queue_cv_.notify_one();
}

void HostNvim::post(std::function<void()> task) {
    queue(std::move(task));
}

void HostNvim::handler_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(queue_mu_);
            queue_cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
            if (stop_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        std::lock_guard lock(handlers_mu_);
        if (!has_handlers_) continue;
        try {
            task();
        } catch (const std::exception& e) {
            if (handlers_.error) handlers_.error(e.what());
        }
    }
}

bool HostNvim::connected() const {
    std::lock_guard lock(mu_);
    return !dead_;
}

void HostNvim::write_all(const std::string& bytes) {
    std::lock_guard lock(write_mu_);
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("the nvim host is gone");
        off += static_cast<size_t>(n);
    }
}

Value HostNvim::request(const std::string& method, std::vector<Value> params, std::chrono::milliseconds timeout) {
    uint32_t id;
    {
        std::lock_guard lock(mu_);
        if (dead_) throw std::runtime_error("the nvim host is gone");
        id = next_id_++;
        pending_[id] = Reply{};
    }
    try {
        write_all(msgpack::encode(Value::arr({Value::integer(0), Value::integer(id), Value::str(method), Value::arr(std::move(params))})));
    } catch (...) {
        std::lock_guard lock(mu_);
        pending_.erase(id);
        throw;
    }
    std::unique_lock lock(mu_);
    cv_.wait_for(lock, timeout, [&] { return dead_ || pending_[id].done; });
    Reply r = std::move(pending_[id]);
    pending_.erase(id);
    if (!r.done) throw std::runtime_error(dead_ ? "the nvim host is gone" : method + ": nvim did not answer in time");
    if (!r.error.is_nil()) throw std::runtime_error(error_text(r.error));
    return r.result;
}

void HostNvim::notify(const std::string& method, std::vector<Value> params) {
    if (!connected()) return;
    try {
        write_all(msgpack::encode(Value::arr({Value::integer(2), Value::str(method), Value::arr(std::move(params))})));
    } catch (const std::exception&) {
        // the reader sees the same and reports the host as gone
    }
}

json HostNvim::exec_lua(const std::string& code, const json& args) {
    return from_msgpack(request("nvim_exec_lua", {Value::str(code), to_msgpack(args.is_array() ? args : json::array())}));
}

void HostNvim::exec_lua_async(const std::string& code, const json& args) {
    notify("nvim_exec_lua", {Value::str(code), to_msgpack(args.is_array() ? args : json::array())});
}

// Replies go to the request waiting for them; notifications become tasks for the handler thread; a request
// from nvim (nothing of maid.nvim's sends one) is answered with an error so nvim never waits on MAID.
void HostNvim::read_loop() {
    std::string buf;
    char chunk[65536];
    for (;;) {
        ssize_t n = read(fd_, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t pos = 0;
        try {
            Value msg;
            while (msgpack::decode(buf, pos, msg)) {
                if (!msg.is_array() || msg.array.empty() || !msg.array[0].is_int()) continue;
                const auto& a = msg.array;
                if (a[0].i == 1 && a.size() == 4 && a[1].is_int()) {
                    std::lock_guard lock(mu_);
                    auto it = pending_.find(static_cast<uint32_t>(a[1].i));
                    if (it == pending_.end()) continue;
                    it->second = {true, a[2], a[3]};
                    cv_.notify_all();
                } else if (a[0].i == 2 && a.size() == 3 && a[1].is_str()) {
                    std::string method = a[1].s;
                    std::string text = a[2].is_array() && !a[2].array.empty() && a[2].array[0].is_str() ? a[2].array[0].s : "";
                    if (method == "maid_send") queue([this, text] { if (handlers_.send) handlers_.send(text); });
                    else if (method == "maid_command") queue([this, text] { if (handlers_.command) handlers_.command(text); });
                    else if (method == "maid_interrupt") queue([this] { if (handlers_.interrupt) handlers_.interrupt(); });
                    else if (method == "maid_colorscheme") queue([this] { if (handlers_.colorscheme) handlers_.colorscheme(); });
                    else if (method == "nvim_error_event") {
                        std::string why = a[2].is_array() && a[2].array.size() >= 2 && a[2].array[1].is_str() ? a[2].array[1].s : "error";
                        queue([this, why] { if (handlers_.error) handlers_.error(why); });
                    }
                } else if (a[0].i == 0 && a.size() == 4 && a[1].is_int()) {
                    write_all(msgpack::encode(Value::arr({Value::integer(1), a[1], Value::str("maid takes no requests; use rpcnotify"), Value::nil()})));
                }
            }
        } catch (const std::exception&) {
            break;
        }
        buf.erase(0, pos);
    }
    {
        std::lock_guard lock(mu_);
        dead_ = true;
    }
    cv_.notify_all();
    queue([this] { if (handlers_.closed) handlers_.closed(); });
}

}  // namespace maid
