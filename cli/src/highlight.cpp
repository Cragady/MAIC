#include "highlight.hpp"

#include "msgpack.hpp"

#include "maic/helper.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace maic {

namespace {

using msgpack::Value;

// Runs inside nvim: the buffer becomes the text, the markdown parser (with its injections: markdown_inline,
// and the fenced languages nvim has parsers for) is run, and every highlight capture comes back as
// {row, byte start, byte end, capture name}, split per row for nodes that span lines.
const char* kLua = R"lua(
local lines, lang = ...
vim.api.nvim_buf_set_lines(0, 0, -1, true, lines)
local ok, parser = pcall(vim.treesitter.get_parser, 0, lang)
if not ok or not parser then return {} end
parser:parse(true)
local out = {}
parser:for_each_tree(function(tree, ltree)
  local query = vim.treesitter.query.get(ltree:lang(), 'highlights')
  if not query then return end
  for id, node in query:iter_captures(tree:root(), 0) do
    local r1, c1, r2, c2 = node:range()
    for row = r1, r2 do
      local a = row == r1 and c1 or 0
      local b = row == r2 and c2 or #(lines[row + 1] or '')
      if b > a then out[#out + 1] = { row, a, b, query.captures[id] } end
    end
  end
end)
return out
)lua";

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        out.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) return out;
        start = nl + 1;
    }
}

// The captures laid over the lines: one flag word per byte, then runs of equal flags become spans.
std::vector<StyledLine> paint(const std::vector<std::string>& lines, const Value& captures) {
    std::vector<std::vector<unsigned>> flags(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) flags[i].assign(lines[i].size(), MdNone);
    if (captures.is_array()) {
        for (const auto& c : captures.array) {
            if (!c.is_array() || c.array.size() != 4 || !c.array[0].is_int() || !c.array[1].is_int() || !c.array[2].is_int() || !c.array[3].is_str()) continue;
            unsigned f = capture_flags(c.array[3].s);
            if (!f || c.array[0].i < 0 || static_cast<size_t>(c.array[0].i) >= lines.size()) continue;
            auto& row = flags[static_cast<size_t>(c.array[0].i)];
            size_t a = static_cast<size_t>(std::max<int64_t>(0, c.array[1].i)), b = static_cast<size_t>(std::max<int64_t>(0, c.array[2].i));
            for (size_t k = a; k < b && k < row.size(); ++k) row[k] |= f;
        }
    }
    std::vector<StyledLine> out(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        for (size_t k = 0; k < l.size();) {
            size_t j = k;
            while (j < l.size() && flags[i][j] == flags[i][k]) ++j;
            // never split a UTF-8 sequence: a capture boundary inside one stretches to its end
            while (j < l.size() && (static_cast<unsigned char>(l[j]) & 0xC0) == 0x80) ++j;
            out[i].push_back({l.substr(k, j - k), flags[i][k]});
            k = j;
        }
    }
    return out;
}

bool starts(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

}  // namespace

unsigned capture_flags(const std::string& name) {
    if (starts(name, "markup.heading")) return HlHeading;
    if (starts(name, "markup.raw")) return HlCode;
    if (starts(name, "markup.strong")) return MdBold;
    if (starts(name, "markup.italic")) return MdItalic;
    if (starts(name, "markup.link")) return MdLink;
    if (starts(name, "markup.list")) return MdBullet;
    if (starts(name, "markup.quote")) return MdQuote;
    if (starts(name, "comment")) return HlComment;
    if (starts(name, "keyword")) return HlKeyword;
    if (starts(name, "string")) return HlString;
    return MdNone;
}

NvimHighlighter::NvimHighlighter(std::string nvim, std::function<void()> on_ready) : nvim_(std::move(nvim)), on_ready_(std::move(on_ready)) {
    start();
}

// The child gets the pipes as its stdio; a close-on-exec pipe carries exec's errno back when it fails, so a
// missing nvim is reported as such rather than as an early exit.
void NvimHighlighter::start() {
    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
        error_ = std::string("pipe: ") + std::strerror(errno);
        dead_ = true;
        return;
    }
    // Built before fork, like the arguments: nvim is a fixed helper here and gets no model key.
    std::vector<std::string> args = {nvim_, "--embed", "--headless", "-u", "NONE", "-i", "NONE", "-n"}, env = keyless_environ();
    std::vector<char*> argv, envp;
    for (auto& a : args) argv.push_back(a.data());
    for (auto& kv : env) envp.push_back(kv.data());
    argv.push_back(nullptr);
    envp.push_back(nullptr);
    pid_ = fork();
    if (pid_ < 0) {
        error_ = std::string("fork: ") + std::strerror(errno);
        dead_ = true;
        return;
    }
    if (pid_ == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        execvpe(argv[0], argv.data(), envp.data());
        int e = errno;
        [[maybe_unused]] ssize_t w = write(err[1], &e, sizeof e);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    to_nvim_ = in[1];
    from_nvim_ = out[0];
    int e = 0;
    ssize_t n = read(err[0], &e, sizeof e);  // EOF (0) the moment exec succeeds
    close(err[0]);
    if (n == static_cast<ssize_t>(sizeof e)) {
        error_ = nvim_ + ": " + std::strerror(e);
        dead_ = true;
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
        close(to_nvim_);
        close(from_nvim_);
        to_nvim_ = from_nvim_ = -1;
        return;
    }
    reader_ = std::thread([this] { read_loop(); });
}

NvimHighlighter::~NvimHighlighter() {
    {
        std::lock_guard lock(mu_);
        dead_ = true;
    }
    if (to_nvim_ >= 0) close(to_nvim_);  // nvim leaves when its channel closes
    to_nvim_ = -1;
    if (pid_ > 0) {
        int status = 0;
        bool gone = false;
        for (int i = 0; i < 50 && !gone; ++i) {
            gone = waitpid(pid_, &status, WNOHANG) == pid_;
            if (!gone) usleep(10000);
        }
        if (!gone) {
            kill(pid_, SIGTERM);
            for (int i = 0; i < 20 && !gone; ++i) {
                gone = waitpid(pid_, &status, WNOHANG) == pid_;
                if (!gone) usleep(10000);
            }
        }
        if (!gone) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
    }
    if (reader_.joinable()) reader_.join();
    if (from_nvim_ >= 0) close(from_nvim_);
}

void NvimHighlighter::fail(const std::string& why) {
    if (!dead_) error_ = why;
    dead_ = true;
    cv_.notify_all();
}

bool NvimHighlighter::alive() const {
    std::lock_guard lock(mu_);
    return !dead_;
}

std::string NvimHighlighter::error() const {
    std::lock_guard lock(mu_);
    return error_;
}

// Replies are [1, msgid, error, result]; the one for the request in flight becomes the answer for its text,
// anything else (notifications, replies to requests that were overtaken) is dropped.
void NvimHighlighter::read_loop() {
    std::string buf;
    char chunk[65536];
    for (;;) {
        ssize_t n = read(from_nvim_, chunk, sizeof chunk);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            std::lock_guard lock(mu_);
            fail("nvim exited");
            break;
        }
        buf.append(chunk, static_cast<size_t>(n));
        size_t pos = 0;
        bool notify = false, failed = false;
        try {
            Value msg;
            while (!failed && msgpack::decode(buf, pos, msg)) {
                if (!msg.is_array() || msg.array.size() != 4 || !msg.array[0].is_int() || msg.array[0].i != 1 || !msg.array[1].is_int()) continue;
                std::lock_guard lock(mu_);
                if (static_cast<uint32_t>(msg.array[1].i) != pending_id_) continue;
                if (!msg.array[2].is_nil()) {
                    const Value& err = msg.array[2];
                    fail("nvim: " + (err.is_array() && err.array.size() == 2 && err.array[1].is_str() ? err.array[1].s : err.is_str() ? err.s : "error"));
                    failed = true;
                    continue;
                }
                done_lines_ = paint(split_lines(pending_text_), msg.array[3]);
                done_text_ = pending_text_;
                have_done_ = true;
                pending_id_ = 0;
                notify = waiting_ == 0;
                cv_.notify_all();
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mu_);
            fail(std::string("nvim: ") + e.what());
            failed = true;
        }
        if (failed) break;
        buf.erase(0, pos);
        if (notify && on_ready_) on_ready_();
    }
    if (on_ready_) on_ready_();
}

std::optional<std::vector<StyledLine>> NvimHighlighter::highlight(const std::string& text, std::chrono::milliseconds budget) {
    std::unique_lock lock(mu_);
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (dead_) return std::nullopt;
        if (have_done_ && done_text_ == text) return done_lines_;
        if (pending_id_ == 0) {
            uint32_t id = next_id_++;
            pending_id_ = id;
            pending_text_ = text;
            std::vector<Value> lines;
            for (auto& l : split_lines(text)) lines.push_back(Value::str(std::move(l)));
            std::string req = msgpack::encode(Value::arr({Value::integer(0), Value::integer(id), Value::str("nvim_exec_lua"),
                                                          Value::arr({Value::str(kLua), Value::arr({Value::arr(std::move(lines)), Value::str("markdown")})})}));
            lock.unlock();
            size_t off = 0;
            bool ok = true;
            while (off < req.size()) {
                ssize_t n = write(to_nvim_, req.data() + off, req.size() - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    ok = false;
                    break;
                }
                off += static_cast<size_t>(n);
            }
            lock.lock();
            if (!ok) {
                fail(std::string("nvim: write failed: ") + std::strerror(errno));
                return std::nullopt;
            }
        }
        ++waiting_;
        bool answered = cv_.wait_until(lock, deadline, [&] { return dead_ || pending_id_ == 0; });
        --waiting_;
        if (!answered) return std::nullopt;  // over budget: skip this refresh, on_ready brings the reply later
    }
}

}  // namespace maic
