#include "maic/status.hpp"

#include "maic/http.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "maic/helper.hpp"
#include "maic/paths.hpp"
#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <algorithm>
#include <filesystem>

namespace maic {

namespace fs = std::filesystem;

namespace {
const ServiceDef* by_name(const std::vector<ServiceDef>& services, const std::string& name) {
    for (const auto& s : services) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
std::string gib(long bytes) {
    char b[32];
    snprintf(b, sizeof(b), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    return b;
}
std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}
std::string local_url(const ServiceDef& def) {
    return "http://127.0.0.1:" + std::to_string(def.port);
}

// ComfyUI's view of the card from /system_stats: used and total in bytes, -1 when it does not say.
std::pair<long, long> comfyui_vram(const std::string& base_url) {
    httplib::Client c(base_url);
    c.set_connection_timeout(2);
    c.set_read_timeout(5);
    auto res = c.Get("/system_stats");
    if (!res || res->status != 200) return {-1, -1};
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    for (const auto& d : j.value("devices", nlohmann::json::array())) {
        long total = d.value("vram_total", 0L), free = d.value("vram_free", 0L);
        if (total > 0) return {total - free, total};
    }
    return {-1, -1};
}

// "queue idle", or "queue: 1 running, 2 pending"; "" when /queue does not answer.
std::string comfyui_queue(const std::string& base_url) {
    httplib::Client c(base_url);
    c.set_connection_timeout(2);
    c.set_read_timeout(5);
    auto res = c.Get("/queue");
    if (!res || res->status != 200) return "";
    auto j = nlohmann::json::parse(res->body, nullptr, false);
    size_t running = j.value("queue_running", nlohmann::json::array()).size(), pending = j.value("queue_pending", nlohmann::json::array()).size();
    if (!running && !pending) return "queue idle";
    return "queue: " + std::to_string(running) + " running, " + std::to_string(pending) + " pending";
}

// ComfyUI's /free: unload its models and release cached memory; it reloads them on its next run.
bool comfyui_free(const ServiceDef& def) {
    httplib::Client c(local_url(def));
    c.set_connection_timeout(2);
    c.set_read_timeout(30);
    auto res = c.Post("/free", R"({"unload_models":true,"free_memory":true})", "application/json");
    return res && res->status == 200;
}

// One model's state on a router: "loaded", "loading", "unloaded" or "failed"; "" when the server does not answer
// or lists no such model.
std::string router_state(const std::string& base_url, const std::string& id) {
    httplib::Client c(base_url);
    c.set_connection_timeout(2);
    c.set_read_timeout(5);
    auto r = c.Get("/v1/models");
    if (!r || r->status != 200) return "";
    auto j = nlohmann::json::parse(r->body, nullptr, false);
    if (!j.is_object()) return "";
    for (const auto& m : j.value("data", nlohmann::json::array())) {
        if (m.value("id", "") != id) continue;
        auto st = m.value("status", nlohmann::json::object());
        return st.value("failed", false) ? "failed" : st.value("value", "");
    }
    return "";
}

// The services that unloaded the completion server's coder while it was loaded, one per line; when the last of
// them stops through MAIC, the coder is loaded again.
fs::path fim_evicted_path() {
    return state_dir() / "run" / "llamacpp-fim.evicted";
}

std::vector<std::string> fim_evictors() {
    std::vector<std::string> out;
    std::ifstream in(fim_evicted_path());
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

void set_fim_evictors(const std::vector<std::string>& names) {
    std::error_code ec;
    if (names.empty()) {
        fs::remove(fim_evicted_path(), ec);
        return;
    }
    fs::create_directories(fim_evicted_path().parent_path(), ec);
    std::ofstream out(fim_evicted_path(), std::ios::trunc);
    for (const auto& n : names) out << n << "\n";
}

std::string first_line(const std::string& command) {
    std::string out;
    run_helper(command, &out);
    out = out.substr(0, out.find('\n'));
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}
}  // namespace

std::string service_detail(const ServiceDef& def, const ServiceStatus& status) {
    if (!status.port_open) return "";
    if (is_llama_server(def.name)) {
        auto models = resident_models(local_url(def));
        return models.empty() ? "no model resident" : "model: " + joined(models);
    }
    if (def.name == "comfyui") {
        auto [used, total] = comfyui_vram(local_url(def));
        std::string out = total > 0 ? "VRAM " + gib(used) + " used of " + gib(total) : "";
        std::string queue = comfyui_queue(local_url(def));
        if (!queue.empty()) out += (out.empty() ? "" : "; ") + queue;
        return out;
    }
    return "";
}

std::vector<ServiceReport> service_reports(const std::vector<ServiceDef>& services) {
    std::vector<ServiceReport> out;
    for (const auto& def : services) {
        ServiceReport r;
        r.name = def.name;
        r.runtime = def.runtime;
        r.log = service_log_path(def).string();
        r.gpu = def.needs_gpu || def.gpu;
        ServiceStatus st = service_status(def);
        r.url = def.port ? "http://127.0.0.1:" + std::to_string(def.port) : "";
        const std::string& url = r.url;
        switch (st.state) {
            case ServiceState::Running:
                r.state = st.port_open ? "running" : "starting";
                r.who = st.who();
                r.where = r.who + (url.empty() ? "" : " · " + url);
                r.detail = service_detail(def, st);
                r.actions = {"maic down " + def.name, "maic logs " + def.name};
                break;
            case ServiceState::Foreign:
                r.state = "foreign";
                r.where = "port " + std::to_string(def.port) + " is held by a process MAIC did not start";
                r.actions = {"stop that process yourself, then: maic up " + def.name};
                break;
            case ServiceState::Stopped:
                r.state = st.crashed ? "failed" : "stopped";
                r.where = url;
                r.actions = st.crashed ? std::vector<std::string>{"maic logs " + def.name, "maic up " + def.name} : std::vector<std::string>{"maic up " + def.name};
                break;
        }
        out.push_back(r);
    }
    return out;
}

StatusReport status_report(const std::vector<ServiceDef>& services) {
    StatusReport rep;
    if (auto lock = tripwire_state()) {
        rep.tripped = true;
        rep.tripwire = *lock;
        rep.actions.push_back("maic unlock   (asks for your sudo password)");
    }
    rep.services = service_reports(services);
    return rep;
}

std::string format_status(const StatusReport& rep, const StatusPaint* paint) {
    auto in = [&](const std::string& text, const char* role) { return paint ? ansi_paint(text, paint->settings.style(role), paint->depth) : text; };
    auto state_role = [](const std::string& state) {
        if (state == "running") return "harness_armed";
        if (state == "starting") return "notice";
        if (state == "stopped") return "status_dim";
        if (state == "failed") return "error";
        return "";
    };
    std::string out = rep.tripped ? in("harness: TRIPPED", "harness_tripped") + "\n" + rep.tripwire : in("harness: armed", "harness_armed") + "\n";
    for (const auto& a : rep.actions) out += "  -> " + a + "\n";
    for (const auto& s : rep.services) {
        out += s.name + ": " + (paint && *state_role(s.state) ? in(s.state, state_role(s.state)) : s.state) + " [" + s.runtime + "]" + (s.where.empty() ? "" : "  " + s.where) +
               (s.gpu ? "  " + in("GPU", "focus") : "") + "\n";
        if (!s.detail.empty()) out += "     " + s.detail + "\n";
        for (const auto& a : s.actions) out += "  -> " + a + "\n";
    }
    if (!rep.lazy_lock.empty()) out += "nvim lazy-lock: " + rep.lazy_lock + "\n";
    return out;
}

std::string format_status_records(const StatusReport& rep) {
    auto field = [](const std::string& s) { return s.empty() ? std::string("-") : s; };
    std::string out = std::string("harness\t") + (rep.tripped ? "tripped" : "armed") + "\n";
    for (const auto& s : rep.services) {
        out += "service\t" + s.name + "\t" + s.state + "\t" + s.runtime + "\t" + (s.gpu ? "gpu" : "cpu") + "\t" + field(s.who) + "\t" + field(s.url) + "\t" + field(s.detail) + "\n";
    }
    if (!rep.lazy_lock.empty()) out += "lazy-lock\t" + rep.lazy_lock + "\n";
    return out;
}

std::string missing_requirement(const ServiceDef& def) {
    std::error_code ec;
    for (const auto& path : def.requires_paths) {
        if (std::filesystem::exists(path, ec)) continue;
        if (is_fim_server(def.name)) {
            return def.name + " needs a completion model: maic models install qwen2.5-coder-7b --link (or the 3b or 1.5b) links " + path.string();
        }
        if (is_llama_server(def.name)) {
            return def.name + " needs a models directory: put a GGUF under " + path.string() + " or run maic vendor model llamacpp URL SHA256 (models_dir in settings moves it)";
        }
        if (def.name == "whisper" && path.filename().string().rfind("ggml-silero", 0) == 0) {
            return "whisper needs its VAD model: maic vendor model whisper URL SHA256 puts " + path.filename().string() + " at " + path.parent_path().string() + " (docs/diction.md has both)";
        }
        if (def.name == "whisper") {
            return "whisper needs a model: maic vendor use whisper FILE, or maic vendor model whisper URL SHA256 (docs/diction.md names one; " + path.string() + " is the link)";
        }
        return def.name + " needs " + path.string() + " (is the drive mounted?)";
    }
    return "";
}

std::vector<std::string> resident_models(const std::string& base_url) {
    httplib::Client c(base_url);
    c.set_connection_timeout(2);
    c.set_read_timeout(5);
    auto r = c.Get("/v1/models");
    std::vector<std::string> out;
    if (!r || r->status != 200) return out;
    auto j = nlohmann::json::parse(r->body, nullptr, false);
    for (const auto& m : j.value("data", nlohmann::json::array())) {
        if (m.value("status", nlohmann::json::object()).value("value", "") == "loaded" && m.contains("id")) out.push_back(m["id"].get<std::string>());
    }
    return out;
}

std::vector<std::string> unload_resident(const std::string& base_url) {
    std::vector<std::string> done;
    httplib::Client c(base_url);
    c.set_connection_timeout(2);
    c.set_read_timeout(30);
    for (const auto& id : resident_models(base_url)) {
        auto r = c.Post("/models/unload", nlohmann::json{{"model", id}}.dump(), "application/json");
        if (r && r->status == 200) done.push_back(id);
    }
    return done;
}

bool is_llama_server(const std::string& service_name) {
    return service_name == "llamacpp" || service_name.rfind("llamacpp-", 0) == 0;
}

bool is_fim_server(const std::string& service_name) {
    return service_name == "llamacpp-fim";
}

std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    if (!def.needs_gpu || is_llama_server(def.name)) return "";
    std::string out;
    for (const auto& other : services) {
        if (!is_llama_server(other.name) || service_status(other).state != ServiceState::Running) continue;
        auto freed = unload_resident(local_url(other));
        if (is_fim_server(other.name)) {
            // The coder stays unloaded (no autoload); remember who owes it back.
            auto owed = fim_evictors();
            if ((!freed.empty() || !owed.empty()) && std::find(owed.begin(), owed.end(), def.name) == owed.end()) owed.push_back(def.name);
            set_fim_evictors(owed);
        }
        if (freed.empty()) continue;
        out += (out.empty() ? "unloaded " : "; ") + joined(freed) + " from " + other.name;
    }
    if (const auto* cf = by_name(services, "comfyui"); cf && def.name != "comfyui" && service_status(*cf).state == ServiceState::Running && comfyui_free(*cf)) {
        out += std::string(out.empty() ? "" : "; ") + "asked comfyui to unload its models";
    }
    if (!out.empty()) out += " to free the GPU for " + def.name + " (they reload on the next request)";
    if (const auto* w = by_name(services, "whisper"); w && def.name != "whisper" && service_status(*w).state == ServiceState::Running) {
        out += std::string(out.empty() ? "" : "; ") + "whisper still holds its model (maic down whisper releases it)";
    }
    return out;
}

GpuReport gpu_report(const std::vector<ServiceDef>& services) {
    GpuReport r;
    for (const auto& def : services) {
        if (!is_llama_server(def.name)) continue;
        GpuReport::Server s;
        s.name = def.name;
        s.running = service_status(def).state == ServiceState::Running;
        if (s.running) s.models = resident_models(local_url(def));
        if (is_fim_server(def.name)) {
            for (size_t i = 0; i + 1 < def.command.size(); ++i) {
                if (def.command[i] == "--ctx-size" || def.command[i] == "-c") s.context = std::atoi(def.command[i + 1].c_str());
            }
            s.linked = fim_current_id();
            for (auto& m : s.models) {
                if (m == "current" && !s.linked.empty()) m = s.linked;
            }
        }
        r.servers.push_back(s);
    }
    if (const auto* cf = by_name(services, "comfyui"); cf && service_status(*cf).state == ServiceState::Running) {
        r.comfyui_running = true;
        auto [used, total] = comfyui_vram(local_url(*cf));
        r.comfyui_vram_used = used;
        r.comfyui_vram_total = total;
    }
    if (const auto* w = by_name(services, "whisper")) {
        r.has_whisper = true;
        r.whisper_running = service_status(*w).state == ServiceState::Running;
        auto e = find_vendor("whisper");
        std::error_code ec;
        if (e && fs::is_symlink(vendor_model_link(*e), ec)) {
            fs::path target = fs::weakly_canonical(vendor_model_link(*e), ec);
            r.whisper_model = target.string();
            long n = static_cast<long>(fs::file_size(target, ec));
            if (!ec) r.whisper_bytes = n;
        }
    }
    r.card_total = r.comfyui_vram_total;
    if (r.card_total <= 0) {
        // nvidia-smi reports MiB; a missing or broken nvidia-smi prints nothing.
        long mib = std::atol(first_line("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1").c_str());
        if (mib > 0) r.card_total = mib * 1024L * 1024L;
    }
    return r;
}

std::string GpuReport::records() const {
    auto field = [](const std::string& s) { return s.empty() ? std::string("-") : s; };
    auto state = [](bool running) { return running ? "running" : "stopped"; };
    std::string out;
    for (const auto& s : servers) out += "server\t" + s.name + "\t" + state(s.running) + "\t" + field(joined(s.models)) + "\t" + std::to_string(s.context) + "\t" + field(s.linked) + "\n";
    out += std::string("comfyui\t") + state(comfyui_running) + "\t" + std::to_string(comfyui_vram_used) + "\t" + std::to_string(comfyui_vram_total) + "\n";
    if (has_whisper) out += std::string("whisper\t") + state(whisper_running) + "\t" + field(whisper_model) + "\t" + std::to_string(whisper_bytes) + "\n";
    return out + "card\t" + std::to_string(card_total) + "\n";
}

std::string GpuReport::text() const {
    std::string out;
    for (const auto& s : servers) {
        out += s.name + ": ";
        if (!s.running) out += "not running\n";
        else if (is_fim_server(s.name) && !s.models.empty()) out += joined(s.models) + " loaded (maic gpu free " + s.name + " unloads it until maic gpu load " + s.name + ")\n";
        else if (is_fim_server(s.name) && !s.linked.empty()) out += s.linked + " unloaded (maic gpu load " + s.name + ")\n";
        else if (is_fim_server(s.name)) out += "running, not linked (maic models install qwen2.5-coder-7b --link)\n";
        else if (s.models.empty()) out += "running, no model resident\n";
        else out += "holds " + joined(s.models) + " (maic gpu free " + s.name + " unloads; it reloads on the next request)\n";
    }
    if (comfyui_running) {
        out += "comfyui: running";
        if (comfyui_vram_total > 0) out += ", the card as it sees it: " + gib(comfyui_vram_used) + " used of " + gib(comfyui_vram_total);
        out += " (maic gpu free comfyui unloads its models and caches)\n";
    } else {
        out += "comfyui: not running\n";
    }
    if (has_whisper) {
        std::string model = fs::path(whisper_model).filename().string() + (whisper_bytes >= 0 ? " (" + gib(whisper_bytes) + ")" : "");
        if (whisper_model.empty()) out += "whisper: " + std::string(whisper_running ? "running" : "not running") + ", no model linked (maic vendor use whisper FILE)\n";
        else if (whisper_running) out += "whisper: running, holds " + model + " (maic down whisper releases it)\n";
        else out += "whisper: not running; its model is " + model + "\n";
    }
    return out;
}

std::string gpu_free(const std::vector<ServiceDef>& services, const std::string& what) {
    std::string out;
    bool known = what == "all" || what == "comfyui" || what == "whisper";
    for (const auto& def : services) {
        if (!is_llama_server(def.name) || (what != "all" && what != def.name)) continue;
        known = true;
        if (service_status(def).state == ServiceState::Running) {
            auto freed = unload_resident(local_url(def));
            out += def.name + ": " + (freed.empty() ? "nothing was resident" : "unloaded " + joined(freed)) + "\n";
        } else if (what == def.name) {
            out += def.name + ": not running\n";
        }
    }
    if (what == "all" || what == "comfyui") {
        if (const auto* cf = by_name(services, "comfyui"); cf && service_status(*cf).state == ServiceState::Running) {
            out += comfyui_free(*cf) ? "comfyui: models unloaded and caches released\n" : "comfyui: /free failed\n";
        } else if (what == "comfyui") {
            out += "comfyui: not running\n";
        }
    }
    if (what == "all" || what == "whisper") {
        // whisper-server has no unload: it holds its model for as long as it runs.
        if (const auto* w = by_name(services, "whisper"); w && service_status(*w).state == ServiceState::Running) {
            out += "whisper: holds its model while it runs; maic down whisper releases it\n";
        } else if (what == "whisper") {
            out += "whisper: not running\n";
        }
    }
    if (!known) throw std::runtime_error("maic gpu free [all|llamacpp|llamacpp-2|llamacpp-fim|whisper|comfyui]");
    return out;
}

std::string load_fim(const ServiceDef& def, const std::vector<ServiceDef>& services, std::chrono::seconds timeout) {
    std::string id = fim_current_id();
    if (id.empty()) throw std::runtime_error(def.name + " has no coder linked: maic models install qwen2.5-coder-7b --link (or the 3b or 1.5b)");
    std::string url = local_url(def), why;
    std::string state = router_state(url, "current");
    if (state != "loaded" && state != "loading") {
        httplib::Client c(url);
        c.set_connection_timeout(2);
        c.set_read_timeout(30);
        auto r = c.Post("/models/load", nlohmann::json{{"model", "current"}}.dump(), "application/json");
        if (!r) {
            why = "it did not answer on port " + std::to_string(def.port);
        } else if (r->status != 200) {
            auto j = nlohmann::json::parse(r->body, nullptr, false);
            why = j.is_object() && j.contains("error") && j["error"].is_object() ? j["error"].value("message", r->body) : r->body;
        }
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (why.empty()) {
        state = router_state(url, "current");
        if (state == "loaded") {
            set_fim_evictors({});
            return def.name + ": loaded " + id;
        }
        if (state == "failed" || state == "unloaded") why = "it exited while loading (maic logs " + def.name + ")";
        else if (std::chrono::steady_clock::now() >= deadline) why = "it was not loaded after " + std::to_string(timeout.count()) + " s";
        else std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::string fit = gpu_budget(gpu_report(services), load_settings());
    throw std::runtime_error(def.name + " could not load " + id + ": " + why + ". " + (fit.empty() ? "maic gpu shows who holds the card" : "On the card: " + fit) +
                             "; a smaller coder needs less (maic models install qwen2.5-coder-3b --link)");
}

std::string gpu_load(const std::vector<ServiceDef>& services, const std::string& what) {
    if (!is_fim_server(what)) throw std::runtime_error("maic gpu load llamacpp-fim (the other llama servers load their model on the next request)");
    const auto* def = by_name(services, what);
    if (!def) throw std::runtime_error("there is no " + what + " service (services/llamacpp-fim.json)");
    if (service_status(*def).state != ServiceState::Running) return what + ": not running (maic up " + what + " starts it and loads its coder)\n";
    return load_fim(*def, services) + "\n";
}

std::string reload_fim(const std::vector<ServiceDef>& services) {
    const auto* def = by_name(services, "llamacpp-fim");
    if (!def || service_status(*def).state != ServiceState::Running) return "";
    std::string url = local_url(*def);
    auto was = unload_resident(url);
    if (was.empty()) return def->name + " holds no coder now; maic gpu load " + def->name + " loads the new link\n";
    // The router unloads in the background; a load while the old one still runs is refused.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!resident_models(url).empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return load_fim(*def, services) + " (the new link; the previous coder was unloaded)\n";
}

std::string restore_gpu_after(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    auto owed = fim_evictors();
    if (is_fim_server(def.name)) {
        set_fim_evictors({});
        return "";
    }
    auto it = std::find(owed.begin(), owed.end(), def.name);
    if (it == owed.end()) return "";
    owed.erase(it);
    set_fim_evictors(owed);
    if (!owed.empty()) return "";
    const auto* fim = by_name(services, "llamacpp-fim");
    if (!fim || service_status(*fim).state != ServiceState::Running) return "";
    return load_fim(*fim, services) + " again (" + def.name + " had unloaded it)";
}

namespace {

// The GGUF a router id names: <root>/<id>.gguf, or the model file in <root>/<id>/ (the mmproj beside it is
// loaded with it, so its size counts too). 0 when neither exists.
long gguf_bytes(const std::string& id, const std::filesystem::path& root) {
    std::error_code ec;
    if (long n = static_cast<long>(std::filesystem::file_size(root / (id + ".gguf"), ec)); !ec) return n;
    long total = 0;
    for (const auto& f : std::filesystem::directory_iterator(root / id, ec)) {
        if (f.path().extension() != ".gguf") continue;
        long n = static_cast<long>(std::filesystem::file_size(f.path(), ec));  // follows a link to the real weights
        if (!ec) total += n;
    }
    return total;
}

// "Qwen3.5-4B-Q4_K_M" -> 4.0; 0 when the name carries no parameter count.
double params_b(const std::string& id) {
    for (size_t i = 0; i < id.size(); ++i) {
        if (id[i] != 'B' || i == 0 || !std::isdigit(static_cast<unsigned char>(id[i - 1]))) continue;
        if (i + 1 < id.size() && std::isalnum(static_cast<unsigned char>(id[i + 1]))) continue;
        size_t start = i;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(id[start - 1])) || id[start - 1] == '.')) --start;
        if (start > 0 && std::isalnum(static_cast<unsigned char>(id[start - 1]))) continue;  // "Q4_K_M" style, not a size
        return std::atof(id.substr(start, i - start).c_str());
    }
    return 0;
}

std::string size_label(const std::string& id) {
    double b = params_b(id);
    if (b <= 0) return id;
    char buf[32];
    snprintf(buf, sizeof(buf), b == static_cast<long>(b) ? "%.0fB" : "%.1fB", b);
    return buf;
}

std::string k_tokens(int context) {
    return std::to_string(context / 1024) + "k";
}

}  // namespace

long estimate_footprint(long weight_bytes, const std::string& id, int context) {
    double per_token_mb = params_b(id) > 6 ? 0.13 : 0.065;
    return weight_bytes + static_cast<long>(context * per_token_mb * 1024 * 1024);
}

long model_footprint(const ModelPlan& plan, const std::filesystem::path& models_root) {
    if (plan.bytes >= 0) return plan.bytes;
    long weights = gguf_bytes(plan.id, models_root);
    if (weights <= 0) return -1;
    return estimate_footprint(weights, plan.id, plan.context);
}

std::string budget_sentence(const std::vector<ModelPlan>& plans, const std::filesystem::path& models_root, long card_total, bool comfyui_running, long comfyui_used) {
    std::string terms;
    long total = 0;
    for (const auto& plan : plans) {
        long bytes = model_footprint(plan, models_root);
        if (bytes < 0) continue;
        total += bytes;
        terms += (terms.empty() ? "" : " + ") + (plan.label.empty() ? size_label(plan.id) + " at " + k_tokens(plan.context) : plan.label) + " (" + gib(bytes) + " est.)";
    }
    if (terms.empty()) return "";
    std::string out = terms + " = " + gib(total);
    if (card_total <= 0) return out + " estimated; the card's size is unknown (ComfyUI's /system_stats or nvidia-smi would tell)";
    out += " of " + gib(card_total) + ": ";
    if (total > card_total) return out + "does not fit; lower context_2, or pick a smaller model for the side server";
    if (!comfyui_running) return out + "fits with ComfyUI stopped";
    if (comfyui_used > 0 && total + comfyui_used > card_total) return out + "fits only with ComfyUI stopped (it holds " + gib(comfyui_used) + ")";
    return out + "fits beside ComfyUI" + (comfyui_used > 0 ? " (" + gib(comfyui_used) + " in use)" : "");
}

std::string gpu_budget(const GpuReport& report, const Settings& settings, long card_total_fallback) {
    std::vector<ModelPlan> plans;
    std::string main_model;
    for (const auto& s : report.servers) {
        if (is_fim_server(s.name)) {
            // Counted while loaded: it loads only when MAIC asks (no autoload), never on llama.vim's requests.
            std::string id = s.models.empty() ? "" : s.models.front();
            long bytes = id.empty() ? -1 : model_footprint({id, s.context}, fim_models_root());
            if (bytes >= 0) plans.push_back({"", 0, "completion " + size_label(id) + " at " + k_tokens(s.context), bytes});
            continue;
        }
        ModelPlan plan;
        plan.context = s.name == "llamacpp" ? settings.context : settings.context_2;
        if (!s.models.empty()) {
            plan.id = s.models.front();
        } else {
            std::string want = s.name == "llamacpp" ? resolve_model_alias(settings.model) : settings.reviewer_model;
            auto slash = want.find('/');
            if (slash != std::string::npos && want.substr(0, slash) == s.name) plan.id = want.substr(slash + 1);
            else if (s.name != "llamacpp") plan.id = main_model;  // the smart harness reviews on the side server with the main model
        }
        if (s.name == "llamacpp") main_model = plan.id;
        if (!plan.id.empty()) plans.push_back(plan);
    }
    // whisper-server holds its model from start: the file plus about 300 MB of KV and compute buffers.
    if (report.has_whisper && report.whisper_bytes >= 0) {
        plans.push_back({"", 0, "whisper " + fs::path(report.whisper_model).stem().string(), report.whisper_bytes + (300L << 20)});
    }
    long card = report.card_total > 0 ? report.card_total : card_total_fallback;
    return budget_sentence(plans, llamacpp_models_root(), card, report.comfyui_running, report.comfyui_vram_used);
}

std::string explain_exit(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    std::string text = log_tail(def, 80);
    if (text.empty()) return "";
    auto has = [&](const char* s) { return text.find(s) != std::string::npos; };
    if (has("out of memory") || has("CUDA_ERROR_OUT_OF_MEMORY") || has("cudaErrorMemoryAllocation") || has("failed to allocate")) {
        GpuReport g = gpu_report(services);
        std::string who;
        for (const auto& s : g.servers) {
            if (s.models.empty() || s.name == def.name) continue;
            who += (who.empty() ? "" : ", ") + s.name + " holds " + joined(s.models);
        }
        if (who.empty() && g.comfyui_running && def.name != "comfyui") who = "comfyui holds its models";
        if (g.whisper_running && def.name != "whisper") who += (who.empty() ? "" : ", ") + std::string("whisper holds its model");
        std::string out = "CUDA out of memory: the card is full" + (who.empty() ? std::string(" (maic gpu shows who holds it)") : " (" + who + "). maic gpu free releases it, then maic up " + def.name + " again");
        if (is_fim_server(def.name)) out += "; a smaller completion model also helps: maic models install qwen2.5-coder-3b --link (or qwen2.5-coder-1.5b)";
        return out;
    }
    if (has("Address already in use")) return "port " + std::to_string(def.port) + " is already in use: another copy is running, or something else took the port (maic status)";
    if (has("ModuleNotFoundError") || has("No module named")) return "a Python module is missing: the venv is incomplete (maic vendor update " + def.name + " rebuilds it)";
    if (has("Driver/library version mismatch")) return "the NVIDIA driver in the kernel does not match the libraries on disk: a reboot fixes it";
    return "";
}

std::string comfyui_torch_cuda() {
    auto e = find_vendor("comfyui");
    if (!e) return "";
    std::filesystem::path python = vendor_link(*e) / ".venv" / "bin" / "python";
    std::error_code ec;
    if (!std::filesystem::exists(python, ec)) return "";
    std::string out = first_line("'" + python.string() + "' -c 'import torch; print(torch.version.cuda or \"\")' 2>/dev/null");
    return out == "None" ? "" : out;
}

std::string driver_cuda() {
    // The header of nvidia-smi's table: "... Driver Version: 580.65.06     CUDA Version: 13.0 |".
    std::string line = first_line("nvidia-smi 2>/dev/null | grep -o 'CUDA Version: [0-9.]*' | head -1");
    return line.rfind("CUDA Version: ", 0) == 0 ? line.substr(14) : "";
}

std::string cuda_agreement(const std::string& torch_cuda, const std::string& driver_cuda) {
    if (torch_cuda.empty() || driver_cuda.empty()) return "";
    auto parse = [](const std::string& v) {
        int major = std::atoi(v.c_str()), minor = 0;
        if (auto dot = v.find('.'); dot != std::string::npos) minor = std::atoi(v.c_str() + dot + 1);
        return std::make_pair(major, minor);
    };
    auto t = parse(torch_cuda), d = parse(driver_cuda);
    if (d >= t) return "ComfyUI's torch is built for CUDA " + torch_cuda + " and the driver supports " + driver_cuda + ": they agree";
    return "ComfyUI's torch is built for CUDA " + torch_cuda + " but the driver supports only " + driver_cuda + ": update the driver (580 or newer for CUDA 13), or install a torch wheel for cu" +
           std::to_string(d.first) + std::to_string(d.second) + " (the --index-url in vendor/comfyui.sh)";
}

std::string unreachable_hint(const Provider& provider, const std::vector<ServiceDef>& services) {
    // The port in the base_url names the service: http://127.0.0.1:8081/v1 -> the one on 8081.
    size_t host = provider.base_url.find("://");
    if (host == std::string::npos) return "";
    size_t colon = provider.base_url.find(':', host + 3);
    if (colon == std::string::npos) return "";
    int port = std::atoi(provider.base_url.c_str() + colon + 1);
    for (const auto& def : services) {
        if (!port || def.port != port) continue;
        ServiceStatus st = service_status(def);
        std::string p = std::to_string(port);
        switch (st.state) {
            case ServiceState::Running:
                if (!st.port_open) return def.name + " is starting (" + st.who() + ") and not answering on port " + p + " yet: maic logs " + def.name;
                return def.name + " is running on port " + p + " but the request failed: maic logs " + def.name;
            case ServiceState::Foreign:
                return "port " + p + " is held by a process MAIC did not start: maic status";
            case ServiceState::Stopped: {
                std::string missing = missing_requirement(def);
                return def.name + " is not running: maic up " + def.name + (missing.empty() ? "" : " (" + missing.substr(def.name.size() + 1) + ")");
            }
        }
    }
    return "";
}

std::string apply_preset(Settings& settings, const std::string& query) {
    auto p = find_preset(settings.presets, query);
    if (!p) return "";
    settings.model = p->model;
    if (p->think >= 0) settings.think = p->think == 1;
    set_preset_window(settings.providers, *p);
    if (p->context > 0) {
        std::string provider = resolve_model(settings.providers, p->model).first.name;
        if (provider == "llamacpp") settings.context = p->context;
        else if (provider == "llamacpp-2") settings.context_2 = p->context;
    }
    return p->name;
}

void set_context(std::vector<Provider>& providers, int tokens, const std::string& service) {
    setenv(service == "llamacpp" ? "MAIC_CONTEXT" : "MAIC_CONTEXT_2", std::to_string(tokens).c_str(), 1);
    for (auto& p : providers) {
        if (p.name == service) p.options["context_window"] = tokens;
    }
}

std::string restart_llamacpp_if_changed(const std::string& service) {
    for (const auto& def : load_services(root_dir() / "services")) {
        if (def.name != service) continue;
        // Restart when the size differs, and also when the running server predates command recording: the
        // user asked for this size, and an unknown one is not it.
        if (service_status(def).state != ServiceState::Running) continue;
        if (!recorded_command(def).empty() && !command_changed(def)) continue;
        stop_service(def);
        bool ready = start_service(def);
        return service + " restarted with the new context size" + std::string(ready ? "" : " (still starting)");
    }
    return "";
}

std::string preset_lines(const Settings& settings) {
    std::string out;
    for (const auto& p : settings.presets) {
        ModelPick sub = subagent_pick(settings.presets, p);
        ModelPick rev = reviewer_pick(settings.presets, settings.providers, p.model, settings.reviewer_model, settings.small_model, {});
        out += "\n  " + p.name + "  " + p.model + "  tier " + std::to_string(p.tier) + (p.limited ? ", limited" : "") + ", context " + std::to_string(p.context) +
               ", subagents on " + (sub.preset == p.name ? "itself" : sub.preset + " (" + sub.reason + ")") +
               ", reviewer " + (rev.model == p.model ? "itself" : rev.preset.empty() ? rev.model : rev.preset);
    }
    return out;
}

}  // namespace maic
