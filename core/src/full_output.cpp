#include "maic/full_output.hpp"

#include "maic/session.hpp"
#include "maic/vendor.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace maic {

namespace fs = std::filesystem;

namespace {

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path index_of(const fs::path& out) {
    fs::path idx = out;
    return idx.replace_extension(".idx");
}

}  // namespace

FullOutputWriter::FullOutputWriter(fs::path out, size_t max_bytes) : out_(std::move(out)) {
    tail_limit_ = std::min<size_t>(max_bytes / 4, 8 << 20);
    head_limit_ = max_bytes - tail_limit_;
}

FullOutputWriter::~FullOutputWriter() {
    if (out_fd_ >= 0) close(out_fd_);
    if (idx_fd_ >= 0) close(idx_fd_);
}

void FullOutputWriter::add(OutputStream stream, std::string_view bytes) {
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    char s = stream == OutputStream::Stdout ? 'o' : 'e';
    totals_[stream == OutputStream::Stdout ? 0 : 1] += bytes.size();
    if (!opened_) {
        early_.push_back({s, std::string(bytes), ms});
        if (std::max(totals_[0], totals_[1]) <= kOutputHeadBytes + kOutputTailBytes) return;
        open_files();
        for (const auto& c : early_) put(c.stream, c.data, c.ms);
        early_.clear();
        return;
    }
    put(s, bytes, ms);
}

void FullOutputWriter::open_files() {
    opened_ = true;
    std::error_code ec;
    fs::create_directories(out_.parent_path(), ec);
    fs::permissions(out_.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
    out_fd_ = open(out_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out_fd_ >= 0) idx_fd_ = open(index_of(out_).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out_fd_ < 0 || idx_fd_ < 0) error_ = "can't create " + (out_fd_ < 0 ? out_ : index_of(out_)).string() + ": " + std::strerror(errno);
}

void FullOutputWriter::write_chunk(char stream, std::string_view data, long ms) {
    if (!error_.empty() || data.empty()) return;
    std::string line = std::string(1, stream) + " " + std::to_string(written_) + " " + std::to_string(data.size()) + " " + std::to_string(ms) + "\n";
    if (!write_all(out_fd_, data) || !write_all(idx_fd_, line)) error_ = "can't write " + out_.string() + ": " + std::strerror(errno);
    written_ += data.size();
}

void FullOutputWriter::put(char stream, std::string_view data, long ms) {
    if (written_ < head_limit_) {
        size_t n = std::min(data.size(), head_limit_ - written_);
        write_chunk(stream, data.substr(0, n), ms);
        data.remove_prefix(n);
    }
    if (data.empty()) return;
    tail_.push_back({stream, std::string(data), ms});
    tail_bytes_ += data.size();
    while (tail_.size() > 1 && tail_bytes_ - tail_.front().data.size() >= tail_limit_) {
        tail_bytes_ -= tail_.front().data.size();
        dropped_ += tail_.front().data.size();
        tail_.pop_front();
    }
}

std::optional<nlohmann::json> FullOutputWriter::finish(const fs::path& base) {
    if (!opened_ || out_fd_ < 0 || idx_fd_ < 0) return std::nullopt;
    if (dropped_) {
        write_chunk('m', "\n... [" + std::to_string(dropped_) + " bytes dropped here: the output was over full_output_max_mb] ...\n",
                    tail_.empty() ? 0 : tail_.front().ms);
    }
    for (const auto& c : tail_) write_chunk(c.stream, c.data, c.ms);
    tail_.clear();
    close(out_fd_);
    close(idx_fd_);
    out_fd_ = idx_fd_ = -1;
    if (!error_.empty()) {
        std::error_code ec;
        fs::remove(out_, ec);
        fs::remove(index_of(out_), ec);
        return std::nullopt;
    }
    nlohmann::json j = {{"path", out_.lexically_relative(base).string()}, {"bytes", written_}, {"sha256", file_sha256(out_)}, {"delivered_to_model", false}};
    if (dropped_) j["dropped"] = dropped_;
    return j;
}

fs::path full_output_path(const fs::path& session_file, const std::string& call_id) {
    std::string name;
    for (char c : call_id) name += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    if (name.empty()) name = "call";
    fs::path dir = side_dir(session_file);
    std::error_code ec;
    fs::path p = dir / (name + ".out");
    for (int n = 2; fs::exists(p, ec); ++n) p = dir / (name + "-" + std::to_string(n) + ".out");
    return p;
}

fs::path resolve_full_output(const fs::path& session_file, const nlohmann::json& full_output) {
    if (!full_output.is_object() || !full_output.contains("path") || !full_output["path"].is_string()) return {};
    // Only ever <id>.d/<name>.out: a record cannot point a reader anywhere else.
    fs::path rel = full_output["path"].get<std::string>();
    std::vector<fs::path> parts(rel.begin(), rel.end());
    if (rel.is_absolute() || parts.size() != 2 || parts[0].extension() != ".d" || parts[1].extension() != ".out" || parts[0] == ".." || parts[1] == "..") return {};
    std::error_code ec;
    if (fs::is_regular_file(session_file.parent_path() / rel, ec)) return session_file.parent_path() / rel;
    if (auto owner = find_session(parts[0].stem().string()); owner && fs::is_regular_file(owner->path.parent_path() / rel, ec)) return owner->path.parent_path() / rel;
    return {};
}

std::vector<OutputChunk> read_output_index(const fs::path& out) {
    std::vector<OutputChunk> chunks;
    std::ifstream in(index_of(out));
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        OutputChunk c{};
        if (fields >> c.stream >> c.offset >> c.length >> c.ms) chunks.push_back(c);
    }
    std::error_code ec;
    if (!in.is_open()) {
        auto size = fs::file_size(out, ec);
        if (!ec && size) chunks.push_back({'o', 0, static_cast<size_t>(size), 0});
    }
    return chunks;
}

std::string full_output_screen(const fs::path& out, size_t max_bytes) {
    std::string data = read_file(out), screen;
    bool held_cr = false;  // a carriage return that ended a chunk: the next chunk says whether it was a CRLF
    auto restart_line = [&] {
        size_t nl = screen.rfind('\n');
        screen.erase(nl == std::string::npos ? 0 : nl + 1);
    };
    for (const auto& c : read_output_index(out)) {
        if (c.offset > data.size()) break;
        std::string_view chunk = std::string_view(data).substr(c.offset, c.length);
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (held_cr) {
                held_cr = false;
                if (chunk[i] != '\n') restart_line();
            }
            if (chunk[i] != '\r') screen += chunk[i];
            else if (i + 1 == chunk.size()) held_cr = true;
            else if (chunk[i + 1] != '\n') restart_line();
        }
    }
    if (screen.size() > max_bytes) {
        size_t cut = screen.size() - max_bytes;
        while (cut < screen.size() && (static_cast<unsigned char>(screen[cut]) & 0xC0) == 0x80) ++cut;
        screen = "[" + std::to_string(cut) + " bytes before this are left out here]\n" + screen.substr(cut);
    }
    return screen;
}

void replay_full_output(const fs::path& out, const std::function<void(char stream, std::string_view bytes)>& write, double speed) {
    std::string data = read_file(out);
    auto start = std::chrono::steady_clock::now();
    for (const auto& c : read_output_index(out)) {
        if (c.offset > data.size()) break;
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(c.ms / speed)));
        write(c.stream, std::string_view(data).substr(c.offset, c.length));
    }
}

}  // namespace maic
