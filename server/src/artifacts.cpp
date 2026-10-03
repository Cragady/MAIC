#include "artifacts.hpp"

#include "auth.hpp"
#include "tunnel.hpp"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace maic::server {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr auto kGrantLife = std::chrono::hours(12);
constexpr size_t kGrantsMax = 4096;

bool safe_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// One path component of an artifact's file: a safe name that may hold dots, never first.
bool component_ok(std::string_view c) {
    if (c.empty() || c.size() > 128 || c[0] == '.') return false;
    return std::all_of(c.begin(), c.end(), [](char ch) { return safe_char(ch) || ch == '.'; });
}

std::string random_secret() {
    unsigned char raw[24];
    if (RAND_bytes(raw, sizeof(raw)) != 1) throw std::runtime_error("OpenSSL could not produce random bytes");
    return b64url(raw, sizeof(raw));
}

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

void write_0600(const fs::path& file, const std::string& body) {
    int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("can't write " + file.string() + ": " + std::strerror(errno));
    ssize_t n = write(fd, body.data(), body.size());
    close(fd);
    if (n != static_cast<ssize_t>(body.size())) throw std::runtime_error("short write to " + file.string());
}

}  // namespace

bool artifact_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 64 || name[0] == '_' || name[0] == '-') return false;
    return std::all_of(name.begin(), name.end(), safe_char);
}

std::optional<fs::path> artifact_file(const fs::path& dir, std::string_view rel) {
    if (rel.empty()) rel = "index.html";
    fs::path p = dir;
    for (size_t start = 0;;) {
        size_t slash = rel.find('/', start);
        std::string_view c = rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!component_ok(c)) return std::nullopt;
        p /= std::string(c);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    std::error_code ec;
    fs::path base = fs::canonical(dir, ec);
    if (ec) return std::nullopt;
    fs::path real = fs::canonical(p, ec);
    if (ec || !fs::is_regular_file(real, ec)) return std::nullopt;
    auto [b, r] = std::mismatch(base.begin(), base.end(), real.begin(), real.end());
    if (b != base.end()) return std::nullopt;
    // A symlink may point elsewhere inside the folder, but not at a dotfile there.
    for (; r != real.end(); ++r) {
        if (!component_ok(r->string())) return std::nullopt;
    }
    return real;
}

std::string artifact_content_type(const fs::path& file) {
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},  {".htm", "text/html; charset=utf-8"},  {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"}, {".json", "application/json"},
        {".map", "application/json"},           {".txt", "text/plain; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"},
        {".svg", "image/svg+xml"},              {".png", "image/png"},                 {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},                {".gif", "image/gif"},                 {".webp", "image/webp"},
        {".ico", "image/x-icon"},               {".woff", "font/woff"},                {".woff2", "font/woff2"},
        {".mp3", "audio/mpeg"},                 {".ogg", "audio/ogg"},                 {".wav", "audio/wav"},
        {".mp4", "video/mp4"},                  {".webm", "video/webm"},               {".pdf", "application/pdf"},
        {".wasm", "application/wasm"}};
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

std::string data_rev(const std::string& body) {
    return sha256_hex(body).substr(0, 16);
}

namespace {

fs::path data_folder(const fs::path& dir) {
    fs::path data = dir / "data";
    std::error_code ec;
    auto st = fs::symlink_status(data, ec);
    if (fs::exists(st) && !fs::is_directory(st)) throw std::runtime_error("the artifact's data/ is not a plain folder");
    return data;
}

}  // namespace

std::optional<std::string> data_revision(const fs::path& dir, const std::string& name) {
    fs::path file = data_folder(dir) / (name + ".json");
    std::error_code ec;
    auto st = fs::symlink_status(file, ec);
    if (!fs::exists(st)) return std::nullopt;
    if (!fs::is_regular_file(st)) throw std::runtime_error("data/" + name + ".json is not a plain file");
    std::ifstream in(file, std::ios::binary);
    return data_rev(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

void write_data(const fs::path& dir, const std::string& name, const std::string& body) {
    fs::path data = data_folder(dir);
    fs::create_directories(data);
    fs::path tmp = data / ("." + name + ".json.tmp");
    write_0600(tmp, body);
    fs::rename(tmp, data / (name + ".json"));
}

std::string page_with_token(std::string html, const std::string& token) {
    std::string lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    size_t at = 0;
    for (size_t p = lower.find("<head"); p != std::string::npos; p = lower.find("<head", p + 5)) {
        if (p + 5 < lower.size() && (lower[p + 5] == '>' || std::isspace(static_cast<unsigned char>(lower[p + 5])))) {
            at = lower.find('>', p) + 1;
            break;
        }
    }
    // No <head>: after the doctype, so the page keeps its standards mode.
    if (at == 0 && lower.rfind("<!doctype", 0) == 0) at = lower.find('>') + 1;
    html.insert(at, "<meta name=\"maic-artifact-token\" content=\"" + token + "\">");
    return html;
}

std::string artifact_trust(const fs::path& dir) {
    std::ifstream in(dir / ".maic-artifact.json");
    json j = in ? json::parse(in, nullptr, false) : json();
    return j.is_object() && j.value("trust", json()) == "trusted" ? "trusted" : "sandboxed";
}

std::vector<ArtifactInfo> list_artifacts(const fs::path& root) {
    std::vector<ArtifactInfo> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        std::string id = e.path().filename().string();
        if (!artifact_name_ok(id) || !e.is_directory(ec)) continue;
        std::ifstream in(e.path() / ".maic-artifact.json");
        json meta = in ? json::parse(in, nullptr, false) : json();
        ArtifactInfo a{id, artifact_trust(e.path()), meta.is_object() ? meta.value("added", "") : "", {}};
        for (const auto& d : fs::directory_iterator(e.path() / "data", ec)) {
            std::string name = d.path().stem().string();
            if (d.path().extension() == ".json" && artifact_name_ok(name) && d.is_regular_file(ec)) a.data.push_back(name);
        }
        std::sort(a.data.begin(), a.data.end());
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const ArtifactInfo& x, const ArtifactInfo& y) { return x.id < y.id; });
    return out;
}

std::vector<std::string> add_artifact(const fs::path& root, const fs::path& src, const std::string& id) {
    if (!artifact_name_ok(id)) throw std::runtime_error("an artifact id is 1 to 64 letters, digits, '_' or '-', not starting with '_' or '-': " + id);
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(src / "index.html", ec))) throw std::runtime_error(src.string() + " has no index.html (a plain file) at its top");
    fs::path dest = root / id;
    if (fs::equivalent(src, dest, ec)) throw std::runtime_error(src.string() + " is the artifact " + id + " itself");
    fs::create_directories(dest);
    std::vector<std::string> skipped;
    for (auto it = fs::recursive_directory_iterator(src); it != fs::recursive_directory_iterator(); ++it) {
        fs::path rel = it->path().lexically_relative(src);
        std::string name = it->path().filename().string();
        if (name[0] == '.') {
            if (it->is_directory(ec) && !it->is_symlink(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink(ec) || !component_ok(name)) {
            skipped.push_back(rel.string() + (it->is_symlink(ec) ? " (a symlink)" : " (its name is not letters, digits, '.', '_' or '-')"));
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            fs::create_directories(dest / rel);
        } else if (it->is_regular_file(ec)) {
            bool data = *rel.begin() == "data";
            if (data && fs::exists(dest / rel, ec)) continue;
            fs::copy_file(it->path(), dest / rel, fs::copy_options::overwrite_existing);
        }
    }
    if (!fs::exists(dest / ".maic-artifact.json", ec)) {
        json meta = {{"trust", "sandboxed"}, {"added", utc_now()}, {"source", fs::absolute(src).lexically_normal().string()}};
        write_0600(dest / ".maic-artifact.json", meta.dump(2) + "\n");
    }
    return skipped;
}

std::string new_artifact_login(const fs::path& state, int seconds) {
    fs::path dir = state / "artifact-logins";
    fs::create_directories(dir);
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    std::string code = random_secret();
    write_0600(dir / sha256_hex(code), std::to_string(static_cast<long long>(std::time(nullptr)) + seconds) + "\n");
    return code;
}

bool claim_artifact_login(const fs::path& state, const std::string& code) {
    fs::path dir = state / "artifact-logins";
    long long now = std::time(nullptr);
    auto expires = [](const fs::path& f) {
        long long t = 0;
        std::ifstream in(f);
        in >> t;
        return t;
    };
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (expires(e.path()) < now) fs::remove(e.path(), ec);
    }
    if (code.size() != 32 || !std::all_of(code.begin(), code.end(), safe_char)) return false;
    fs::path file = dir / sha256_hex(code);
    long long until = expires(file);
    // Removing it is the claim: of two requests with one code, only one removes the file.
    return fs::remove(file, ec) && until >= now;
}

std::string ArtifactGrants::login() {
    return mint("");
}

bool ArtifactGrants::logged_in(const std::string& secret) {
    return valid(secret, "");
}

std::string ArtifactGrants::grant(const std::string& artifact) {
    return mint(artifact);
}

bool ArtifactGrants::granted(const std::string& secret, const std::string& artifact) {
    return !artifact.empty() && valid(secret, artifact);
}

std::string ArtifactGrants::mint(const std::string& artifact) {
    std::string secret = random_secret();
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mu_);
    std::erase_if(grants_, [&](const auto& g) { return g.second.expires < now; });
    if (grants_.size() >= kGrantsMax) {
        grants_.erase(std::min_element(grants_.begin(), grants_.end(), [](const auto& a, const auto& b) { return a.second.expires < b.second.expires; }));
    }
    grants_[sha256_hex(secret)] = {artifact, now + kGrantLife};
    return secret;
}

bool ArtifactGrants::valid(const std::string& secret, const std::string& artifact) {
    if (secret.empty()) return false;
    std::lock_guard lock(mu_);
    auto it = grants_.find(sha256_hex(secret));
    return it != grants_.end() && it->second.expires >= std::chrono::steady_clock::now() && it->second.artifact == artifact;
}

}  // namespace maic::server
