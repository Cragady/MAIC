#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The end-to-end tunnel between a phone and the workstation, carried by maid-relay. The relay sees frames:
// len(4, big endian) | body. The first body each way is the hello, "MAID1" | static key(32) | ephemeral
// key(32); every later body is nonce(24) | ciphertext (XChaCha20-Poly1305). The nonce is a big-endian frame
// counter in its first 8 bytes and zero after, one counter per direction, and a receiver accepts only the
// next counter, so a replayed or reordered frame is refused. A body of length zero is a keepalive.
//
// Keys: each side has a long-term X25519 key (the phone's is enrolled by `maid server pair`), each
// connection adds an ephemeral one, and the two directional session keys come from HKDF-SHA256 over the
// three Diffie-Hellman results (ephemeral-ephemeral, phone ephemeral with home static, home ephemeral with
// phone static), salted with "maid-tunnel-v1" and bound to all four public keys.
namespace maid::server {

using Key32 = std::array<unsigned char, 32>;

struct KeyPair {
    Key32 pk;
    Key32 sk;
};

KeyPair make_keypair();

// URL-safe base64 without padding, the form keys take in files, JSON and the pairing bundle.
std::string b64url(const unsigned char* data, size_t len);
inline std::string b64url(const Key32& k) { return b64url(k.data(), k.size()); }
std::optional<std::vector<unsigned char>> unb64url(std::string_view text);
std::optional<Key32> key_from_b64url(std::string_view text);

// RFC 5869 with SHA-256, through libsodium; exposed for the known-answer test.
std::string hkdf_sha256(std::string_view salt, std::string_view ikm, std::string_view info, size_t len);

struct SessionKeys {
    Key32 to_home;   // what the phone seals with and the home opens
    Key32 to_phone;  // the other way
};
SessionKeys derive_session(const KeyPair& my_static, const KeyPair& my_ephemeral, const Key32& their_static, const Key32& their_ephemeral, bool i_am_home);

constexpr size_t kNonceBytes = 24;
constexpr size_t kTagBytes = 16;
constexpr size_t kMaxFrame = 1u << 20;

std::string hello_body(const Key32& static_pk, const Key32& ephemeral_pk);
bool parse_hello(std::string_view body, Key32& static_pk, Key32& ephemeral_pk);

std::string frame(std::string_view body);
// Takes one whole frame off the front of `buffer` into `body`; false when none is complete yet. Throws on a
// frame longer than kMaxFrame.
bool next_frame(std::string& buffer, std::string& body);

class Sealer {
public:
    explicit Sealer(const Key32& key) : key_(key) {}
    std::string seal(std::string_view plain);  // nonce | ciphertext
    uint64_t counter() const { return counter_; }

private:
    Key32 key_;
    uint64_t counter_ = 0;
};

class Opener {
public:
    explicit Opener(const Key32& key) : key_(key) {}
    // False for a bad tag, a short body, or any counter but the one expected next.
    bool open(std::string_view body, std::string& plain);

private:
    Key32 key_;
    uint64_t expected_ = 0;
};

// Inside the tunnel: kind(1) | stream(4, big endian) | payload. A Request payload is a JSON head
// {method, path, headers} then a newline then the body; Head is JSON {status, headers}; Data is body bytes;
// End closes a response, or from the phone cancels a request; Error carries text; Ready is the home's
// first message after the hello.
enum class Kind : unsigned char { Ready = 0, Request = 1, Head = 2, Data = 3, End = 4, Error = 5 };

std::string message(Kind kind, uint32_t stream, std::string_view payload);
bool parse_message(std::string_view plain, Kind& kind, uint32_t& stream, std::string_view& payload);

// This workstation's tunnel identity and the phones paired with it: <state>/server/pairs.json, 0600.
struct PhoneInfo {
    std::string name;
    std::string public_key;  // base64url
    std::string created;
};

class PairStore {
public:
    explicit PairStore(std::filesystem::path file);

    // Re-reads the file when another process changed it (a pairing while the server runs, `maid server unpair`).
    void refresh();
    // The pairing id and key pair, made and written on first use.
    const std::string& pairing_id();
    const KeyPair& keypair();
    // Enrols a phone; a phone of the same name is replaced.
    void add(const std::string& name, const Key32& public_key);
    bool remove(const std::string& name);
    std::vector<PhoneInfo> list() const { return phones_; }
    std::optional<PhoneInfo> find(const Key32& public_key) const;

private:
    void load();
    void save();
    void ensure_identity();
    std::filesystem::path file_;
    std::filesystem::file_time_type loaded_{};
    std::string id_;
    std::optional<KeyPair> keys_;
    std::vector<PhoneInfo> phones_;
};

// The one-time code `maid server pair` leaves for the running server in <state>/server/pairing.json: its
// SHA-256, an expiry two minutes out, and how many wrong guesses it has taken. Three wrong guesses void it.
std::string new_pairing_code();  // 8 digits
void write_pairing_offer(const std::filesystem::path& file, const std::string& code, int seconds = 120);
// Consumes the offer when the code matches; otherwise the reason it did not.
std::optional<std::string> claim_pairing_offer(const std::filesystem::path& file, const std::string& code);

}  // namespace maid::server
