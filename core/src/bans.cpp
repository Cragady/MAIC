#include "maic/bans.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <fstream>
#include <cstdlib>

namespace maic {

std::vector<std::string> expand_ban_entry(const std::string& value) {
    if (value.empty() || value[0] != '@') return {value};
    std::string p = value.substr(1);
    if (!p.empty() && p[0] == '~') p = std::string(std::getenv("HOME")) + p.substr(1);
    std::ifstream in(p);
    if (!in) throw std::runtime_error("ban file not found: " + p);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        out.push_back(line);
    }
    return out;
}

nlohmann::json Bans::to_json() const {
    return {{"strings", strings}, {"patterns", patterns}, {"tokens", tokens}, {"retries", retries}, {"replacement", replacement}, {"ignore_case", ignore_case}, {"window", window}};
}

Bans Bans::from_json(const nlohmann::json& j) {
    Bans b;
    if (!j.is_object()) return b;
    for (const auto& s : j.value("strings", nlohmann::json::array())) {
        if (!s.is_string() || s.get<std::string>().empty()) continue;
        for (const auto& e : expand_ban_entry(s.get<std::string>())) b.strings.push_back(e);
    }
    for (const auto& p : j.value("patterns", nlohmann::json::array())) {
        if (!p.is_string() || p.get<std::string>().empty()) continue;
        for (const auto& e : expand_ban_entry(p.get<std::string>())) b.patterns.push_back(e);
    }
    for (const auto& t : j.value("tokens", nlohmann::json::array())) {
        if (t.is_number_integer()) b.tokens.push_back(t);
        else if (t.is_string() && !t.get<std::string>().empty()) {
            for (const auto& e : expand_ban_entry(t.get<std::string>())) {
                bool numeric = std::all_of(e.begin(), e.end(), [](unsigned char c) { return std::isdigit(c); });
                b.tokens.push_back(numeric ? nlohmann::json(std::stoll(e)) : nlohmann::json(e));
            }
        }
    }
    b.retries = j.value("retries", b.retries);
    b.replacement = j.value("replacement", b.replacement);
    b.ignore_case = j.value("ignore_case", b.ignore_case);
    b.window = std::max(8, j.value("window", b.window));
    return b;
}

BanFilter::BanFilter(const Bans& bans, bool replace_mode) : bans_(bans), replace_(replace_mode) {
    // A token ban written as text is a string ban everywhere the filter runs.
    for (const auto& t : bans_.tokens) {
        if (t.is_string()) bans_.strings.push_back(t.get<std::string>());
    }
    for (const auto& s : bans_.strings) longest_ = std::max(longest_, s.size());
    for (const auto& p : bans_.patterns) {
        regex_t re;
        int rc = regcomp(&re, p.c_str(), REG_EXTENDED | (bans_.ignore_case ? REG_ICASE : 0));
        if (rc != 0) {
            char err[200];
            regerror(rc, &re, err, sizeof(err));
            bad_.push_back(p + ": " + err);
            continue;
        }
        res_.push_back(re);
    }
}

BanFilter::~BanFilter() {
    for (auto& re : res_) regfree(&re);
}

// The regex stage. `text` has passed the literal stage; what comes back may be shown. Everything from
// rscan_ on is searched; a match that starts in text already shown cannot be unshown, so the cut lands at
// the first unshown byte instead. `release_all` gives out the held window too (a literal cut, the end of
// the reply); `at_end` says the reply really ended there. `^` and `$` mean the reply's own start and end:
// once text has left the front of the buffer `^` cannot match, and `$` matches only at_end.
std::string BanFilter::through_patterns(std::string text, bool release_all, bool at_end) {
    if (res_.empty()) {
        clean_ += text;
        return text;
    }
    rtext_ += text;
    std::string out;
    int anchors = (rfront_ ? 0 : REG_NOTBOL) | (at_end ? 0 : REG_NOTEOL);
    for (;;) {
        bool found = false;
        size_t best_s = 0, best_e = 0;
        for (auto& re : res_) {
            regmatch_t m[1];
            m[0].rm_so = static_cast<regoff_t>(rscan_);
            m[0].rm_eo = static_cast<regoff_t>(rtext_.size());
            if (regexec(&re, rtext_.c_str(), 1, m, REG_STARTEND | anchors) != 0) continue;
            size_t s = static_cast<size_t>(m[0].rm_so), e = static_cast<size_t>(m[0].rm_eo);
            if (e == s) continue;  // an empty match bans nothing
            if (!found || s < best_s) found = true, best_s = s, best_e = e;
        }
        if (!found) break;
        if (!replace_) {
            out += rtext_.substr(0, best_s);
            clean_ += rtext_.substr(0, best_s);
            hit_ = rtext_.substr(best_s, best_e - best_s);
            rtext_.clear();
            rscan_ = 0;
            return out;
        }
        rtext_.replace(best_s, best_e - best_s, bans_.replacement);
        rscan_ = best_s + bans_.replacement.size();
    }
    // Keep the last `window` bytes until more text arrives or the reply ends; a match could still grow there.
    size_t keep = release_all ? 0 : std::min(rtext_.size(), static_cast<size_t>(bans_.window));
    size_t give = rtext_.size() - keep;
    out += rtext_.substr(0, give);
    clean_ += rtext_.substr(0, give);
    rtext_.erase(0, give);
    if (give > 0) rfront_ = false;
    rscan_ = rscan_ > give ? rscan_ - give : 0;
    return out;
}

bool BanFilter::matches(const std::string& text, size_t at, const std::string& ban) const {
    if (at + ban.size() > text.size()) return false;
    for (size_t i = 0; i < ban.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(text[at + i]), b = static_cast<unsigned char>(ban[i]);
        if (bans_.ignore_case ? std::tolower(a) != std::tolower(b) : a != b) return false;
    }
    return true;
}

std::string BanFilter::release(size_t n) {
    std::string out = held_.substr(0, n);
    held_.erase(0, n);
    return out;  // shown only after the regex stage
}

std::string BanFilter::feed(std::string_view delta) {
    if (!hit_.empty()) return "";  // cut already; nothing more is shown
    if (longest_ == 0) return through_patterns(std::string(delta), false, false);
    held_.append(delta);
    std::string out;
    size_t at = 0;
    while (at < held_.size()) {
        bool could_start = false;
        bool replaced = false;
        for (const auto& ban : bans_.strings) {
            if (matches(held_, at, ban)) {
                if (!replace_) {
                    out += release(at);
                    held_.clear();
                    std::string shown = through_patterns(out, true, false);
                    if (hit_.empty()) hit_ = ban;  // a regex cut inside the released text comes first and is the one reported
                    return shown;
                }
                out += release(at);
                held_.erase(0, ban.size());
                out += bans_.replacement;
                replaced = true;
                break;
            }
            // The text from `at` to the end is a proper prefix of the ban: more input could complete it.
            size_t avail = held_.size() - at;
            if (avail < ban.size() && matches(held_, at, ban.substr(0, avail))) could_start = true;
        }
        if (replaced) {
            at = 0;
            continue;
        }
        if (could_start) {
            out += release(at);  // show what is before it, keep the rest until it resolves
            return through_patterns(out, false, false);
        }
        ++at;
    }
    out += release(held_.size());
    return through_patterns(out, false, false);
}

std::string BanFilter::flush() {
    if (!hit_.empty()) return "";
    return through_patterns(release(held_.size()), true, true);
}

}  // namespace maic
