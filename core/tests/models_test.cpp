// The model catalog: parsing and `check`, user overrides by id, installs from a local server with real hashes,
// shares, removal rules, verify, the installed column on a tree shaped like Micaiah's drive, and the code
// completion service's place in `maid gpu`.
#include "check.hpp"

#include "maid/http.hpp"
#include "maid/models.hpp"
#include "maid/paths.hpp"
#include "maid/service.hpp"
#include "maid/settings.hpp"
#include "maid/status.hpp"
#include "maid/vendor.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace maid;
using nlohmann::json;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

// A file of `bytes` that takes no space (a sparse file), for a tree with real model sizes.
void sized(const fs::path& p, long bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "GGUF";
    fs::resize_file(p, static_cast<uintmax_t>(bytes));
}

std::string sha_of(const std::string& content, const fs::path& scratch) {
    write_file(scratch, content);
    std::string h = file_sha256(scratch);
    fs::remove(scratch);
    return h;
}

json file_json(const std::string& name, const std::string& url, const std::string& sha, long size, const std::string& kind = "weights") {
    return {{"name", name}, {"url", url}, {"sha256", sha}, {"size", size}, {"kind", kind}};
}

json entry_json(const std::string& id, const std::string& role, const std::string& root, const std::string& dir, const json& files) {
    return {{"id", id}, {"name", id}, {"role", role}, {"brief", "a test entry"}, {"source", {{"repo", "local/test"}, {"revision", "0"}}},
            {"files", files}, {"install", {{"root", root}, {"dir", dir}}}};
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A pid file line naming this process (its pid and start time, field 22 of /proc/self/stat), so a service whose
// pid file holds it counts as running.
std::string self_pid_line() {
    std::ifstream in("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::istringstream fields(stat.substr(stat.rfind(')') + 2));
    std::string f;
    for (int i = 0; i < 19; ++i) fields >> f;
    fields >> f;
    return std::to_string(getpid()) + " " + f + "\n";
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : " | ") + s;
    return out;
}

}  // namespace

int main() {
    setenv("MAID_TRIPWIRE_FILE", ("/tmp/maid-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);
    fs::path ws = fs::path(std::getenv("HOME")) / ".cache" / ("maid-models-test-" + std::to_string(getpid()));
    fs::remove_all(ws);
    fs::path cfg = ws / "config", state = ws / "state", mdir = ws / "models";
    fs::create_directories(cfg / "maid");
    setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
    setenv("XDG_STATE_HOME", state.c_str(), 1);
    write_file(cfg / "maid" / "settings.lua", "return { models_dir = '" + mdir.string() + "' }");
    setenv("MAID_MODELS_DIR", mdir.c_str(), 1);  // as main() exports it from settings

    section("the shipped catalog");
    std::ifstream in(catalog_path());
    json shipped = json::parse(in, nullptr, false, true);
    expect(!shipped.is_discarded() && shipped["models"].is_array(), "models/catalog.json parses");
    auto problems = check_catalog(shipped, json::object());
    expect(problems.empty(), "maid models check finds nothing wrong with it: " + joined(problems));
    auto all = load_catalog();
    for (const char* id : {"qwen3.5-4b", "qwen3.5-9b", "qwen3.5-9b-text", "whisper-distil-large-v3", "whisper-large-v3-turbo-q5_0", "silero-vad-v6.2.0",
                           "qwen2.5-coder-7b", "qwen2.5-coder-3b", "qwen2.5-coder-1.5b"}) {
        expect(find_entry(all, id) != nullptr, std::string("the catalog has ") + id);
    }
    const CatalogEntry* text = find_entry(all, "qwen3.5-9b-text");
    const CatalogEntry* nine = find_entry(all, "qwen3.5-9b");
    expect(text && text->role == "agent" && text->shares_entry == "qwen3.5-9b" && text->shares_file == "Qwen3.5-9B-Q4_K_M.gguf" && text->files.size() == 1 &&
               text->files[0].name == "Qwen3.5-9B-Q4_K_M-text.gguf" && text->dir == "Qwen3.5-9B-Q4_K_M-text",
           "the text-only 9B is its own agent entry: a folder with a link to the 9B's GGUF and no projector");
    expect(nine && nine->role == "vision" && nine->files.size() == 2 && nine->files[1].kind == "mmproj" && nine->context == 8192, "the 9B with its projector is the vision entry at 8k");
    bool base = true, pinned = true;
    for (const auto& e : all) {
        if (e.role == "completion") base = base && e.root == "fim" && e.repo.find("nstruct") == std::string::npos && e.files[0].url.find("nstruct") == std::string::npos;
        for (const auto& f : e.files) pinned = pinned && f.url.find("/resolve/" + e.revision + "/") != std::string::npos && e.revision.size() == 40;
    }
    expect(base, "the completion models are Qwen2.5-Coder base models served from the fim root");
    expect(pinned, "every file is pinned to a 40-character commit");
    const CatalogEntry* seven = find_entry(all, "qwen2.5-coder-7b");
    expect(seven && seven->files[0].size < (5L << 30) && seven->vram.front().second < 6.0, "the 7B coder is a quantisation that fits an 8 GB card");
    expect(seven && vram_line(*seven).find("5.4 GB at 8k") == 0 && vram_line(*seven).find("estimates") != std::string::npos, "its VRAM line is labelled an estimate: " + vram_line(*seven));

    section("API models: limits and prices per model");
    {
        auto apis = parse_api_models(shipped);
        auto find = [&](const std::string& id) -> const ApiModel* {
            for (const auto& a : apis) if (a.id == id) return &a;
            return nullptr;
        };
        const ApiModel* flash = find("deepseek-flash");
        const ApiModel* pro = find("deepseek-v4-pro");
        expect(flash && pro && flash->provider == "deepseek" && pro->model == "deepseek-v4-pro" && flash->context == 1000000 && pro->output == 384000,
               "the catalog holds DeepSeek's two API models with their 1M context and 384K output");
        expect(flash && pro && flash->capabilities.value("vision", false) && !pro->capabilities.value("vision", true), "Flash reads pictures, Pro does not");
        auto price = [](const ApiModel* a, size_t period, const char* k) { return a ? a->pricing["periods"][period].value(k, -1.0) : -1.0; };
        expect(price(flash, 0, "input_cache_miss") == 0.15 && price(flash, 1, "output") == 1.2 && price(pro, 0, "input_cache_hit") == 0.022 && price(pro, 1, "input_cache_miss") == 1.32,
               "off-peak and peak prices per 1M tokens, per model");
        expect(pro && pro->pricing["periods"][1]["utc"] == json::array({"01:00-04:00", "06:00-10:00"}) && pro->pricing["periods"][1]["days"].size() == 5 && pro->checked == "2026-10-02",
               "peak is 01:00-04:00 and 06:00-10:00 UTC on weekdays, with the day the figures were read");
        json broken = {{"api_models", json::array({{{"id", "x"}, {"provider", "p"}, {"model", "m"}, {"brief", "b"}, {"source", "s"}, {"checked", "soon"},
                                                    {"limits", {{"context", 1000}}},
                                                    {"pricing", {{"currency", "USD"}, {"per", 1000000}, {"periods", json::array({{{"name", "a"}, {"input_cache_hit", 1}, {"input_cache_miss", -1}, {"output", 1}},
                                                                                                                       {{"name", "b"}, {"utc", {"1-2"}}, {"input_cache_hit", 1}, {"input_cache_miss", 1}, {"output", 1}}})}}}}})}};
        auto p = joined(check_catalog(shipped, broken));
        expect(p.find("api_models x: checked must be") != std::string::npos && p.find("limits.context and limits.output") != std::string::npos &&
                   p.find("input_cache_miss must be a price") != std::string::npos && p.find("utc ranges are HH:MM-HH:MM") != std::string::npos,
               "check names a bad date, a missing limit, a negative price and a malformed hour range: " + p);
        // One malformed entry is skipped; the others are still read and priced.
        json one_bad = shipped;
        one_bad["api_models"].push_back({{"id", "mangled"}, {"provider", 7}, {"model", "m"}});
        std::vector<std::string> skipped;
        auto rest = parse_api_models(one_bad, &skipped);
        bool priced = std::any_of(rest.begin(), rest.end(), [](const ApiModel& a) { return a.id == "deepseek-flash" && a.pricing.contains("periods"); });
        expect(priced && rest.size() == apis.size() && skipped.size() == 1 && skipped[0].find("'mangled'") != std::string::npos && skipped[0].find("skipped") != std::string::npos,
               "a malformed api_models entry is skipped with a warning naming it; the rest stay priced: " + joined(skipped));
        json bad_entry = shipped;
        bad_entry["models"].push_back({{"id", "mangled"}, {"files", 3}});
        skipped.clear();
        expect(parse_catalog(bad_entry, &skipped).size() == all.size() && skipped.size() == 1 && joined(check_catalog(bad_entry, json::object())).find("'mangled'") != std::string::npos,
               "a malformed catalog entry is skipped, the others load, and maid models check names it");

        // 2026-10-05 is a Monday: 02:00 UTC is peak, 04:00 (the end of the first range) and 12:00 are not; Saturday never is.
        const std::time_t mon_0200 = 1791165600, mon_0400 = 1791172800, mon_1200 = 1791201600, sat_0200 = 1790992800;
        expect(flash && price_period(flash->pricing, mon_0200).value("name", "") == "peak" && price_period(flash->pricing, mon_0400).value("name", "") == "off-peak" &&
                   price_period(flash->pricing, mon_1200).value("name", "") == "off-peak" && price_period(flash->pricing, sat_0200).value("name", "") == "off-peak",
               "the period in force: peak on a weekday inside its UTC hours only");
        // DeepSeek's usage for one call: 120 prompt tokens, 64 of them from the cache, 30 out.
        double off = flash ? call_cost(*flash, 120, 64, 30, mon_1200) : 0, peak = flash ? call_cost(*flash, 120, 64, 30, mon_0200) : 0;
        expect(std::abs(off - (64 * 0.003 + 56 * 0.15 + 30 * 0.6) / 1e6) < 1e-12 && std::abs(peak - (64 * 0.006 + 56 * 0.3 + 30 * 1.2) / 1e6) < 1e-12,
               "a call's cost: cache hits, misses and output each at the period's price (" + std::to_string(off) + ", " + std::to_string(peak) + ")");
        expect(pro && std::abs(call_cost(*pro, 1000000, 0, 0, sat_0200) - 0.66) < 1e-12 && format_cost(0.66, "USD") == "0.6600 USD" && format_cost(12.5, "USD") == "12.50 USD",
               "a million uncached input tokens on Pro off-peak cost its miss price, written to four places under 1");
        expect(find_api_model(api_models(), "deepseek", "deepseek-flash") && !find_api_model(api_models(), "llamacpp", "deepseek-flash"), "API models are found by provider and model");

        expect(session_cost_averages().empty(), "no sessions costed yet, no average");
        add_session_cost("s1", 0.25, "USD");
        add_session_cost("s1", 0.25, "USD");
        add_session_cost("s2", 0.10, "USD");
        add_session_cost("s3", 0, "USD");
        auto avg = session_cost_averages();
        expect(avg.size() == 1 && avg[0].sessions == 2 && std::abs(avg[0].average - 0.30) < 1e-12 && avg[0].currency == "USD",
               "each session's costs add up under its id, and the average runs across the sessions that cost anything");
        std::ifstream costs(state / "maid" / "costs.json");
        expect(costs.good(), "kept in the state directory");
    }

    section("check catches a broken catalog");
    {
        json bad = {{"models", json::array({entry_json("a", "agent", "llamacpp", "A", json::array({file_json("a.gguf", "https://huggingface.co/x/y/resolve/main/a.gguf", "abc", 0)})),
                                            entry_json("a", "agent", "llamacpp", "A", json::array()),
                                            entry_json("b", "wizard", "llamacpp", "B", json::array({file_json("b.gguf", "u", std::string(64, 'a'), 5)})),
                                            entry_json("c", "agent", "llamacpp", "C", json::array({file_json("c.gguf", "u", std::string(64, 'a'), 5)}))})}};
        bad["models"][3]["shares"] = {{"entry", "nowhere"}, {"file", "x.gguf"}};
        bad["models"][2]["vram"] = json::array({{{"context", 8192}, {"gb", 9.9}}});
        auto p = joined(check_catalog(bad, json::object()));
        expect(p.find("'a' appears twice") != std::string::npos, "a duplicate id: " + p);
        expect(p.find("sha256 must be 64") != std::string::npos && p.find("no size") != std::string::npos, "a missing hash and size");
        expect(p.find("not pinned to the revision") != std::string::npos, "a url on main instead of the commit");
        expect(p.find("role 'wizard'") != std::string::npos, "an unknown role");
        expect(p.find("shares.entry 'nowhere'") != std::string::npos, "a share that does not resolve");
        expect(p.find("vram at 8192 tokens says 9.9 GB") != std::string::npos, "a VRAM figure that disagrees with the arithmetic");
    }

    section("user entries merge by id");
    {
        json user = {{"models", json::array({{{"id", "qwen3.5-4b"}, {"context", 32768}, {"notes", "mine"}},
                                             entry_json("my-model", "agent", "llamacpp", "My-1B-Q4", json::array({file_json("m.gguf", "http://x/m.gguf", std::string(64, 'b'), 3)}))})}};
        auto merged = parse_catalog(merge_catalog(shipped, user));
        const CatalogEntry* four = find_entry(merged, "qwen3.5-4b");
        expect(four && four->context == 32768 && four->notes == "mine" && four->files.size() == 2 && !four->brief.empty(), "an override replaces only the fields it names");
        expect(find_entry(merged, "my-model") && merged.size() == all.size() + 1, "an unknown id is added");
        write_file(user_catalog_path(), user.dump(2));
        expect(find_entry(load_catalog(), "my-model") && find_entry(load_catalog(), "qwen3.5-4b")->context == 32768, "load_catalog reads ~/.config/maid/models.json");
        fs::remove(user_catalog_path());
    }

    section("install from a local server");
    httplib::Server srv;
    std::atomic<int> hits{0};
    std::string weights = "GGUF" + std::string(200, 'w'), proj = "GGUF" + std::string(100, 'p'), coder = "GGUF" + std::string(150, 'c');
    for (const auto& [path, body] : std::vector<std::pair<std::string, std::string>>{{"/v.gguf", weights}, {"/mmproj-F16.gguf", proj}, {"/c.gguf", coder}}) {
        std::string b = body;
        srv.Get(path, [b, &hits](const httplib::Request&, httplib::Response& res) {
            ++hits;
            res.set_content(b, "application/octet-stream");
        });
    }
    int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv.listen_after_bind(); });
    srv.wait_until_ready();
    std::string base_url = "http://127.0.0.1:" + std::to_string(port);
    std::string hw = sha_of(weights, ws / "h"), hp = sha_of(proj, ws / "h"), hc = sha_of(coder, ws / "h");
    json user = {{"models", json::array({
        entry_json("t-vision", "vision", "llamacpp", "Tiny-4B-Q4", json::array({file_json("Tiny-4B-Q4.gguf", base_url + "/v.gguf", hw, static_cast<long>(weights.size())),
                                                                                  file_json("mmproj-F16.gguf", base_url + "/mmproj-F16.gguf", hp, static_cast<long>(proj.size()), "mmproj")})),
        entry_json("t-text", "agent", "llamacpp", "Tiny-4B-Q4-text", json::array({file_json("Tiny-4B-Q4-text.gguf", base_url + "/v.gguf", hw, static_cast<long>(weights.size()))})),
        entry_json("t-coder", "completion", "fim", "Tiny-Coder-7B-Q4", json::array({file_json("c.gguf", base_url + "/c.gguf", hc, static_cast<long>(coder.size()))})),
        entry_json("t-bad", "completion", "fim", "Tiny-Bad", json::array({file_json("bad.gguf", base_url + "/c.gguf", std::string(64, '0'), static_cast<long>(coder.size()))}))})}};
    user["models"][1]["shares"] = {{"entry", "t-vision"}, {"file", "Tiny-4B-Q4.gguf"}};
    write_file(user_catalog_path(), user.dump(2));
    all = load_catalog();
    expect(check_catalog(shipped, user).empty(), "the test entries pass check: " + joined(check_catalog(shipped, user)));
    std::ostringstream out;
    bool refused = false;
    try {
        install_entry(*find_entry(all, "t-bad"), all, false, out);
    } catch (const std::exception& e) {
        refused = std::string(e.what()).find("SHA-256 mismatch") != std::string::npos;
    }
    fs::path badir = mdir / "fim" / "Tiny-Bad";
    expect(refused && !fs::exists(badir / "bad.gguf") && !fs::exists(badir / "bad.gguf.part"), "a hash mismatch is refused and nothing is kept");
    // The shared entry is installed first, then the link: relative, and no copy.
    install_entry(*find_entry(all, "t-text"), all, false, out);
    fs::path link = mdir / "llamacpp" / "Tiny-4B-Q4-text" / "Tiny-4B-Q4-text.gguf";
    expect(entry_installed(*find_entry(all, "t-vision"), all) && fs::exists(mdir / "llamacpp" / "Tiny-4B-Q4" / "mmproj-F16.gguf"), "installing the share installed the entry it links first");
    expect(fs::is_symlink(link) && fs::read_symlink(link) == fs::path("../Tiny-4B-Q4/Tiny-4B-Q4.gguf"), "the share is a relative link: " + fs::read_symlink(link).string());
    expect(!fs::exists(link.parent_path() / "mmproj-F16.gguf") && entry_installed(*find_entry(all, "t-text"), all), "with no projector beside it, and it counts as installed");
    int before = hits;
    out.str("");
    install_entry(*find_entry(all, "t-vision"), all, false, out);
    expect(hits == before && out.str().find("present: ") != std::string::npos, "files already there with the right size are not fetched again");
    // A file of the right size placed by hand counts as present too.
    sized(mdir / "fim" / "Tiny-Coder-7B-Q4" / "c.gguf", static_cast<long>(coder.size()));
    out.str("");
    install_entry(*find_entry(all, "t-coder"), all, true, out);
    expect(hits == before && entry_installed(*find_entry(all, "t-coder"), all), "a file of the catalog's size is kept as present, no download");
    expect(fs::read_symlink(fim_model_link()) == fs::path("Tiny-Coder-7B-Q4/c.gguf") && fim_current_id() == "Tiny-Coder-7B-Q4" && entry_current(*find_entry(all, "t-coder")),
           "--link points fim's current.gguf at it, relatively");
    out.str("");
    expect(!verify_entry(*find_entry(all, "t-coder"), out) && out.str().find("MISMATCH") != std::string::npos, "verify hashes the file: the hand-placed one does not match");
    fs::remove(mdir / "fim" / "Tiny-Coder-7B-Q4" / "c.gguf");
    out.str("");
    install_entry(*find_entry(all, "t-coder"), all, false, out);
    expect(hits == before + 1 && verify_entry(*find_entry(all, "t-coder"), out), "once fetched, verify passes");

    section("current links and removal");
    link_entry(*find_entry(all, "t-text"), out);
    expect(llamacpp_current_id() == "Tiny-4B-Q4-text" && entry_current(*find_entry(all, "t-text")) && !entry_current(*find_entry(all, "t-vision")),
           "linking the text entry makes it current, not the folder its link points into");
    expect(remove_blocker(*find_entry(all, "t-text"), all).find("is the current llamacpp model") != std::string::npos, "the current model is not removed");
    link_entry(*find_entry(all, "t-vision"), out);
    std::string why = remove_blocker(*find_entry(all, "t-vision"), all);
    expect(why.find("t-text links t-vision's weights") == 0 && dependents(*find_entry(all, "t-vision"), all) == std::vector<std::string>{"t-text"}, "a shared entry names its dependent: " + why);
    link_entry(*find_entry(all, "t-text"), out);
    refused = false;
    try {
        remove_entry(*find_entry(all, "t-vision"), all, out);
    } catch (const std::exception& e) {
        refused = std::string(e.what()).find("t-text") != std::string::npos;
    }
    expect(refused && fs::exists(mdir / "llamacpp" / "Tiny-4B-Q4" / "Tiny-4B-Q4.gguf"), "remove refuses while the share is installed, and nothing is touched");
    link_entry(*find_entry(all, "t-vision"), out);
    out.str("");
    remove_entry(*find_entry(all, "t-text"), all, out);
    expect(!fs::exists(link.parent_path()) && fs::exists(mdir / "llamacpp" / "Tiny-4B-Q4" / "Tiny-4B-Q4.gguf") && out.str().find("a link;") != std::string::npos,
           "removing the share removes its folder and link only");
    expect(remove_blocker(*find_entry(all, "t-vision"), all).find("current") != std::string::npos, "and the vision entry is now blocked only by being current");
    srv.stop();
    th.join();
    fs::remove(user_catalog_path());
    fs::remove_all(mdir);

    section("a tree shaped like Micaiah's drive");
    all = load_catalog();
    for (const char* id : {"qwen3.5-4b", "qwen3.5-9b"}) {
        const CatalogEntry* e = find_entry(all, id);
        for (const auto& f : e->files) sized(entry_dir(*e) / f.name, f.size);
    }
    fs::create_directories(mdir / "llamacpp" / "Qwen3.5-9B-Q4_K_M-text");
    fs::create_symlink("../Qwen3.5-9B-Q4_K_M/Qwen3.5-9B-Q4_K_M.gguf", mdir / "llamacpp" / "Qwen3.5-9B-Q4_K_M-text" / "Qwen3.5-9B-Q4_K_M-text.gguf");
    expect(entry_installed(*find_entry(all, "qwen3.5-4b"), all) && entry_installed(*find_entry(all, "qwen3.5-9b"), all) && entry_installed(*find_entry(all, "qwen3.5-9b-text"), all),
           "the 4B and 9B folders with their projectors and the -text link all count as installed, no download");
    expect(!entry_installed(*find_entry(all, "qwen2.5-coder-7b"), all) && !entry_installed(*find_entry(all, "whisper-distil-large-v3"), all), "and nothing else does");
    expect(remove_blocker(*find_entry(all, "qwen3.5-9b"), all).find("qwen3.5-9b-text") == 0, "removing the 9B is refused while the text entry is installed");
    std::vector<std::pair<std::string, std::string>> presets;
    for (const auto& p : load_settings().presets) presets.emplace_back(p.name, p.model);
    expect(contains(entry_presets(*find_entry(all, "qwen3.5-9b-text"), presets), "qwen-9b") && contains(entry_presets(*find_entry(all, "qwen3.5-9b"), presets), "qwen-9b-vision") &&
               contains(entry_presets(*find_entry(all, "qwen3.5-4b"), presets), "qwen-4b"),
           "each Qwen3.5 entry names the preset that uses it");
    presets.emplace_back("qwen-4b-side", "llamacpp-2/Qwen3.5-4B-Q4_K_M");
    expect(contains(entry_presets(*find_entry(all, "qwen3.5-4b"), presets), "qwen-4b-side"), "a settings preset on the side server counts too");
    fs::remove_all(mdir);

    section("the code completion service");
    auto defs = load_services(root_dir() / "services");
    const ServiceDef* fim = nullptr;
    for (const auto& d : defs) {
        if (d.name == "llamacpp-fim") fim = &d;
    }
    expect(fim && fim->port == 8084 && fim->needs_gpu && !fim->ready_pattern.empty(), "services/llamacpp-fim.json: port 8084, needs_gpu, a ready_pattern");
    bool router = false, downloads = false, local = true;
    if (fim) {
        for (size_t i = 0; i < fim->command.size(); ++i) {
            const std::string& a = fim->command[i];
            if (a == "--models-dir" && i + 1 < fim->command.size()) router = fs::path(fim->command[i + 1]).filename() == "fim";
            downloads = downloads || a.rfind("--fim-qwen", 0) == 0 || a == "-hf" || a == "--hf-repo";
            if (a == "--host" && i + 1 < fim->command.size()) local = fim->command[i + 1] == "127.0.0.1";
        }
    }
    expect(router && !downloads && local, "router mode over <models_dir>/fim on loopback, with no preset that downloads");
    expect(is_llama_server("llamacpp-fim") && is_fim_server("llamacpp-fim") && !is_fim_server("llamacpp-2"), "it is a llama server, so GPU turn-taking unloads it like the others");
    expect(fim && missing_requirement(*fim).find("maid models install qwen2.5-coder-7b --link") != std::string::npos, "without a linked coder, maid up says how to get one");
    Provider p;
    p.name = "fim";
    p.base_url = "http://127.0.0.1:8084/v1";
    std::string hint = fim ? unreachable_hint(p, {*fim}) : "";
    expect(hint.find("llamacpp-fim is not running: maid up llamacpp-fim (needs a completion model") == 0, "something pointed at 8084 gets the same hint: " + hint);
    if (fim) {
        GpuReport g = gpu_report({*fim});
        expect(g.servers.size() == 1 && g.servers[0].name == "llamacpp-fim" && g.servers[0].context == 8192 && !g.servers[0].running, "maid gpu lists it, with its --ctx-size");
    }
    sized(mdir / "fim" / "Qwen2.5-Coder-7B-Q4_K_M" / "Qwen2.5-Coder-7B.i1-Q4_K_M.gguf", find_entry(all, "qwen2.5-coder-7b")->files[0].size);
    GpuReport g;
    g.servers = {{"llamacpp-fim", true, {"Qwen2.5-Coder-7B-Q4_K_M"}, 8192}};
    std::string t = g.text();
    expect(t.find("llamacpp-fim: Qwen2.5-Coder-7B-Q4_K_M loaded (maid gpu free llamacpp-fim unloads") != std::string::npos, "the report names what it holds: " + t);
    g.servers[0].linked = "Qwen2.5-Coder-7B-Q4_K_M";
    g.servers[0].models.clear();
    t = g.text();
    expect(t.find("llamacpp-fim: Qwen2.5-Coder-7B-Q4_K_M unloaded (maid gpu load llamacpp-fim)") != std::string::npos, "an unloaded coder says how to load it: " + t);
    g.servers[0].linked.clear();
    t = g.text();
    expect(t.find("llamacpp-fim: running, not linked (maid models install qwen2.5-coder-7b --link)") != std::string::npos, "and with no coder linked, how to link one: " + t);
    g.servers[0].models = {"Qwen2.5-Coder-7B-Q4_K_M"};
    Settings s;
    std::string fit = gpu_budget(g, s, 8L << 30);
    expect(fit == "completion 7B at 8k (5.4 GB est.) = 5.4 GB of 8.0 GB: fits with ComfyUI stopped", "the budget sentence counts it: " + fit);
    g.servers[0].running = false;
    g.servers[0].models.clear();
    expect(gpu_budget(g, s, 8L << 30).empty(), "and leaves it out when it is not running");
    ServiceDef dead = fim ? *fim : ServiceDef{};
    dead.name = "llamacpp-fim";
    fs::create_directories(service_log_path(dead).parent_path());
    write_file(service_log_path(dead), "ggml_backend_cuda_buffer_type_alloc_buffer: allocating 4466.00 MiB on device 0: cudaMalloc failed: out of memory\n");
    expect(explain_exit(dead, {dead}).find("maid models install qwen2.5-coder-3b --link") != std::string::npos, "an out of memory suggests a smaller coder");
    fs::remove(service_log_path(dead));

    section("the completion server loads only when MAID asks");
    expect(fim && contains(fim->command, "--no-models-autoload"), "services/llamacpp-fim.json runs with --no-models-autoload, so llama.vim's requests never load the coder");
    {
        // A router like the vendored llama-server with autoload off: an infill for a model that is not loaded is
        // refused, and only POST /models/load loads it.
        httplib::Server fr, cf;
        std::atomic<bool> loaded{true}, fail_load{false};
        std::atomic<int> loads{0}, unloads{0}, refused{0}, frees{0};
        fr.Get("/v1/models", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(json{{"data", {{{"id", "current"}, {"status", {{"value", loaded ? "loaded" : "unloaded"}}}}}}}.dump(), "application/json");
        });
        fr.Post("/models/load", [&](const httplib::Request&, httplib::Response& res) {
            if (fail_load || loaded) {
                res.status = 400;
                res.set_content(json{{"error", {{"message", fail_load ? "model limit reached, try again later" : "model is already running"}}}}.dump(), "application/json");
                return;
            }
            ++loads;
            loaded = true;
            res.set_content(R"({"success":true})", "application/json");
        });
        fr.Post("/models/unload", [&](const httplib::Request&, httplib::Response& res) {
            ++unloads;
            loaded = false;
            res.set_content(R"({"success":true})", "application/json");
        });
        fr.Post("/infill", [&](const httplib::Request&, httplib::Response& res) {
            if (!loaded) {
                ++refused;
                res.status = 400;
                res.set_content(R"({"error":{"message":"model is not loaded"}})", "application/json");
                return;
            }
            res.set_content(R"({"content":"x"})", "application/json");
        });
        cf.Post("/free", [&](const httplib::Request&, httplib::Response& res) {
            ++frees;
            res.set_content("{}", "application/json");
        });
        int fr_port = fr.bind_to_any_port("127.0.0.1"), cf_port = cf.bind_to_any_port("127.0.0.1");
        std::thread tfr([&] { fr.listen_after_bind(); }), tcf([&] { cf.listen_after_bind(); });
        fr.wait_until_ready();
        cf.wait_until_ready();
        ServiceDef fim_def = *fim, comfy_def, whisper_def;
        fim_def.port = fr_port;
        comfy_def.name = "comfyui";
        comfy_def.needs_gpu = true;
        comfy_def.port = cf_port;
        whisper_def.name = "whisper";
        whisper_def.needs_gpu = true;
        whisper_def.port = fr_port;  // only its pid file matters here
        fs::path run = state / "maid" / "run";
        for (const char* name : {"llamacpp-fim", "comfyui"}) write_file(run / (std::string(name) + ".pid"), self_pid_line());
        fs::create_symlink("Qwen2.5-Coder-7B-Q4_K_M/Qwen2.5-Coder-7B.i1-Q4_K_M.gguf", fim_model_link());
        std::vector<ServiceDef> svc = {fim_def, comfy_def, whisper_def};
        auto infill = [&] { return httplib::Client("127.0.0.1", fr_port).Post("/infill", R"({"model":"current","input_prefix":"int ","input_suffix":""})", "application/json"); };

        std::string t = gpu_report(svc).text();
        expect(t.find("llamacpp-fim: Qwen2.5-Coder-7B-Q4_K_M loaded (maid gpu free llamacpp-fim unloads it until maid gpu load llamacpp-fim)") != std::string::npos, "maid gpu shows the coder loaded: " + t);
        std::string freed = free_gpu_for(comfy_def, svc);
        expect(unloads == 1 && !loaded && freed.find("from llamacpp-fim") != std::string::npos, "starting ComfyUI unloads the coder: " + freed);
        auto r = infill();
        expect(r && r->status == 400 && refused == 1 && loads == 0 && !loaded, "an infill request afterwards is refused and loads nothing");
        GpuReport g2 = gpu_report(svc);
        t = g2.text();
        expect(t.find("llamacpp-fim: Qwen2.5-Coder-7B-Q4_K_M unloaded (maid gpu load llamacpp-fim)") != std::string::npos, "maid gpu shows it unloaded and how to load it: " + t);
        expect(gpu_budget(g2, Settings{}, 8L << 30).find("completion") == std::string::npos, "an unloaded coder is not counted on the card");
        std::string back = restore_gpu_after(comfy_def, svc);
        expect(loads == 1 && loaded && back == "llamacpp-fim: loaded Qwen2.5-Coder-7B-Q4_K_M again (comfyui had unloaded it)", "maid down comfyui loads it again, since it was loaded before: " + back);
        r = infill();
        expect(r && r->status == 200, "and completions work again");

        gpu_free(svc, "llamacpp-fim");
        expect(unloads == 2 && !loaded, "maid gpu free llamacpp-fim unloads it");
        free_gpu_for(comfy_def, svc);
        expect(restore_gpu_after(comfy_def, svc).empty() && loads == 1 && !loaded, "maid down comfyui leaves it unloaded when it was not loaded before ComfyUI started");
        std::string loaded_msg = gpu_load(svc, "llamacpp-fim");
        expect(loads == 2 && loaded && loaded_msg == "llamacpp-fim: loaded Qwen2.5-Coder-7B-Q4_K_M\n", "maid gpu load llamacpp-fim loads it: " + loaded_msg);
        bool threw = false;
        try { gpu_load(svc, "llamacpp"); } catch (const std::exception& e) { threw = std::string(e.what()).find("maid gpu load llamacpp-fim") == 0; }
        expect(threw, "the chat servers load on demand, so gpu load names only llamacpp-fim");

        // Two services unloaded it: it comes back when the last of them stops.
        write_file(run / "whisper.pid", self_pid_line());
        free_gpu_for(whisper_def, svc);
        free_gpu_for(comfy_def, svc);
        expect(unloads == 3 && !loaded && frees >= 1, "starting whisper unloads it (and asks ComfyUI to unload), starting ComfyUI after that finds it unloaded");
        expect(restore_gpu_after(whisper_def, svc).empty() && loads == 2, "stopping whisper while ComfyUI still runs leaves it unloaded");
        expect(restore_gpu_after(comfy_def, svc).find("again (comfyui had unloaded it)") != std::string::npos && loads == 3 && loaded, "stopping ComfyUI then loads it");

        // A relink (maid models install --link, maid vendor use) unloads the old coder and loads the new one.
        sized(mdir / "fim" / "Qwen2.5-Coder-3B-Q8_0" / "Qwen2.5-Coder-3B-Q8_0.gguf", 4096);
        vendor_use(*find_vendor("llamacpp"), mdir / "fim" / "Qwen2.5-Coder-3B-Q8_0" / "Qwen2.5-Coder-3B-Q8_0.gguf");
        expect(fs::read_symlink(fim_model_link()) == fs::path("Qwen2.5-Coder-3B-Q8_0/Qwen2.5-Coder-3B-Q8_0.gguf") && fim_current_id() == "Qwen2.5-Coder-3B-Q8_0",
               "maid vendor use llamacpp on a GGUF under the fim root relinks llamacpp-fim's current.gguf");
        std::string re = reload_fim(svc);
        expect(unloads == 4 && loads == 4 && loaded && re == "llamacpp-fim: loaded Qwen2.5-Coder-3B-Q8_0 (the new link; the previous coder was unloaded)\n", "relinking reloads it: " + re);
        gpu_free(svc, "llamacpp-fim");
        re = reload_fim(svc);
        expect(loads == 4 && !loaded && re.find("holds no coder now; maid gpu load llamacpp-fim") != std::string::npos, "a relink while it is unloaded leaves it unloaded: " + re);

        fail_load = true;
        std::string why;
        try { gpu_load(svc, "llamacpp-fim"); } catch (const std::exception& e) { why = e.what(); }
        expect(why.find("llamacpp-fim could not load Qwen2.5-Coder-3B-Q8_0: model limit reached") == 0 && why.find("qwen2.5-coder-3b --link") != std::string::npos,
               "a failed load says why, with the card and a smaller coder: " + why);
        restore_gpu_after(fim_def, svc);
        expect(!fs::exists(run / "llamacpp-fim.evicted"), "stopping llamacpp-fim forgets who unloaded it");
        fr.stop();
        cf.stop();
        tfr.join();
        tcf.join();
        fs::remove_all(run);
    }

    fs::remove_all(ws);
    return finish();
}
