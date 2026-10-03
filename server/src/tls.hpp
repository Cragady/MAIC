#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace maid::server {

struct TlsPair {
    std::filesystem::path cert;
    std::filesystem::path key;
    std::string fingerprint;  // SHA-256 of the certificate, "AB:CD:...", what a client pins
};

// A self-signed P-256 certificate for `hosts` (IP addresses or names), valid ten years, written to cert/key
// when they don't both exist yet. The key file is 0600. Returns the pair with its fingerprint.
TlsPair ensure_self_signed(const std::filesystem::path& cert, const std::filesystem::path& key, const std::vector<std::string>& hosts);

std::string cert_fingerprint(const std::filesystem::path& cert);

// IPv4 addresses of this machine's interfaces, loopback left out: what goes in the certificate when the
// server listens on every interface.
std::vector<std::string> interface_addresses();

}  // namespace maid::server
