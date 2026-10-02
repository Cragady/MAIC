// JSON Schema validation for the keyword subset named in maic/jsonschema.hpp: MAIC's protocol schemas and the
// pinned OpenAI description. No dependency beyond nlohmann-json and glibc's regcomp.
#include "maic/jsonschema.hpp"

#include <regex.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

namespace maic {

using nlohmann::json;

namespace {

const std::set<std::string> kChecked = {"type", "enum", "const", "properties", "required", "additionalProperties", "propertyNames",
                                        "maxProperties", "items", "minItems", "maxItems", "minLength", "maxLength", "pattern",
                                        "minimum", "maximum", "anyOf", "oneOf", "allOf", "$ref", "$defs"};
const std::set<std::string> kAnnotations = {"description", "title", "default", "example", "examples", "deprecated", "format",
                                            "discriminator", "readOnly", "writeOnly", "$comment", "$schema", "$id"};

struct Problem {
    std::string where;  // JSON pointer into the value
    std::string what;
    int depth = 0;      // how many levels into the value
    bool weak = false;  // a type, const or enum mismatch at the value or at a discriminator property: what the
                        // branches of a union the value did not mean say
};

// The branch of a failed union that got furthest: deeper first, then a real problem over a weak one, so a missing
// field in the branch an event's `type` picked beats the type mismatch every other branch reports one level up.
int score(const Problem& p) {
    return p.depth * 2 + (p.weak ? 0 : 1);
}

std::string token(const std::string& key) {
    std::string out;
    for (char c : key) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return out;
}

std::string kind_of(const json& v) {
    switch (v.type()) {
        case json::value_t::object: return "object";
        case json::value_t::array: return "array";
        case json::value_t::string: return "string";
        case json::value_t::boolean: return "boolean";
        case json::value_t::null: return "null";
        case json::value_t::number_integer:
        case json::value_t::number_unsigned: return "integer";
        default: return "number";
    }
}

bool has_type(const json& v, const std::string& t) {
    if (t == "integer") {
        if (!v.is_number_float()) return v.is_number_integer();
        double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    if (t == "number") return v.is_number();
    return kind_of(v) == t;
}

size_t code_points(const std::string& s) {
    return std::count_if(s.begin(), s.end(), [](unsigned char c) { return (c & 0xC0) != 0x80; });
}

std::string listing(const json& values) {
    if (values.size() > 8) return std::to_string(values.size()) + " allowed values";
    std::string out;
    for (const auto& v : values) out += (out.empty() ? "" : ", ") + v.dump();
    return out;
}

std::string type_names(const json& t) {
    if (t.is_string()) return t;
    std::string out;
    for (const auto& x : t) out += (out.empty() ? "" : " or ") + (x.is_string() ? x.get<std::string>() : x.dump());
    return out;
}

// s[key], or an empty list when s has none; a reference either way, so nothing is copied.
const json& member(const json& s, const char* key) {
    static const json none = json::array();
    auto it = s.find(key);
    return it == s.end() ? none : *it;
}

class Validator {
public:
    explicit Validator(const json& document) : doc_(document) {}

    // hops counts $refs followed without moving into the value, so a schema that refers to itself in a loop
    // fails instead of recursing forever.
    std::optional<Problem> check(const json& s, const json& v, const std::string& where, int depth, int hops) const {
        auto fail = [&](std::string what, bool weak = false) { return std::optional<Problem>(Problem{where, std::move(what), depth, weak}); };
        if (s.is_boolean()) return s.get<bool>() ? std::nullopt : fail("no value is allowed here");
        if (!s.is_object()) return fail("the schema here is neither an object nor a boolean");
        if (s.contains("$ref")) {
            const json* target = s["$ref"].is_string() ? resolve(s["$ref"]) : nullptr;
            if (!target) return fail("$ref " + s["$ref"].dump() + " names nothing in the document");
            if (hops > 64) return fail("$ref " + s["$ref"].dump() + " loops without reaching a schema");
            if (auto p = check(*target, v, where, depth, hops + 1)) return p;
        }
        if (s.contains("type")) {
            const json& t = s["type"];
            bool ok = t.is_string() ? has_type(v, t) : std::any_of(t.begin(), t.end(), [&](const json& x) { return x.is_string() && has_type(v, x); });
            if (!ok) return fail("must be " + type_names(t) + ", not " + kind_of(v), true);
        }
        if (s.contains("const") && s["const"] != v) return fail("must be " + s["const"].dump(), true);
        if (s.contains("enum") && std::find(s["enum"].begin(), s["enum"].end(), v) == s["enum"].end()) {
            return fail("must be one of " + listing(s["enum"]) + ", not " + v.dump(), true);
        }
        if (v.is_string()) {
            const std::string& str = v.get_ref<const std::string&>();
            size_t n = code_points(str);
            if (s.contains("minLength") && n < s["minLength"].get<size_t>()) return fail("is shorter than " + s["minLength"].dump() + " characters");
            if (s.contains("maxLength") && n > s["maxLength"].get<size_t>()) return fail("is longer than " + s["maxLength"].dump() + " characters");
            if (s.contains("pattern")) {
                regex_t re;
                if (regcomp(&re, s["pattern"].get<std::string>().c_str(), REG_EXTENDED | REG_NOSUB) != 0) return fail("pattern " + s["pattern"].dump() + " does not compile");
                bool ok = regexec(&re, str.c_str(), 0, nullptr, 0) == 0;
                regfree(&re);
                if (!ok) return fail("does not match the pattern " + s["pattern"].dump());
            }
        }
        if (v.is_number()) {
            if (s.contains("minimum") && v.get<double>() < s["minimum"].get<double>()) return fail("is less than the minimum " + s["minimum"].dump());
            if (s.contains("maximum") && v.get<double>() > s["maximum"].get<double>()) return fail("is more than the maximum " + s["maximum"].dump());
        }
        if (v.is_array()) {
            if (s.contains("minItems") && v.size() < s["minItems"].get<size_t>()) return fail("has fewer than " + s["minItems"].dump() + " items");
            if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>()) return fail("has more than " + s["maxItems"].dump() + " items");
            if (s.contains("items")) {
                for (size_t i = 0; i < v.size(); ++i) {
                    if (auto p = check(s["items"], v[i], where + "/" + std::to_string(i), depth + 1, 0)) {
                        p->weak = false;
                        return p;
                    }
                }
            }
        }
        if (v.is_object()) {
            if (s.contains("maxProperties") && v.size() > s["maxProperties"].get<size_t>()) return fail("has more than " + s["maxProperties"].dump() + " properties");
            const json& props = member(s, "properties");
            // Properties held to a const or an enum first: they are a union's discriminator, and a branch the
            // value did not mean should fail there, weakly, before it reports anything else.
            for (int pass = 0; pass < 2; ++pass) {
                for (const auto& [k, sub] : props.items()) {
                    bool fixed = sub.is_object() && (sub.contains("const") || sub.contains("enum"));
                    if (fixed != (pass == 0) || !v.contains(k)) continue;
                    if (auto p = check(sub, v[k], where + "/" + token(k), depth + 1, 0)) {
                        p->weak = p->weak && pass == 0;  // past the discriminators, a mismatch is the meant branch's problem
                        return p;
                    }
                }
            }
            for (const auto& r : member(s, "required")) {
                if (r.is_string() && !v.contains(r.get<std::string>())) return Problem{where + "/" + token(r), "is required", depth + 1, false};
            }
            for (const auto& [k, val] : v.items()) {
                std::string here = where + "/" + token(k);
                if (s.contains("propertyNames")) {
                    if (auto p = check(s["propertyNames"], k, here, depth + 1, 0)) return Problem{here, "the name " + p->what, depth + 1, false};
                }
                if (s.contains("additionalProperties") && !props.contains(k)) {
                    if (s["additionalProperties"] == false) return Problem{here, "is not allowed (additionalProperties is false)", depth + 1, false};
                    if (auto p = check(s["additionalProperties"], val, here, depth + 1, 0)) {
                        p->weak = false;
                        return p;
                    }
                }
            }
        }
        for (const auto& sub : member(s, "allOf")) {
            if (auto p = check(sub, v, where, depth, hops)) return p;
        }
        if (s.contains("anyOf")) {
            if (auto p = branches(s["anyOf"], v, where, depth, hops, false)) return p;
        }
        if (s.contains("oneOf")) {
            if (auto p = branches(s["oneOf"], v, where, depth, hops, true)) return p;
        }
        return std::nullopt;
    }

private:
    const json* resolve(const std::string& ref) const {
        if (ref.empty() || ref[0] != '#') return nullptr;
        try {
            json::json_pointer p(ref.substr(1));
            return doc_.contains(p) ? &doc_.at(p) : nullptr;
        } catch (const json::exception&) {
            return nullptr;
        }
    }

    std::optional<Problem> branches(const json& list, const json& v, const std::string& where, int depth, int hops, bool one) const {
        std::optional<Problem> best;
        std::vector<size_t> matched;
        for (size_t i = 0; i < list.size(); ++i) {
            auto p = check(list[i], v, where, depth, hops);
            if (!p) {
                if (!one) return std::nullopt;
                matched.push_back(i);
            } else if (!best || score(*p) > score(*best)) {
                best = p;
            }
        }
        if (matched.size() == 1) return std::nullopt;
        if (matched.size() > 1) {
            return Problem{where, "matches " + std::to_string(matched.size()) + " branches of oneOf (#" + std::to_string(matched[0]) + " and #" +
                                      std::to_string(matched[1]) + "); exactly one must match", depth, false};
        }
        if (!best) return Problem{where, "no branch to match: the list is empty", depth, false};
        return best;
    }

    const json& doc_;
};

std::string unsupported_at(const json& s, const std::string& where) {
    if (s.is_boolean()) return "";
    if (!s.is_object()) return (where.empty() ? "(root)" : where) + ": not a schema";
    for (const auto& [k, v] : s.items()) {
        if (kAnnotations.count(k) || k.rfind("x-", 0) == 0) continue;
        if (!kChecked.count(k)) return (where.empty() ? "(root)" : where) + ": " + k;
        std::string here = where + "/" + token(k);
        std::string e;
        if (k == "properties" || k == "$defs") {
            if (!v.is_object()) return here + ": not an object";
            for (const auto& [name, sub] : v.items()) {
                if (e.empty()) e = unsupported_at(sub, here + "/" + token(name));
            }
        } else if (k == "items" || k == "additionalProperties" || k == "propertyNames") {
            e = unsupported_at(v, here);
        } else if (k == "anyOf" || k == "oneOf" || k == "allOf") {
            if (!v.is_array()) return here + ": not a list";
            for (size_t i = 0; i < v.size() && e.empty(); ++i) e = unsupported_at(v[i], here + "/" + std::to_string(i));
        }
        if (!e.empty()) return e;
    }
    return "";
}

}  // namespace

std::string schema_error(const json& document, const json& schema, const json& value) {
    auto p = Validator(document).check(schema, value, "", 0, 0);
    if (!p) return "";
    return (p->where.empty() ? "(root)" : p->where) + ": " + p->what;
}

std::string schema_unsupported(const json& schema) {
    return unsupported_at(schema, "");
}

}  // namespace maic
