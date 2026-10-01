// The model catalog: parsing and `check`, user overrides by id, installs from a local server with real hashes,
// shares, removal rules, verify, the installed column on a tree shaped like Micaiah's drive, and the code
// completion service's place in `maic gpu`.
#include "check.hpp"

#include "maic/http.hpp"
#include "maic/models.hpp"
#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/settings.hpp"
#include "maic/status.hpp"
#include "maic/vendor.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace maic;
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

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : " | ") + s;
    return out;
}

}  // namespace

int main() {
    setenv("MAIC_TRIPWIRE_FILE", ("/tmp/maic-test-tripwire-" + std::to_string(getpid()) + ".none").c_str(), 1);
    fs::path ws = fs::path(std::getenv("HOME")) / ".cache" / ("maic-models-test-" + std::to_string(getpid()));
    fs::remove_all(ws);
    fs::path cfg = ws / "config", state = ws / "state", mdir = ws / "models";
    fs::create_directories(cfg / "maic");
    setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
    setenv("XDG_STATE_HOME", state.c_str(), 1);
    write_file(cfg / "maic" / "settings.lua", "return { models_dir = '" + mdir.string() + "' }");

    section("the shipped catalog");
    std::ifstream in(catalog_path());
    json shipped = json::parse(in, nullptr, false, true);
    expect(!shipped.is_discarded() && shipped["models"].is_array(), "models/catalog.json parses");
    auto problems = check_catalog(shipped, json::object());
    expect(problems.empty(), "maic models check finds nothing wrong with it: " + joined(problems));
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
        expect(find_entry(load_catalog(), "my-model") && find_entry(load_catalog(), "qwen3.5-4b")->context == 32768, "load_catalog reads ~/.config/maic/models.json");
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
    expect(fim && missing_requirement(*fim).find("maic models install qwen2.5-coder-7b --link") != std::string::npos, "without a linked coder, maic up says how to get one");
    Provider p;
    p.name = "fim";
    p.base_url = "http://127.0.0.1:8084/v1";
    std::string hint = fim ? unreachable_hint(p, {*fim}) : "";
    expect(hint.find("llamacpp-fim is not running: maic up llamacpp-fim (needs a completion model") == 0, "something pointed at 8084 gets the same hint: " + hint);
    if (fim) {
        GpuReport g = gpu_report({*fim});
        expect(g.servers.size() == 1 && g.servers[0].name == "llamacpp-fim" && g.servers[0].context == 8192 && !g.servers[0].running, "maic gpu lists it, with its --ctx-size");
    }
    sized(mdir / "fim" / "Qwen2.5-Coder-7B-Q4_K_M" / "Qwen2.5-Coder-7B.i1-Q4_K_M.gguf", find_entry(all, "qwen2.5-coder-7b")->files[0].size);
    GpuReport g;
    g.servers = {{"llamacpp-fim", true, {"Qwen2.5-Coder-7B-Q4_K_M"}, 8192}};
    std::string t = g.text();
    expect(t.find("llamacpp-fim: holds Qwen2.5-Coder-7B-Q4_K_M (maic gpu free llamacpp-fim unloads") != std::string::npos, "the report names what it holds: " + t);
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
    expect(explain_exit(dead, {dead}).find("maic models install qwen2.5-coder-3b --link") != std::string::npos, "an out of memory suggests a smaller coder");
    fs::remove(service_log_path(dead));

    fs::remove_all(ws);
    return finish();
}
