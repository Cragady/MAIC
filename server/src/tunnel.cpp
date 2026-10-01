#include "tunnel.hpp"

#include "auth.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace maic::server {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

void init_sodium() {
    static const int rc = sodium_init();
    if (rc < 0) throw std::runtime_error("libsodium could not initialise");
}

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

void put_u32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

uint32_t get_u32(const unsigned char* b) {
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}

std::array<unsigned char, kNonceBytes> nonce_for(uint64_t counter) {
    std::array<unsigned char, kNonceBytes> n{};
    for (int i = 0; i < 8; ++i) n[i] = static_cast<unsigned char>(counter >> (56 - 8 * i));
    return n;
}

Key32 scalarmult(const Key32& sk, const Key32& pk) {
    Key32 out;
    if (crypto_scalarmult(out.data(), sk.data(), pk.data()) != 0) throw std::runtime_error("the other side's key is not a valid X25519 point");
    return out;
}

// Written next to the file and renamed over it, 0600, like tokens.json.
void write_private(const fs::path& file, const std::string& body) {
    fs::create_directories(file.parent_path());
    std::error_code ec;
    fs::permissions(file.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::path tmp = file.string() + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("can't write " + tmp.string() + ": " + std::strerror(errno));
    ssize_t n = write(fd, body.data(), body.size());
    close(fd);
    if (n != static_cast<ssize_t>(body.size())) throw std::runtime_error("short write to " + tmp.string());
    fs::rename(tmp, file);
}

}  // namespace

KeyPair make_keypair() {
    init_sodium();
    KeyPair k;
    randombytes_buf(k.sk.data(), k.sk.size());
    crypto_scalarmult_base(k.pk.data(), k.sk.data());
    return k;
}

std::string b64url(const unsigned char* data, size_t len) {
    std::string out(sodium_base64_encoded_len(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::optional<std::vector<unsigned char>> unb64url(std::string_view text) {
    std::vector<unsigned char> out(text.size() / 4 * 3 + 3);
    size_t n = 0;
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &n, nullptr, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) return std::nullopt;
    out.resize(n);
    return out;
}

std::optional<Key32> key_from_b64url(std::string_view text) {
    auto bytes = unb64url(text);
    if (!bytes || bytes->size() != 32) return std::nullopt;
    Key32 k;
    std::copy(bytes->begin(), bytes->end(), k.begin());
    return k;
}

std::string hkdf_sha256(std::string_view salt, std::string_view ikm, std::string_view info, size_t len) {
    init_sodium();
    unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(salt.data()), salt.size(), reinterpret_cast<const unsigned char*>(ikm.data()), ikm.size());
    std::string out(len, '\0');
    if (crypto_kdf_hkdf_sha256_expand(reinterpret_cast<unsigned char*>(out.data()), len, info.data(), info.size(), prk) != 0) throw std::runtime_error("HKDF output too long");
    sodium_memzero(prk, sizeof(prk));
    return out;
}

SessionKeys derive_session(const KeyPair& my_static, const KeyPair& my_ephemeral, const Key32& their_static, const Key32& their_ephemeral, bool i_am_home) {
    Key32 ee = scalarmult(my_ephemeral.sk, their_ephemeral);
    // Phone ephemeral with home static, then home ephemeral with phone static, from whichever end this is.
    Key32 ph = i_am_home ? scalarmult(my_static.sk, their_ephemeral) : scalarmult(my_ephemeral.sk, their_static);
    Key32 hp = i_am_home ? scalarmult(my_ephemeral.sk, their_static) : scalarmult(my_static.sk, their_ephemeral);
    std::string ikm;
    for (const Key32* k : {&ee, &ph, &hp}) ikm.append(reinterpret_cast<const char*>(k->data()), k->size());
    const Key32& phone_static = i_am_home ? their_static : my_static.pk;
    const Key32& phone_eph = i_am_home ? their_ephemeral : my_ephemeral.pk;
    const Key32& home_static = i_am_home ? my_static.pk : their_static;
    const Key32& home_eph = i_am_home ? my_ephemeral.pk : their_ephemeral;
    std::string info;
    for (const Key32* k : {&phone_static, &phone_eph, &home_static, &home_eph}) info.append(reinterpret_cast<const char*>(k->data()), k->size());
    std::string okm = hkdf_sha256("maic-tunnel-v1", ikm, info, 64);
    SessionKeys keys;
    std::memcpy(keys.to_home.data(), okm.data(), 32);
    std::memcpy(keys.to_phone.data(), okm.data() + 32, 32);
    sodium_memzero(ikm.data(), ikm.size());
    sodium_memzero(okm.data(), okm.size());
    return keys;
}

std::string hello_body(const Key32& static_pk, const Key32& ephemeral_pk) {
    std::string out = "MAIC1";
    out.append(reinterpret_cast<const char*>(static_pk.data()), 32);
    out.append(reinterpret_cast<const char*>(ephemeral_pk.data()), 32);
    return out;
}

bool parse_hello(std::string_view body, Key32& static_pk, Key32& ephemeral_pk) {
    if (body.size() != 69 || body.substr(0, 5) != "MAIC1") return false;
    std::memcpy(static_pk.data(), body.data() + 5, 32);
    std::memcpy(ephemeral_pk.data(), body.data() + 37, 32);
    return true;
}

std::string frame(std::string_view body) {
    std::string out;
    put_u32(out, static_cast<uint32_t>(body.size()));
    out.append(body);
    return out;
}

bool next_frame(std::string& buffer, std::string& body) {
    if (buffer.size() < 4) return false;
    size_t n = get_u32(reinterpret_cast<const unsigned char*>(buffer.data()));
    if (n > kMaxFrame) throw std::runtime_error("tunnel frame of " + std::to_string(n) + " bytes exceeds the limit");
    if (buffer.size() < 4 + n) return false;
    body = buffer.substr(4, n);
    buffer.erase(0, 4 + n);
    return true;
}

std::string Sealer::seal(std::string_view plain) {
    init_sodium();
    auto nonce = nonce_for(counter_++);
    std::string out(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    out.resize(kNonceBytes + plain.size() + kTagBytes);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(out.data() + kNonceBytes), &clen, reinterpret_cast<const unsigned char*>(plain.data()), plain.size(),
                                               nullptr, 0, nullptr, nonce.data(), key_.data());
    out.resize(kNonceBytes + clen);
    return out;
}

bool Opener::open(std::string_view body, std::string& plain) {
    init_sodium();
    if (body.size() < kNonceBytes + kTagBytes) return false;
    auto expected = nonce_for(expected_);
    if (std::memcmp(body.data(), expected.data(), kNonceBytes) != 0) return false;
    plain.resize(body.size() - kNonceBytes - kTagBytes);
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr, reinterpret_cast<const unsigned char*>(body.data() + kNonceBytes),
                                                   body.size() - kNonceBytes, nullptr, 0, expected.data(), key_.data()) != 0) {
        plain.clear();
        return false;
    }
    plain.resize(mlen);
    ++expected_;
    return true;
}

std::string message(Kind kind, uint32_t stream, std::string_view payload) {
    std::string out(1, static_cast<char>(kind));
    put_u32(out, stream);
    out.append(payload);
    return out;
}

bool parse_message(std::string_view plain, Kind& kind, uint32_t& stream, std::string_view& payload) {
    if (plain.size() < 5) return false;
    unsigned char k = static_cast<unsigned char>(plain[0]);
    if (k > static_cast<unsigned char>(Kind::Error)) return false;
    kind = static_cast<Kind>(k);
    stream = get_u32(reinterpret_cast<const unsigned char*>(plain.data() + 1));
    payload = plain.substr(5);
    return true;
}

PairStore::PairStore(fs::path file) : file_(std::move(file)) {
    load();
}

void PairStore::load() {
    phones_.clear();
    id_.clear();
    keys_.reset();
    std::error_code ec;
    loaded_ = fs::last_write_time(file_, ec);
    std::ifstream in(file_);
    if (!in) return;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) throw std::runtime_error(file_.string() + " is not valid JSON");
    id_ = j.value("pairing_id", "");
    auto pk = key_from_b64url(j.value("public_key", "")), sk = key_from_b64url(j.value("secret_key", ""));
    if (pk && sk) keys_ = KeyPair{*pk, *sk};
    for (const auto& p : j.value("phones", json::array())) phones_.push_back({p.value("name", ""), p.value("public_key", ""), p.value("created", "")});
}

void PairStore::save() {
    json phones = json::array();
    for (const auto& p : phones_) phones.push_back({{"name", p.name}, {"public_key", p.public_key}, {"created", p.created}});
    json j = {{"pairing_id", id_}, {"public_key", b64url(keys_->pk)}, {"secret_key", b64url(keys_->sk)}, {"phones", phones}};
    write_private(file_, j.dump(2) + "\n");
    std::error_code ec;
    loaded_ = fs::last_write_time(file_, ec);
}

void PairStore::refresh() {
    std::error_code ec;
    if (fs::last_write_time(file_, ec) != loaded_) load();
}

void PairStore::ensure_identity() {
    if (!id_.empty() && keys_) return;
    init_sodium();
    unsigned char raw[16];
    randombytes_buf(raw, sizeof(raw));
    id_ = b64url(raw, sizeof(raw));
    keys_ = make_keypair();
    save();
}

const std::string& PairStore::pairing_id() {
    ensure_identity();
    return id_;
}

const KeyPair& PairStore::keypair() {
    ensure_identity();
    return *keys_;
}

void PairStore::add(const std::string& name, const Key32& public_key) {
    if (name.empty()) throw std::runtime_error("a phone needs a name");
    ensure_identity();
    std::erase_if(phones_, [&](const PhoneInfo& p) { return p.name == name; });
    phones_.push_back({name, b64url(public_key), utc_now()});
    save();
}

bool PairStore::remove(const std::string& name) {
    for (auto it = phones_.begin(); it != phones_.end(); ++it) {
        if (it->name == name) {
            phones_.erase(it);
            save();
            return true;
        }
    }
    return false;
}

std::optional<PhoneInfo> PairStore::find(const Key32& public_key) const {
    std::string wanted = b64url(public_key);
    for (const auto& p : phones_) {
        if (p.public_key.size() == wanted.size() && sodium_memcmp(p.public_key.data(), wanted.data(), wanted.size()) == 0) return p;
    }
    return std::nullopt;
}

std::string new_pairing_code() {
    init_sodium();
    std::string code;
    for (int i = 0; i < 8; ++i) code += static_cast<char>('0' + randombytes_uniform(10));
    return code;
}

void write_pairing_offer(const fs::path& file, const std::string& code, int seconds) {
    json j = {{"code_sha256", sha256_hex(code)}, {"expires", static_cast<long long>(std::time(nullptr)) + seconds}, {"attempts", 0}};
    write_private(file, j.dump() + "\n");
}

std::optional<std::string> claim_pairing_offer(const fs::path& file, const std::string& code) {
    std::ifstream in(file);
    if (!in) return "no pairing is offered; run `maic server pair` at the workstation";
    json j = json::parse(in, nullptr, false);
    in.close();
    std::error_code ec;
    if (!j.is_object() || j.value("expires", 0LL) < static_cast<long long>(std::time(nullptr))) {
        fs::remove(file, ec);
        return "the pairing code has expired; run `maic server pair` again";
    }
    std::string hash = sha256_hex(code), stored = j.value("code_sha256", "");
    if (hash.size() == stored.size() && sodium_memcmp(hash.data(), stored.data(), hash.size()) == 0) {
        fs::remove(file, ec);
        return std::nullopt;
    }
    int attempts = j.value("attempts", 0) + 1;
    if (attempts >= 3) {
        fs::remove(file, ec);
        return "wrong code three times; the offer is void, run `maic server pair` again";
    }
    j["attempts"] = attempts;
    write_private(file, j.dump() + "\n");
    return "wrong code (" + std::to_string(3 - attempts) + " more " + (attempts == 2 ? "try" : "tries") + ")";
}

}  // namespace maic::server
