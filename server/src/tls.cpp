#include "tls.hpp"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace maid::server {

namespace fs = std::filesystem;

namespace {

std::string fingerprint_of(X509* x) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!X509_digest(x, EVP_sha256(), md, &n)) throw std::runtime_error("can't digest the certificate");
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) {
        if (i) out += ':';
        out += hex[md[i] >> 4];
        out += hex[md[i] & 15];
    }
    return out;
}

bool is_ip(const std::string& host) {
    in_addr v4;
    in6_addr v6;
    return inet_pton(AF_INET, host.c_str(), &v4) == 1 || inet_pton(AF_INET6, host.c_str(), &v6) == 1;
}

// fopen with the file created 0600, so the private key is never readable by others even for a moment.
FILE* open_private(const fs::path& p) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("can't write " + p.string() + ": " + std::strerror(errno));
    return fdopen(fd, "w");
}

}  // namespace

std::string cert_fingerprint(const fs::path& cert) {
    std::unique_ptr<FILE, int (*)(FILE*)> fp(fopen(cert.c_str(), "r"), fclose);
    if (!fp) throw std::runtime_error("can't read " + cert.string());
    std::unique_ptr<X509, void (*)(X509*)> x(PEM_read_X509(fp.get(), nullptr, nullptr, nullptr), X509_free);
    if (!x) throw std::runtime_error(cert.string() + " is not a PEM certificate");
    return fingerprint_of(x.get());
}

TlsPair ensure_self_signed(const fs::path& cert, const fs::path& key, const std::vector<std::string>& hosts) {
    if (fs::exists(cert) && fs::exists(key)) return {cert, key, cert_fingerprint(cert)};
    fs::create_directories(cert.parent_path());

    std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)> pkey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free);
    if (!pkey) throw std::runtime_error("OpenSSL could not generate a key");
    std::unique_ptr<X509, void (*)(X509*)> x(X509_new(), X509_free);
    X509_set_version(x.get(), X509_VERSION_3);
    unsigned char serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7f;  // a positive serial number
    BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x.get()));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x.get()), -600);  // ten minutes of clock skew between the phone and here
    X509_gmtime_adj(X509_getm_notAfter(x.get()), 10L * 365 * 24 * 3600);
    X509_set_pubkey(x.get(), pkey.get());
    X509_NAME* name = X509_get_subject_name(x.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("maid-server"), -1, -1, 0);
    X509_set_issuer_name(x.get(), name);

    std::string san;
    for (const auto& h : hosts) san += (san.empty() ? "" : ",") + std::string(is_ip(h) ? "IP:" : "DNS:") + h;
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x.get(), x.get(), nullptr, nullptr, 0);
    for (const auto& [nid, value] : std::vector<std::pair<int, std::string>>{
             {NID_subject_alt_name, san}, {NID_basic_constraints, "critical,CA:FALSE"}, {NID_ext_key_usage, "serverAuth"}}) {
        if (value.empty()) continue;
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
        if (!ex) throw std::runtime_error("bad certificate extension: " + value);
        X509_add_ext(x.get(), ex, -1);
        X509_EXTENSION_free(ex);
    }
    if (!X509_sign(x.get(), pkey.get(), EVP_sha256())) throw std::runtime_error("can't sign the certificate");

    std::unique_ptr<FILE, int (*)(FILE*)> kf(open_private(key), fclose);
    if (!PEM_write_PrivateKey(kf.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr)) throw std::runtime_error("can't write " + key.string());
    kf.reset();
    std::unique_ptr<FILE, int (*)(FILE*)> cf(fopen(cert.c_str(), "w"), fclose);
    if (!cf || !PEM_write_X509(cf.get(), x.get())) throw std::runtime_error("can't write " + cert.string());
    cf.reset();
    return {cert, key, fingerprint_of(x.get())};
}

std::vector<std::string> interface_addresses() {
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        std::string a = buf;
        if (a.rfind("127.", 0) == 0) continue;
        out.push_back(a);
    }
    freeifaddrs(list);
    return out;
}

}  // namespace maid::server
