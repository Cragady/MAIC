#include "auth.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace maic::server {

namespace fs = std::filesystem;
using nlohmann::json;

std::string sha256_hex(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 15];
    }
    return out;
}

namespace {

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// URL-safe base64 without padding: 24 random bytes become 32 characters a phone can type.
std::string base64url(const unsigned char* data, size_t len) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (i + 1 < len) out += alphabet[(v >> 6) & 63];
        if (i + 2 < len) out += alphabet[v & 63];
    }
    return out;
}

}  // namespace

TokenStore::TokenStore(fs::path file) : file_(std::move(file)) {
    load();
}

void TokenStore::load() {
    entries_.clear();
    std::error_code ec;
    loaded_ = fs::last_write_time(file_, ec);
    std::ifstream in(file_);
    if (!in) return;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) throw std::runtime_error(file_.string() + " is not valid JSON");
    for (const auto& t : j.value("tokens", json::array())) {
        entries_.push_back({t.value("name", ""), t.value("sha256", ""), t.value("created", "")});
    }
}

void TokenStore::save() const {
    fs::create_directories(file_.parent_path());
    std::error_code ec;
    fs::permissions(file_.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
    json tokens = json::array();
    for (const auto& e : entries_) tokens.push_back({{"name", e.name}, {"sha256", e.hash}, {"created", e.created}});
    // Written next to the file and renamed over it, so a crash never leaves a half-written token list.
    fs::path tmp = file_.string() + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("can't write " + tmp.string() + ": " + std::strerror(errno));
    std::string body = json{{"tokens", tokens}}.dump(2) + "\n";
    ssize_t n = write(fd, body.data(), body.size());
    close(fd);
    if (n != static_cast<ssize_t>(body.size())) throw std::runtime_error("short write to " + tmp.string());
    fs::rename(tmp, file_);
}

void TokenStore::refresh() {
    std::error_code ec;
    if (fs::last_write_time(file_, ec) != loaded_) load();
}

std::string TokenStore::create(const std::string& name) {
    if (name.empty()) throw std::runtime_error("a token needs a name (the device it is for)");
    for (const auto& e : entries_) {
        if (e.name == name) throw std::runtime_error("a token named '" + name + "' exists; revoke it first");
    }
    unsigned char raw[24];
    if (RAND_bytes(raw, sizeof(raw)) != 1) throw std::runtime_error("OpenSSL could not produce random bytes");
    std::string token = base64url(raw, sizeof(raw));
    entries_.push_back({name, sha256_hex(token), utc_now()});
    save();
    return token;
}

bool TokenStore::revoke(const std::string& name) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->name == name) {
            entries_.erase(it);
            save();
            return true;
        }
    }
    return false;
}

std::vector<TokenInfo> TokenStore::list() const {
    std::vector<TokenInfo> out;
    for (const auto& e : entries_) out.push_back({e.name, e.created});
    return out;
}

std::optional<std::string> TokenStore::verify(const std::string& token) const {
    if (token.empty()) return std::nullopt;
    std::string hash = sha256_hex(token);
    const Entry* match = nullptr;
    for (const auto& e : entries_) {
        if (e.hash.size() == hash.size() && CRYPTO_memcmp(e.hash.data(), hash.data(), hash.size()) == 0) match = &e;
    }
    if (!match) return std::nullopt;
    return match->name;
}

bool RateLimit::blocked(const std::string& source) {
    std::lock_guard lock(mu_);
    auto it = failures_.find(source);
    if (it == failures_.end()) return false;
    if (std::chrono::steady_clock::now() - it->second.first > window_) {
        failures_.erase(it);
        return false;
    }
    return it->second.count >= limit_;
}

void RateLimit::failed(const std::string& source) {
    std::lock_guard lock(mu_);
    auto now = std::chrono::steady_clock::now();
    auto& f = failures_[source];
    if (f.count == 0 || now - f.first > window_) f = {0, now};
    ++f.count;
}

void RateLimit::succeeded(const std::string& source) {
    std::lock_guard lock(mu_);
    failures_.erase(source);
}

}  // namespace maic::server
