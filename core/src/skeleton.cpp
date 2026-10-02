#include "maic/skeleton.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <vector>

namespace maic {

using json = nlohmann::json;

namespace {

void put_string(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// ECMAScript's Number.prototype.toString over the shortest round-trip digits.
void put_double(std::string& out, double x) {
    if (!std::isfinite(x)) throw std::runtime_error("RFC 8785 has no form for " + std::to_string(x));
    if (x == 0) {
        out += '0';
        return;
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool negative = sci[0] == '-';
    if (negative) sci.erase(0, 1);
    size_t e = sci.find('e');
    std::string digits = sci.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    int n = std::stoi(sci.substr(e + 1)) + 1;  // where the decimal point falls after the first digit
    int k = static_cast<int>(digits.size());
    if (negative) out += '-';
    if (k <= n && n <= 21) {
        out += digits + std::string(n - k, '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, n) + "." + digits.substr(n);
    } else if (-6 < n && n <= 0) {
        out += "0." + std::string(-n, '0') + digits;
    } else {
        out += digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += "e";
        out += n - 1 >= 0 ? "+" : "-";
        out += std::to_string(std::abs(n - 1));
    }
}

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
        for (int j = 1; j < len && i + j < s.size(); ++j) cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3f);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xd800 + (cp >> 10));
            out += static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

void put(std::string& out, const json& v) {
    switch (v.type()) {
        case json::value_t::null: out += "null"; break;
        case json::value_t::boolean: out += v.get<bool>() ? "true" : "false"; break;
        case json::value_t::number_integer: out += std::to_string(v.get<long long>()); break;
        case json::value_t::number_unsigned: out += std::to_string(v.get<unsigned long long>()); break;
        case json::value_t::number_float: put_double(out, v.get<double>()); break;
        case json::value_t::string: put_string(out, v.get_ref<const std::string&>()); break;
        case json::value_t::array: {
            out += '[';
            bool first = true;
            for (const auto& e : v) {
                if (!first) out += ',';
                first = false;
                put(out, e);
            }
            out += ']';
            break;
        }
        case json::value_t::object: {
            std::vector<std::pair<std::u16string, const std::string*>> keys;
            for (const auto& [k, _] : v.items()) keys.emplace_back(utf16(k), &k);
            std::sort(keys.begin(), keys.end());
            out += '{';
            bool first = true;
            for (const auto& [_, k] : keys) {
                if (!first) out += ',';
                first = false;
                put_string(out, *k);
                out += ':';
                put(out, v.at(*k));
            }
            out += '}';
            break;
        }
        default: throw std::runtime_error("RFC 8785 has no form for a binary value");
    }
}

}  // namespace

std::string canonical_json(const json& v) {
    std::string out;
    put(out, v);
    return out;
}

json skeleton_of(const json& v) {
    switch (v.type()) {
        case json::value_t::null: return nullptr;
        case json::value_t::boolean: return false;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float: return 0;
        case json::value_t::string: return "";
        case json::value_t::object: {
            json out = json::object();
            for (const auto& [k, e] : v.items()) out[k] = skeleton_of(e);
            return out;
        }
        case json::value_t::array: {
            std::set<std::string> seen;
            std::vector<std::pair<std::string, json>> parts;
            for (const auto& e : v) {
                json s = skeleton_of(e);
                std::string c = canonical_json(s);
                if (seen.insert(c).second) parts.emplace_back(std::move(c), std::move(s));
            }
            std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            json out = json::array();
            for (auto& [_, s] : parts) out.push_back(std::move(s));
            return out;
        }
        default: return nullptr;
    }
}

std::string sha256_digest(const std::string& bytes) {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_Digest(bytes.data(), bytes.size(), d, &n, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out = "sha256:";
    for (unsigned i = 0; i < n; ++i) out += hex[d[i] >> 4], out += hex[d[i] & 15];
    return out;
}

}  // namespace maic
