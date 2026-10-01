// Credential scrubbing for session files. Patterns describe shapes, never values, and run with glibc's
// regcomp (a DFA: a 3 MB tool result cannot overflow the stack the way std::regex once did in search_files).
// Replacement happens inside JSON string values, never in the serialised line, so the copy stays valid JSON.
#include "maic/redact.hpp"

#include "maic/session.hpp"

#include <fcntl.h>
#include <regex.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace maic {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Pattern {
    const char* kind;
    const char* regex;  // POSIX ERE
    int group;          // the subexpression that is the secret; 0 for the whole match
    bool icase = false;
};

// Context first, so a token after "--password" or "Bearer" is reported by where it sat; vendor prefixes
// next; the opaque shapes cai's redact removes last. Every pattern only sees what the earlier ones left,
// and a marker is never matched again because no value class here accepts '['.
const Pattern kPatterns[] = {
    {"private-key", "-----BEGIN [A-Z ]*PRIVATE KEY-----.*-----END [A-Z ]*PRIVATE KEY-----", 0},
    {"url-password", "[A-Za-z][A-Za-z0-9+.-]*://[^/:@[:space:]]+:([^@/[:space:]]+)@", 1},
    {"basic-auth", "(^|[^A-Za-z0-9])(-u|--user)[ =]+[^[:space:]:'\"]+:([^[:space:]'\"[]+)", 3},
    {"basic-auth", "[Bb]asic +([A-Za-z0-9+/]{8,}={0,2})", 1},
    {"env-secret", "(^|[^A-Za-z0-9_])(([A-Za-z0-9]+_)*(API_?KEY|KEY|TOKEN|SECRET|PASSWORD|PASSWD|PWD|CREDENTIALS?)(_[A-Za-z0-9]+)*[ \t]*=[ \t]*[\"']?)([^[:space:]\"'[]{8,})", 6, true},
    {"cli-password", "(^|[^A-Za-z0-9_-])(--?(password|passwd|token|api-?key|secret|access-?key)[= ]+)([^[:space:]\"'[]{4,})", 4, true},
    {"json-secret", "\"(api_?key|access_?key|secret(_?key)?|token|access_?token|refresh_?token|password|passwd|private_?key|client_?secret)\"[ \t]*:[ \t]*\"([^\"[]{4,})\"", 3, true},
    {"bearer", "[Bb]earer +([A-Za-z0-9._~+/=-]{16,})", 1},
    {"jwt", "eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}", 0},
    {"aws-key", "(^|[^A-Za-z0-9])((AKIA|ASIA)[0-9A-Z]{16})", 2},
    {"github-token", "(gh[pousr]_|github_pat_)[A-Za-z0-9_]{20,}", 0},
    {"slack-token", "xox[abposr]-[A-Za-z0-9-]{10,}", 0},
    {"api-key", "(^|[^A-Za-z0-9_-])((sk|rk)-[A-Za-z0-9_-]{16,}|(sk|rk)_(live|test)_[A-Za-z0-9]{16,}|AIza[0-9A-Za-z_-]{35}|glpat-[A-Za-z0-9_-]{20,}|hf_[A-Za-z0-9]{20,}|npm_[A-Za-z0-9]{36}|pypi-[A-Za-z0-9_-]{20,})", 2},
    {"hex-32", "(^|[^0-9A-Za-z])([0-9A-F]{32,})([^0-9A-Za-z]|$)", 2},
    {"numeric-handle", "(^|[^0-9A-Za-z])([0-9]{16,})([^0-9A-Za-z]|$)", 2},
    {"base64-blob", "[A-Za-z0-9+/]{48,}={0,2}", 0},
};

// A JSON key whose string value is a secret whatever it looks like: {"env": {"API_KEY": "..."}}.
const char* kSecretName = "^([A-Za-z0-9]+_)*(API_?KEY|KEY|TOKEN|SECRET|PASSWORD|PASSWD|PWD|CREDENTIALS?)(_[A-Za-z0-9]+)*$";

struct Compiled {
    const Pattern* pattern;
    regex_t re;
};

regex_t compile(const char* regex, bool icase) {
    regex_t re;
    if (int rc = regcomp(&re, regex, REG_EXTENDED | (icase ? REG_ICASE : 0)); rc != 0) {
        char why[256];
        regerror(rc, &re, why, sizeof(why));
        throw std::logic_error(std::string("redact: bad pattern ") + regex + ": " + why);
    }
    return re;
}

const std::vector<Compiled>& patterns() {
    static const std::vector<Compiled> all = [] {
        std::vector<Compiled> out;
        for (const auto& p : kPatterns) out.push_back({&p, compile(p.regex, p.icase)});
        return out;
    }();
    return all;
}

bool is_secret_name(const std::string& key) {
    static const regex_t re = compile(kSecretName, true);
    return regexec(&re, key.c_str(), 0, nullptr, 0) == 0;
}

// Values that thread a session together or say what a record is. Never credentials, and a fork or a
// replay would break without them.
bool structural(const std::string& key) {
    for (const char* k : {"type", "time", "id", "tool_call_id", "raw_kind", "role", "signature", "tool", "tool_name", "name", "kind", "format", "stage"}) {
        if (key == k) return true;
    }
    return false;
}

void redact_json(json& j, std::map<std::string, size_t>& counts) {
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) {
            if (structural(key)) continue;
            if (value.is_string() && is_secret_name(key)) {
                if (!value.get_ref<const std::string&>().empty()) {
                    value = "[REDACTED:secret-field]";
                    ++counts["secret-field"];
                }
            } else {
                redact_json(value, counts);
            }
        }
    } else if (j.is_array()) {
        for (auto& v : j) redact_json(v, counts);
    } else if (j.is_string()) {
        j = redact_text(j.get<std::string>(), counts);
    }
}

}  // namespace

size_t RedactReport::total() const {
    size_t n = 0;
    for (const auto& [kind, count] : counts) n += count;
    return n;
}

std::string redact_text(const std::string& text, std::map<std::string, size_t>& counts) {
    std::string s = text;
    for (const auto& c : patterns()) {
        std::string marker = std::string("[REDACTED:") + c.pattern->kind + "]";
        size_t pos = 0;
        while (pos < s.size()) {
            regmatch_t m[8];
            m[0].rm_so = static_cast<regoff_t>(pos);
            m[0].rm_eo = static_cast<regoff_t>(s.size());
            // REG_STARTEND scans a range of a string that may hold NULs; offsets come back relative to s.
            if (regexec(&c.re, s.c_str(), 8, m, REG_STARTEND | (pos ? REG_NOTBOL : 0)) != 0) break;
            const regmatch_t& g = m[c.pattern->group];
            if (g.rm_so < 0 || g.rm_eo <= g.rm_so) {
                pos = static_cast<size_t>(m[0].rm_eo) + (m[0].rm_eo == m[0].rm_so ? 1 : 0);
                continue;
            }
            s.replace(g.rm_so, g.rm_eo - g.rm_so, marker);
            ++counts[c.pattern->kind];
            pos = static_cast<size_t>(g.rm_so) + marker.size();
        }
    }
    return s;
}

RedactReport redact_session(const fs::path& in, const fs::path& out) {
    std::ifstream src(in);
    if (!src) throw std::runtime_error("can't read " + in.string());
    int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("can't create " + out.string() + ": " + std::strerror(errno) +
                                 (errno == EEXIST ? " (a redacted copy is never written over an existing file; give -o another name)" : ""));
    }
    close(fd);
    std::ofstream dst(out);
    RedactReport report;
    for (std::string line; std::getline(src, line);) {
        if (line.empty()) {
            dst << '\n';
            continue;
        }
        auto j = json::parse(line, nullptr, false);
        if (j.is_object()) {
            redact_json(j, report.counts);
            dst << j.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
        } else {
            ++report.malformed;
            dst << redact_text(line, report.counts) << '\n';
        }
        ++report.records;
    }
    return report;
}

fs::path redact_session_in_place(const fs::path& path, const std::string& invocation, RedactReport& report) {
    bool maic_session = is_maic_session(path);
    fs::path backup = backup_session(path), tmp = path.string() + ".redacting";
    report = redact_session(path, tmp);
    fs::rename(tmp, path);
    if (maic_session) {
        std::time_t t = std::time(nullptr);
        char stamp[40];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", std::localtime(&t));
        json rec = {{"type", "rewritten"}, {"time", stamp}, {"tool", "maic sessions redact"}, {"invocation", invocation}, {"backup", backup.string()},
                    {"records_before", report.records}, {"records_after", report.records}};
        std::ofstream(path, std::ios::app) << rec.dump() << '\n';
    }
    return backup;
}

}  // namespace maic
