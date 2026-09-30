#include "maic/bans.hpp"

#include <algorithm>
#include <cctype>

namespace maic {

nlohmann::json Bans::to_json() const {
    return {{"strings", strings}, {"tokens", tokens}, {"retries", retries}, {"replacement", replacement}, {"ignore_case", ignore_case}};
}

Bans Bans::from_json(const nlohmann::json& j) {
    Bans b;
    if (!j.is_object()) return b;
    for (const auto& s : j.value("strings", nlohmann::json::array())) {
        if (s.is_string() && !s.get<std::string>().empty()) b.strings.push_back(s.get<std::string>());
    }
    for (const auto& t : j.value("tokens", nlohmann::json::array())) {
        if (t.is_number_integer() || (t.is_string() && !t.get<std::string>().empty())) b.tokens.push_back(t);
    }
    b.retries = j.value("retries", b.retries);
    b.replacement = j.value("replacement", b.replacement);
    b.ignore_case = j.value("ignore_case", b.ignore_case);
    return b;
}

BanFilter::BanFilter(const Bans& bans, bool replace_mode) : bans_(bans), replace_(replace_mode) {
    // A token ban written as text is a string ban everywhere the filter runs.
    for (const auto& t : bans_.tokens) {
        if (t.is_string()) bans_.strings.push_back(t.get<std::string>());
    }
    for (const auto& s : bans_.strings) longest_ = std::max(longest_, s.size());
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
    clean_ += out;
    return out;
}

std::string BanFilter::feed(std::string_view delta) {
    if (!hit_.empty()) return "";  // cut already; nothing more is shown
    if (longest_ == 0) {
        clean_.append(delta);
        return std::string(delta);
    }
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
                    hit_ = ban;
                    return out;
                }
                out += release(at);
                held_.erase(0, ban.size());
                clean_ += bans_.replacement;
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
            return out;
        }
        ++at;
    }
    out += release(held_.size());
    return out;
}

std::string BanFilter::flush() {
    if (!hit_.empty()) return "";
    return release(held_.size());
}

}  // namespace maic
