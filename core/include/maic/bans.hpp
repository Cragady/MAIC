#pragma once

#include <nlohmann/json.hpp>

#include <regex.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maic {

// Things the model must not say.
//
// String bans are enforced by MAIC itself, so they work with every provider: the streamed reply is passed
// through a filter that holds back a short tail, and the moment a banned string would appear the call is cut
// before the text reaches the screen; the agent then re-asks with the clean part kept and a nudge, up to
// `retries` times, and after that replaces the string instead. Token bans map to `logit_bias` (the token's
// probability becomes minus infinity) on providers that take it: OpenAI-compatible servers such as llama.cpp,
// vLLM and LM Studio. Anthropic has no logit bias; there a token ban given as text is treated as a string ban
// and one given as a number is reported as unsupported.
// Regex bans (`patterns`, POSIX extended) are matched over the same stream. A regex cannot say how much more
// text might complete a match, so the filter keeps the last `window` characters back until more arrives or
// the reply ends; a pattern longer than the window can slip partly onto the screen before it is cut.
// A ban given as "@path" stands for the file's lines: one entry per non-empty line, `#` lines skipped, `~`
// expanded. Anything else is one entry. Throws when the file cannot be read.
std::vector<std::string> expand_ban_entry(const std::string& value);

// A string or pattern entry may name a steer action for its hits instead of the cut, tell and re-ask:
// `{ "kubernetes|helm chart", steer = "drop", note = "..." }` (docs/bans.md, engine-protocol.md section 11).
struct BanSteer {
    std::string action;  // "" for none: steer, drop, interrupt, keep, halt
    std::string note;
};

// A ban entry with a steer that fired: its action, note, and which entry ({list, index}, never the matched text).
struct BanHit {
    std::string action, note;
    std::string list;  // "strings" or "patterns"
    size_t index = 0;
};

struct Bans {
    std::vector<std::string> strings;
    std::vector<std::string> patterns;
    std::vector<BanSteer> string_steers;   // beside strings, by index; shorter means none for the rest
    std::vector<BanSteer> pattern_steers;  // beside patterns
    std::vector<nlohmann::json> tokens;  // integers (token ids) or strings (for servers that accept them)
    int retries = 3;
    std::string replacement = "[banned]";
    bool ignore_case = false;
    int window = 64;

    bool empty() const { return strings.empty() && patterns.empty() && tokens.empty(); }
    // Layers add up: another file's entries after these, steers kept beside their entries.
    void add(const Bans& more);
    nlohmann::json to_json() const;
    static Bans from_json(const nlohmann::json& j);
};

// Streams text through the string bans. feed() returns the part that is safe to show now; the rest is held
// until it is known not to start a ban. When a ban is found, `triggered` is set with the string, and feed()
// returns only the text before it (the held tail is dropped). In replace mode the string is swapped for the
// replacement instead and streaming continues.
class BanFilter {
public:
    explicit BanFilter(const Bans& bans, bool replace_mode = false);
    ~BanFilter();
    BanFilter(const BanFilter&) = delete;
    BanFilter& operator=(const BanFilter&) = delete;

    // Patterns that did not compile, with the error; the rest are in force.
    const std::vector<std::string>& bad_patterns() const { return bad_; }

    std::string feed(std::string_view delta);
    std::string flush();  // the held-back tail at the end of a reply (nothing when a ban triggered)

    bool triggered() const { return !hit_.empty(); }
    const std::string& hit() const { return hit_; }
    // The steer of the entry that triggered, when it names one. Such an entry cuts in replace mode too.
    std::optional<BanHit> hit_steer() const;
    const std::string& clean() const { return clean_; }  // everything released so far

private:
    bool matches(const std::string& text, size_t at, const std::string& ban) const;
    std::string release(size_t n);
    std::string through_patterns(std::string text, bool release_all, bool at_end);

    Bans bans_;
    bool replace_;
    std::string held_;
    std::string clean_;
    std::string hit_;
    size_t longest_ = 0;
    const BanSteer* steer_of(const std::string& list, size_t index) const;

    std::vector<regex_t> res_;
    std::vector<size_t> re_index_;  // each compiled pattern's index in bans_.patterns
    std::string hit_list_;
    size_t hit_index_ = 0;
    std::vector<std::string> bad_;
    std::string rtext_;    // text past the literal stage, not yet released by the regex stage
    size_t rscan_ = 0;     // rtext_ before this is known clean
    bool rfront_ = true;   // rtext_ still begins where the reply began, so `^` may match
};

}  // namespace maic
