#include "maid/models.hpp"

#include "maid/paths.hpp"
#include "maid/status.hpp"
#include "maid/tripwire.hpp"
#include "maid/vendor.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace maid {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const std::set<std::string> ROLES = {"agent", "vision", "scribe", "completion", "speech", "vad"};
const std::set<std::string> KINDS = {"weights", "mmproj", "vad"};
const std::set<std::string> ROOTS = {"llamacpp", "whisper", "fim"};

json read_json(const fs::path& p) {
    std::ifstream in(p);
    json j = json::parse(in, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error(p.string() + " is not a JSON object");
    return j;
}

std::string gb(long bytes) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    return b;
}

bool hex64(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool plain_name(const std::string& s) {
    return !s.empty() && s.find('/') == std::string::npos && s != "." && s != "..";
}

// The size on disk through a link, -1 when it is not there.
long size_of(const fs::path& p) {
    std::error_code ec;
    auto n = fs::file_size(p, ec);
    return ec ? -1 : static_cast<long>(n);
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec);
}

const CatalogFile* weights_of(const CatalogEntry& e) {
    for (const auto& f : e.files) {
        if (f.kind == "weights") return &f;
    }
    return nullptr;
}

// What an entry holds on the card at `context`, with `maid gpu`'s arithmetic.
double estimate_gb(const CatalogEntry& e, int context) {
    long bytes = 0;
    for (const auto& f : e.files) bytes += f.size;
    long total = e.root == "whisper" ? bytes + (300L << 20) : estimate_footprint(bytes, e.dir, context);
    return static_cast<double>(total) / (1024.0 * 1024 * 1024);
}

std::string ktok(int context) {
    return std::to_string(context / 1024) + "k";
}

}  // namespace

fs::path catalog_path() {
    return root_dir() / "models" / "catalog.json";
}

fs::path user_catalog_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "maid" / "models.json";
    return home_dir() / ".config" / "maid" / "models.json";
}

json merge_catalog(const json& shipped, const json& user) {
    json out = shipped;
    if (!out.contains("models") || !out["models"].is_array()) out["models"] = json::array();
    if (!user.is_object() || !user.contains("models") || !user["models"].is_array()) return out;
    for (const auto& u : user["models"]) {
        if (!u.is_object()) continue;
        std::string id = u.value("id", "");
        auto it = std::find_if(out["models"].begin(), out["models"].end(), [&](const json& m) { return m.is_object() && m.value("id", "") == id; });
        if (it == out["models"].end()) {
            out["models"].push_back(u);
            continue;
        }
        for (const auto& [k, v] : u.items()) (*it)[k] = v;
    }
    return out;
}

std::vector<CatalogEntry> parse_catalog(const json& j, std::vector<std::string>* problems) {
    std::vector<CatalogEntry> out;
    for (const auto& m : j.value("models", json::array())) {
        if (!m.is_object()) continue;
        CatalogEntry e;
        e.id = m.value("id", "");
        try {
            e.name = m.value("name", "");
            e.role = m.value("role", "");
            e.brief = m.value("brief", "");
            e.license = m.value("license", "");
            e.license_url = m.value("license_url", "");
            json source = m.value("source", json::object());
            e.repo = source.value("repo", "");
            e.revision = source.value("revision", "");
            for (const auto& f : m.value("files", json::array())) {
                e.files.push_back({f.value("name", ""), f.value("url", ""), f.value("sha256", ""), f.value("size", 0L), f.value("kind", "")});
            }
            json install = m.value("install", json::object());
            e.root = install.value("root", "");
            e.dir = install.value("dir", "");
            json shares = m.value("shares", json::object());
            e.shares_entry = shares.value("entry", "");
            e.shares_file = shares.value("file", "");
            for (const auto& v : m.value("vram", json::array())) e.vram.emplace_back(v.value("context", 0), v.value("gb", 0.0));
            e.context = m.value("context", 0);
            e.presets = m.value("presets", std::vector<std::string>{});
            e.notes = m.value("notes", "");
        } catch (const json::exception& ex) {
            report_skipped(problems, "model catalog entry '" + e.id + "': " + ex.what() + " (this entry is skipped; the others still load)");
            continue;
        }
        out.push_back(e);
    }
    return out;
}

std::vector<ApiModel> parse_api_models(const json& j, std::vector<std::string>* problems) {
    std::vector<ApiModel> out;
    for (const auto& m : j.value("api_models", json::array())) {
        if (!m.is_object()) continue;
        ApiModel a;
        a.id = m.value("id", "");
        try {
            a.provider = m.value("provider", "");
            a.model = m.value("model", "");
            a.name = m.value("name", "");
            a.brief = m.value("brief", "");
            a.source = m.value("source", "");
            a.checked = m.value("checked", "");
            json limits = m.value("limits", json::object());
            a.context = limits.value("context", 0L);
            a.output = limits.value("output", 0L);
            a.capabilities = m.value("capabilities", json::object());
            a.pricing = m.value("pricing", json::object());
            a.presets = m.value("presets", std::vector<std::string>{});
        } catch (const json::exception& ex) {
            report_skipped(problems, "model catalog api_models entry '" + a.id + "': " + ex.what() + " (this entry is skipped; the others are still priced)");
            continue;
        }
        out.push_back(a);
    }
    return out;
}

const std::vector<ApiModel>& api_models() {
    static const std::vector<ApiModel> all = [] {
        std::vector<ApiModel> out;
        std::error_code ec;
        for (auto path : {catalog_path, user_catalog_path}) {
            try {
                fs::path p = path();
                if (!fs::exists(p, ec)) continue;
                for (auto& a : parse_api_models(read_json(p))) {
                    auto it = std::find_if(out.begin(), out.end(), [&](const ApiModel& o) { return o.id == a.id; });
                    if (it == out.end()) out.push_back(std::move(a));
                    else *it = std::move(a);
                }
            } catch (const std::exception&) {
                // A broken file costs nothing; `maid models check` names what is wrong with it.
            }
        }
        return out;
    }();
    return all;
}

const ApiModel* find_api_model(const std::vector<ApiModel>& all, const std::string& provider, const std::string& model) {
    for (const auto& a : all) {
        if (a.provider == provider && a.model == model) return &a;
    }
    return nullptr;
}

const json& price_period(const json& pricing, std::time_t when) {
    static const json none = json::object();
    std::tm utc{};
    gmtime_r(&when, &utc);
    static const char* days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    int minute = utc.tm_hour * 60 + utc.tm_min;
    const json* fallback = &none;
    if (!pricing.contains("periods") || !pricing["periods"].is_array()) return none;
    for (const auto& p : pricing["periods"]) {
        if (!p.is_object()) continue;
        if (!p.contains("days") && !p.contains("utc")) {
            fallback = &p;
            continue;
        }
        const json d = p.value("days", json::array()), hours = p.value("utc", json::array());
        if (!d.empty() && std::find(d.begin(), d.end(), json(days[utc.tm_wday])) == d.end()) continue;
        bool in_hours = hours.empty();
        for (const auto& h : hours) {
            int a = 0, b = 0, c = 0, e = 0;
            if (h.is_string() && std::sscanf(h.get<std::string>().c_str(), "%d:%d-%d:%d", &a, &b, &c, &e) == 4) in_hours = in_hours || (minute >= a * 60 + b && minute < c * 60 + e);
        }
        if (in_hours) return p;
    }
    return *fallback;
}

double call_cost(const ApiModel& m, long input, long cached, long output, std::time_t when) {
    const json& p = price_period(m.pricing, when);
    double per = m.pricing.value("per", 1000000);
    long hit = std::min(cached, input);
    return (hit * p.value("input_cache_hit", 0.0) + (input - hit) * p.value("input_cache_miss", 0.0) + output * p.value("output", 0.0)) / per;
}

std::string format_cost(double cost, const std::string& currency) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), cost < 1 ? "%.4f" : "%.2f", cost);
    return std::string(buf) + " " + currency;
}

namespace {

fs::path costs_path() {
    return state_dir() / "costs.json";
}

// The file, or an empty object when it is missing or unreadable (it is only an estimate).
json read_costs() {
    std::ifstream in(costs_path());
    json j = json::parse(in, nullptr, false);
    return j.is_object() && j.contains("sessions") && j["sessions"].is_object() ? j : json{{"sessions", json::object()}};
}

}  // namespace

void add_session_cost(const std::string& session, double cost, const std::string& currency) {
    if (session.empty() || cost <= 0) return;
    fs::path path = costs_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Processes share the file (the daemon, a TUI of its own, maid-server): one writes at a time.
    int fd = open((path.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;
    flock(fd, LOCK_EX);
    json all = read_costs();
    json& s = all["sessions"][session];
    if (!s.is_object() || s.value("currency", currency) != currency) s = {{"cost", 0.0}, {"currency", currency}};
    s["cost"] = s.value("cost", 0.0) + cost;
    s["at"] = static_cast<long>(std::time(nullptr));
    while (all["sessions"].size() > 1000) {
        auto oldest = all["sessions"].begin();
        for (auto it = all["sessions"].begin(); it != all["sessions"].end(); ++it) {
            if (it.value().value("at", 0L) < oldest.value().value("at", 0L)) oldest = it;
        }
        all["sessions"].erase(oldest);
    }
    fs::path tmp = path;
    tmp += ".tmp";
    std::ofstream(tmp, std::ios::trunc) << all.dump() << "\n";
    fs::rename(tmp, path, ec);
    flock(fd, LOCK_UN);
    close(fd);
}

std::vector<CostAverage> session_cost_averages() {
    std::map<std::string, CostAverage> by;
    const json all = read_costs();  // named: iterating a temporary dangles
    for (const auto& [id, s] : all["sessions"].items()) {
        if (!s.is_object() || !s.value("cost", json()).is_number()) continue;
        CostAverage& a = by[s.value("currency", "")];
        a.average += s["cost"].get<double>();
        ++a.sessions;
    }
    std::vector<CostAverage> out;
    for (auto& [currency, a] : by) {
        a.currency = currency;
        a.average /= a.sessions;
        out.push_back(a);
    }
    return out;
}

std::vector<CatalogEntry> load_catalog() {
    fs::path shipped = catalog_path(), user = user_catalog_path();
    std::error_code ec;
    if (!fs::exists(shipped, ec)) throw std::runtime_error("no model catalog at " + shipped.string());
    json mine = json::object();
    if (fs::exists(user, ec)) {
        try {
            mine = read_json(user);
        } catch (const std::exception& e) {
            report_skipped(nullptr, std::string(e.what()) + " (your entries are skipped; the shipped catalog still loads)");
        }
    }
    return parse_catalog(merge_catalog(read_json(shipped), mine));
}

const CatalogEntry* find_entry(const std::vector<CatalogEntry>& all, const std::string& id) {
    for (const auto& e : all) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<std::string> check_catalog(const json& shipped, const json& user) {
    std::vector<std::string> problems;
    for (const auto& [where, j] : {std::pair<std::string, json>{"catalog", shipped}, {"models.json", user}}) {
        std::set<std::string> seen;
        for (const auto& m : j.value("models", json::array())) {
            std::string id = m.is_object() ? m.value("id", "") : "";
            if (!seen.insert(id).second) problems.push_back(where + ": the id '" + id + "' appears twice");
        }
    }
    std::vector<CatalogEntry> all;
    all = parse_catalog(merge_catalog(shipped, user), &problems);
    for (const auto& e : all) {
        auto bad = [&](const std::string& what) { problems.push_back((e.id.empty() ? std::string("(no id)") : e.id) + ": " + what); };
        if (e.id.empty() || e.id.find_first_of(" \t/") != std::string::npos) bad("the id must be a single word without slashes");
        if (!ROLES.count(e.role)) bad("role '" + e.role + "' is not one of agent, vision, scribe, completion, speech, vad");
        if (!ROOTS.count(e.root)) bad("install.root '" + e.root + "' is not one of llamacpp, whisper, fim");
        if (e.root == "whisper" && !e.dir.empty()) bad("whisper models sit directly in <models_dir>/whisper (install.dir \"\"), where services/whisper.json loads them");
        if (e.root != "whisper" && !plain_name(e.dir)) bad("install.dir must name one folder under the root");
        if (e.brief.empty()) bad("no brief");
        if (e.files.empty()) bad("no files");
        if (e.repo.empty() || e.revision.empty()) bad("source.repo and source.revision are required");
        for (const auto& f : e.files) {
            std::string label = "file '" + f.name + "'";
            if (!plain_name(f.name)) bad(label + ": a file name without slashes is required");
            if (f.url.empty()) bad(label + ": no url");
            if (!hex64(f.sha256)) bad(label + ": sha256 must be 64 lowercase hex characters");
            if (f.size <= 0) bad(label + ": no size");
            if (!KINDS.count(f.kind)) bad(label + ": kind '" + f.kind + "' is not one of weights, mmproj, vad");
            if (f.url.rfind("https://huggingface.co/", 0) == 0 && f.url.find("/resolve/" + e.revision + "/") == std::string::npos) {
                bad(label + ": the url is not pinned to the revision (resolve/" + e.revision + "/)");
            }
        }
        if (!e.shares_entry.empty()) {
            const CatalogEntry* s = find_entry(all, e.shares_entry);
            const CatalogFile* shared = nullptr;
            if (s) {
                for (const auto& f : s->files) {
                    if (f.name == e.shares_file) shared = &f;
                }
            }
            const CatalogFile* own = weights_of(e);
            if (!s) bad("shares.entry '" + e.shares_entry + "' is not in the catalog");
            else if (!s->shares_entry.empty()) bad("shares " + s->id + ", which is itself a share");
            else if (s->root != e.root) bad("shares an entry under another root");
            else if (!shared) bad("shares.file '" + e.shares_file + "' is not one of " + s->id + "'s files");
            else if (e.files.size() != 1 || !own) bad("a share has exactly one file, the link, of kind weights");
            else if (own->sha256 != shared->sha256 || own->size != shared->size) bad("the link's sha256 and size must be those of " + s->id + "'s " + shared->name);
        }
        for (const auto& [context, figure] : e.vram) {
            double want = estimate_gb(e, context);
            if (std::fabs(want - figure) > 0.051) {
                char b[160];
                snprintf(b, sizeof(b), "vram at %d tokens says %.1f GB; the files and context give %.1f GB", context, figure, want);
                bad(b);
            }
        }
    }
    for (const auto& [where, j] : {std::pair<std::string, json>{"catalog", shipped}, {"models.json", user}}) {
        std::vector<std::string> skipped;
        std::vector<ApiModel> apis = parse_api_models(j, &skipped);
        for (const auto& p : skipped) problems.push_back(where + ": " + p);
        std::set<std::string> seen;
        for (const auto& a : apis) {
            auto bad = [&](const std::string& what) { problems.push_back(where + ": api_models " + (a.id.empty() ? std::string("(no id)") : a.id) + ": " + what); };
            if (!seen.insert(a.id).second) bad("the id appears twice");
            if (a.id.empty() || a.provider.empty() || a.model.empty()) bad("id, provider and model are required");
            if (a.brief.empty() || a.source.empty()) bad("brief and source are required");
            static const std::regex date(R"(\d{4}-\d{2}-\d{2})"), hours(R"(([01]\d|2[0-4]):[0-5]\d-([01]\d|2[0-4]):[0-5]\d)");
            if (!std::regex_match(a.checked, date)) bad("checked must be the date the figures were read, YYYY-MM-DD");
            if (a.context <= 0 || a.output <= 0) bad("limits.context and limits.output are required");
            const json periods = a.pricing.value("periods", json::array());
            if (!a.pricing.value("currency", json()).is_string() || !a.pricing.value("per", json()).is_number_integer() || !periods.is_array() || periods.empty()) {
                bad("pricing needs currency, per (tokens) and periods");
                continue;
            }
            int defaults = 0;
            for (const auto& p : periods) {
                std::string name = p.is_object() ? p.value("name", "") : "";
                for (const char* k : {"input_cache_hit", "input_cache_miss", "output"}) {
                    if (!p.is_object() || !p.value(k, json()).is_number() || p[k].get<double>() < 0) bad("period '" + name + "': " + k + " must be a price, 0 or more");
                }
                if (!p.is_object() || (!p.contains("days") && !p.contains("utc"))) {
                    ++defaults;
                    continue;
                }
                for (const auto& d : p.value("days", json::array())) {
                    static const std::set<std::string> days = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
                    if (!d.is_string() || !days.count(d.get<std::string>())) bad("period '" + name + "': days are mon to sun, not " + d.dump());
                }
                for (const auto& h : p.value("utc", json::array())) {
                    if (!h.is_string() || !std::regex_match(h.get<std::string>(), hours)) bad("period '" + name + "': utc ranges are HH:MM-HH:MM, not " + h.dump());
                }
            }
            if (defaults != 1) bad("pricing needs exactly one period without days and utc, the default");
        }
    }
    return problems;
}

fs::path models_root(const std::string& root) {
    if (root == "llamacpp") return llamacpp_models_root();
    if (root == "whisper") return whisper_models_root();
    if (root == "fim") return fim_models_root();
    throw std::runtime_error("unknown models root '" + root + "' (llamacpp, whisper or fim)");
}

fs::path entry_dir(const CatalogEntry& e) {
    return e.dir.empty() ? models_root(e.root) : models_root(e.root) / e.dir;
}

bool entry_installed(const CatalogEntry& e, const std::vector<CatalogEntry>& all) {
    if (e.files.empty()) return false;
    if (!e.shares_entry.empty()) {
        const CatalogEntry* s = find_entry(all, e.shares_entry);
        std::error_code ec;
        fs::path link = entry_dir(e) / e.files[0].name;
        return s && fs::is_symlink(link, ec) && same_file(link, entry_dir(*s) / e.shares_file) && size_of(link) == e.files[0].size;
    }
    for (const auto& f : e.files) {
        if (size_of(entry_dir(e) / f.name) != f.size) return false;
    }
    return true;
}

bool entry_current(const CatalogEntry& e) {
    const CatalogFile* w = weights_of(e);
    if (!w) return false;
    if (e.root == "llamacpp") return !e.dir.empty() && llamacpp_current_id() == e.dir;
    if (e.root == "fim") return !e.dir.empty() && fim_current_id() == e.dir;
    auto v = find_vendor("whisper");
    std::error_code ec;
    return v && fs::is_symlink(vendor_model_link(*v), ec) && same_file(vendor_model_link(*v), entry_dir(e) / w->name);
}

std::vector<std::string> entry_presets(const CatalogEntry& e, const std::vector<std::pair<std::string, std::string>>& presets) {
    std::vector<std::string> out = e.presets;
    if (e.root != "llamacpp" || e.dir.empty()) return out;
    for (const auto& [name, model] : presets) {
        if ((model == "llamacpp/" + e.dir || model == "llamacpp-2/" + e.dir) && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

std::string vram_line(const CatalogEntry& e) {
    if (e.vram.empty()) return "";
    std::string out;
    for (const auto& [context, figure] : e.vram) {
        char b[48];
        snprintf(b, sizeof(b), "%.1f GB", figure);
        out += (out.empty() ? "" : ", ") + std::string(b) + (context ? " at " + ktok(context) : " resident");
    }
    return out + " (estimates: the files plus " + (e.root == "whisper" ? "300 MB of buffers" : "a KV cache for the context") + ")";
}

void link_entry(const CatalogEntry& e, std::ostream& out) {
    const CatalogFile* w = weights_of(e);
    if (!w) throw std::runtime_error(e.id + " is not a model a server loads as current (services/whisper.json loads the VAD by its own name)");
    fs::path file = entry_dir(e) / w->name;
    std::error_code ec;
    if (!fs::exists(file, ec)) throw std::runtime_error(e.id + " is not installed: maid models install " + e.id);
    if (e.root == "fim") {
        fs::path link = fim_model_link();
        if (fs::is_symlink(link, ec)) fs::remove(link);
        else if (fs::exists(link, ec)) throw std::runtime_error(link.string() + " exists and is not a link; remove it first");
        fs::create_symlink(fs::path(e.dir) / w->name, link);
        out << link.string() << " -> " << (fs::path(e.dir) / w->name).string() << "\n";
        out << "llamacpp-fim serves it as the model \"current\"\n" << reload_fim(load_services(root_dir() / "services"));
        return;
    }
    auto v = find_vendor(e.root == "whisper" ? "whisper" : "llamacpp");
    if (!v) throw std::runtime_error("the vendor manifest has no " + e.root + " entry");
    vendor_use(*v, file);
}

void install_entry(const CatalogEntry& e, const std::vector<CatalogEntry>& all, bool link, std::ostream& out) {
    require_armed("install a model");
    fs::path dir = entry_dir(e);
    std::error_code ec;
    if (!e.shares_entry.empty()) {
        const CatalogEntry* s = find_entry(all, e.shares_entry);
        if (!s || e.files.empty()) throw std::runtime_error(e.id + " shares '" + e.shares_entry + "', which the catalog does not have (maid models check)");
        if (!entry_installed(*s, all)) {
            out << "installing " << s->id << " first: " << e.id << " links its " << e.shares_file << "\n";
            install_entry(*s, all, false, out);
        }
        fs::path linkfile = dir / e.files[0].name, shared = entry_dir(*s) / e.shares_file;
        if (fs::is_symlink(linkfile, ec) && same_file(linkfile, shared)) {
            out << "present: " << linkfile.string() << " -> " << fs::read_symlink(linkfile, ec).string() << "\n";
        } else if (fs::exists(linkfile, ec) || fs::is_symlink(linkfile, ec)) {
            throw std::runtime_error(linkfile.string() + " is there and is not the link to " + shared.string() + "; move it aside first");
        } else {
            fs::create_directories(dir);
            fs::path target = shared.lexically_normal().lexically_relative(dir.lexically_normal());
            fs::create_symlink(target, linkfile);
            out << "linked " << linkfile.string() << " -> " << target.string() << " (nothing copied)\n";
        }
    } else {
        for (const auto& f : e.files) {
            fs::path p = dir / f.name;
            long have = size_of(p);
            if (have == f.size) {
                out << "present: " << p.string() << " (" << gb(f.size) << ")\n";
                continue;
            }
            if (have >= 0 || fs::is_symlink(p, ec)) {
                throw std::runtime_error(p.string() + " is there with " + std::to_string(have) + " bytes where the catalog says " + std::to_string(f.size) + "; move it aside, then install again");
            }
            out << "fetching " << f.name << " (" << gb(f.size) << ") from " << e.repo << "\n" << std::flush;
            download_verified(f.url, f.sha256, dir, f.name);
        }
    }
    out << e.id << " is installed in " << dir.string() << "\n";
    if (std::string v = vram_line(e); !v.empty()) out << "on the card: " << v << "\n";
    if (!e.presets.empty()) {
        out << "presets:";
        for (const auto& p : e.presets) out << " " << p;
        out << "\n";
    }
    if (link) link_entry(e, out);
    else if (weights_of(e) && !entry_current(e)) out << "maid models install " << e.id << " --link makes it the current " << e.root << " model\n";
    if (e.root == "llamacpp") out << "a running llama server lists a new folder after it restarts (maid down, then maid up)\n";
}

bool verify_entry(const CatalogEntry& e, std::ostream& out) {
    bool ok = true;
    for (const auto& f : e.files) {
        fs::path p = entry_dir(e) / f.name;
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            out << "missing  " << p.string() << "\n";
            ok = false;
            continue;
        }
        out << "hashing  " << p.string() << " (" << gb(f.size) << ") ..." << std::flush;
        std::string have = file_sha256(p);
        if (have == f.sha256) {
            out << " ok" << (fs::is_symlink(p, ec) ? " (through the link to " + fs::read_symlink(p, ec).string() + ")" : "") << "\n";
        } else {
            out << " MISMATCH: expected " << f.sha256 << ", got " << (have.empty() ? "nothing" : have) << "\n";
            ok = false;
        }
    }
    return ok;
}

std::vector<std::string> dependents(const CatalogEntry& e, const std::vector<CatalogEntry>& all) {
    std::vector<std::string> out;
    for (const auto& x : all) {
        if (x.id == e.id || !entry_installed(x, all)) continue;
        bool depends = x.shares_entry == e.id;
        for (const auto& xf : x.files) {
            std::error_code ec;
            fs::path xp = entry_dir(x) / xf.name;
            if (!fs::is_symlink(xp, ec)) continue;
            for (const auto& f : e.files) depends = depends || same_file(xp, entry_dir(e) / f.name);
        }
        if (depends) out.push_back(x.id);
    }
    return out;
}

std::string remove_blocker(const CatalogEntry& e, const std::vector<CatalogEntry>& all) {
    auto deps = dependents(e, all);
    if (!deps.empty()) {
        std::string names;
        for (const auto& d : deps) names += (names.empty() ? "" : ", ") + d;
        return names + " links " + e.id + "'s weights and is installed; remove " + names + " first, or keep both";
    }
    if (entry_current(e)) return e.id + " is the current " + e.root + " model; make another current first (maid models install ID --link)";
    return "";
}

void remove_entry(const CatalogEntry& e, const std::vector<CatalogEntry>& all, std::ostream& out) {
    require_armed("remove a model");
    if (std::string why = remove_blocker(e, all); !why.empty()) throw std::runtime_error("refusing to remove " + e.id + ": " + why);
    fs::path dir = entry_dir(e);
    std::error_code ec;
    for (const auto& f : e.files) {
        fs::path p = dir / f.name;
        if (!fs::is_symlink(p, ec) && !fs::exists(p, ec)) continue;
        bool was_link = fs::is_symlink(p, ec);
        fs::remove(p, ec);
        if (ec) throw std::runtime_error("could not remove " + p.string() + ": " + ec.message());
        out << "removed " << p.string() << (was_link ? " (a link; what it points at is untouched)" : "") << "\n";
    }
    if (e.dir.empty() || !fs::is_directory(dir, ec)) return;
    if (fs::is_empty(dir, ec)) {
        fs::remove(dir, ec);
        out << "removed " << dir.string() << "\n";
    } else {
        out << "kept " << dir.string() << ": other files are in it\n";
    }
}

}  // namespace maid
