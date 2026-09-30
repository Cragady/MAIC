#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace maic::server {

std::string sha256_hex(const std::string& data);

struct TokenInfo {
    std::string name;
    std::string created;  // ISO 8601, UTC
};

// Per-device bearer tokens. Only SHA-256 hashes are kept on disk (0600 in a 0700 directory); the token itself is
// printed once by `maic server token new` and never stored.
class TokenStore {
public:
    explicit TokenStore(std::filesystem::path file);

    // Returns the new token. Names are unique; revoke first to replace one.
    std::string create(const std::string& name);
    bool revoke(const std::string& name);
    std::vector<TokenInfo> list() const;
    bool empty() const { return entries_.empty(); }
    // Re-reads the file when another process (`maic server token new` while the server runs) changed it.
    void refresh();

    // The token's name when its hash is on file. Every entry is compared in constant time, none is skipped.
    std::optional<std::string> verify(const std::string& token) const;

private:
    struct Entry {
        std::string name;
        std::string hash;
        std::string created;
    };
    void load();
    void save() const;
    std::filesystem::path file_;
    std::filesystem::file_time_type loaded_{};
    std::vector<Entry> entries_;
};

// Failed authentications per source address: after `limit` failures inside `window` the source is refused
// until the window has passed since its first failure. A success clears the count.
class RateLimit {
public:
    explicit RateLimit(int limit = 5, std::chrono::seconds window = std::chrono::seconds(60)) : limit_(limit), window_(window) {}
    bool blocked(const std::string& source);
    void failed(const std::string& source);
    void succeeded(const std::string& source);

private:
    struct Failures {
        int count = 0;
        std::chrono::steady_clock::time_point first;
    };
    std::mutex mu_;
    std::map<std::string, Failures> failures_;
    int limit_;
    std::chrono::seconds window_;
};

}  // namespace maic::server
