#include "maic/status.hpp"

#include "maic/http.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <filesystem>

namespace maic {

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

std::string first_line(const char* command) {
    FILE* p = popen(command, "r");
    if (!p) return "";
    char buf[256] = "";
    std::string out = fgets(buf, sizeof(buf), p) ? buf : "";
    pclose(p);
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
        ServiceStatus st = service_status(def);
        std::string url = def.port ? "http://127.0.0.1:" + std::to_string(def.port) : "";
        switch (st.state) {
            case ServiceState::Running:
                r.state = st.port_open ? "running" : "starting";
                r.where = st.who() + (url.empty() ? "" : " · " + url);
                r.detail = service_detail(def, st);
                r.actions = {"maic down " + def.name, "maic logs " + def.name};
                break;
            case ServiceState::Foreign:
                r.state = "foreign";
                r.where = "port " + std::to_string(def.port) + " is held by a process MAIC did not start";
                r.actions = {"stop that process yourself, then: maic up " + def.name};
                break;
            case ServiceState::Stopped:
                r.state = "stopped";
                r.where = url;
                r.actions = {"maic up " + def.name};
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

std::string format_status(const StatusReport& rep) {
    std::string out = rep.tripped ? "harness: TRIPPED\n" + rep.tripwire : "harness: armed\n";
    for (const auto& a : rep.actions) out += "  -> " + a + "\n";
    for (const auto& s : rep.services) {
        out += s.name + ": " + s.state + " [" + s.runtime + "]" + (s.where.empty() ? "" : "  " + s.where) + "\n";
        if (!s.detail.empty()) out += "     " + s.detail + "\n";
        for (const auto& a : s.actions) out += "  -> " + a + "\n";
    }
    if (!rep.lazy_lock.empty()) out += "nvim lazy-lock: " + rep.lazy_lock + "\n";
    return out;
}

std::string missing_requirement(const ServiceDef& def) {
    std::error_code ec;
    for (const auto& path : def.requires_paths) {
        if (std::filesystem::exists(path, ec)) continue;
        if (is_llama_server(def.name)) {
            return def.name + " needs a models directory: put a GGUF under " + path.string() + " or run maic vendor model llamacpp URL SHA256 (models_dir in settings moves it)";
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

std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    if (!def.needs_gpu || is_llama_server(def.name)) return "";
    std::string out;
    for (const auto& other : services) {
        if (!is_llama_server(other.name) || service_status(other).state != ServiceState::Running) continue;
        auto freed = unload_resident(local_url(other));
        if (freed.empty()) continue;
        out += (out.empty() ? "unloaded " : "; ") + joined(freed) + " from " + other.name;
    }
    if (out.empty()) return "";
    return out + " to free the GPU for " + def.name + " (they reload on the next request)";
}

GpuReport gpu_report(const std::vector<ServiceDef>& services) {
    GpuReport r;
    for (const auto& def : services) {
        if (!is_llama_server(def.name)) continue;
        GpuReport::Server s;
        s.name = def.name;
        s.running = service_status(def).state == ServiceState::Running;
        if (s.running) s.models = resident_models(local_url(def));
        r.servers.push_back(s);
    }
    if (const auto* cf = by_name(services, "comfyui"); cf && service_status(*cf).state == ServiceState::Running) {
        r.comfyui_running = true;
        auto [used, total] = comfyui_vram(local_url(*cf));
        r.comfyui_vram_used = used;
        r.comfyui_vram_total = total;
    }
    r.card_total = r.comfyui_vram_total;
    if (r.card_total <= 0) {
        // nvidia-smi reports MiB; a missing or broken nvidia-smi prints nothing.
        long mib = std::atol(first_line("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null | head -1").c_str());
        if (mib > 0) r.card_total = mib * 1024L * 1024L;
    }
    return r;
}

std::string GpuReport::text() const {
    std::string out;
    for (const auto& s : servers) {
        out += s.name + ": ";
        if (!s.running) out += "not running\n";
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
    return out;
}

std::string gpu_free(const std::vector<ServiceDef>& services, const std::string& what) {
    std::string out;
    bool known = what == "all" || what == "comfyui";
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
            httplib::Client c(local_url(*cf));
            c.set_connection_timeout(2);
            c.set_read_timeout(30);
            auto res = c.Post("/free", R"({"unload_models":true,"free_memory":true})", "application/json");
            out += res && res->status == 200 ? "comfyui: models unloaded and caches released\n" : "comfyui: /free failed\n";
        } else if (what == "comfyui") {
            out += "comfyui: not running\n";
        }
    }
    if (!known) throw std::runtime_error("maic gpu free [all|llamacpp|llamacpp-2|comfyui]");
    return out;
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

long model_footprint(const ModelPlan& plan, const std::filesystem::path& models_root) {
    long weights = gguf_bytes(plan.id, models_root);
    if (weights <= 0) return -1;
    double per_token_mb = params_b(plan.id) > 6 ? 0.13 : 0.065;
    return weights + static_cast<long>(plan.context * per_token_mb * 1024 * 1024);
}

std::string budget_sentence(const std::vector<ModelPlan>& plans, const std::filesystem::path& models_root, long card_total, bool comfyui_running, long comfyui_used) {
    std::string terms;
    long total = 0;
    for (const auto& plan : plans) {
        long bytes = model_footprint(plan, models_root);
        if (bytes < 0) continue;
        total += bytes;
        terms += (terms.empty() ? "" : " + ") + size_label(plan.id) + " at " + k_tokens(plan.context) + " (" + gib(bytes) + " est.)";
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
        return "CUDA out of memory: the card is full" + (who.empty() ? std::string(" (maic gpu shows who holds it)") : " (" + who + "). maic gpu free releases it, then maic up " + def.name + " again");
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
    std::string out = first_line(("'" + python.string() + "' -c 'import torch; print(torch.version.cuda or \"\")' 2>/dev/null").c_str());
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

}  // namespace maic
