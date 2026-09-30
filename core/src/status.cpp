#include "maic/status.hpp"

#include "maic/http.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

#include <deque>

#include "maic/tripwire.hpp"
#include "maic/vendor.hpp"

#include <filesystem>

namespace maic {

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
                r.where = "pid " + std::to_string(st.pid) + (url.empty() ? "" : " · " + url);
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
        for (const auto& a : s.actions) out += "  -> " + a + "\n";
    }
    return out;
}

std::string missing_requirement(const ServiceDef& def) {
    std::error_code ec;
    for (const auto& path : def.requires_paths) {
        if (std::filesystem::exists(path, ec)) continue;
        if (def.name == "llamacpp") {
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

std::string free_gpu_for(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    if (!def.needs_gpu || def.name == "llamacpp") return "";
    for (const auto& other : services) {
        if (other.name != "llamacpp" || service_status(other).state != ServiceState::Running) continue;
        auto freed = unload_resident("http://127.0.0.1:" + std::to_string(other.port));
        if (freed.empty()) return "";
        std::string names;
        for (const auto& f : freed) names += (names.empty() ? "" : ", ") + f;
        return "unloaded " + names + " from llamacpp to free the GPU for " + def.name + " (it reloads on the next request)";
    }
    return "";
}

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
}  // namespace

GpuReport gpu_report(const std::vector<ServiceDef>& services) {
    GpuReport r;
    if (const auto* lc = by_name(services, "llamacpp"); lc && service_status(*lc).state == ServiceState::Running) {
        r.llamacpp_running = true;
        r.llamacpp_models = resident_models("http://127.0.0.1:" + std::to_string(lc->port));
    }
    if (const auto* cf = by_name(services, "comfyui"); cf && service_status(*cf).state == ServiceState::Running) {
        r.comfyui_running = true;
        httplib::Client c("http://127.0.0.1:" + std::to_string(cf->port));
        c.set_connection_timeout(2);
        c.set_read_timeout(5);
        if (auto res = c.Get("/system_stats"); res && res->status == 200) {
            auto j = nlohmann::json::parse(res->body, nullptr, false);
            for (const auto& d : j.value("devices", nlohmann::json::array())) {
                long total = d.value("vram_total", 0L), free = d.value("vram_free", 0L);
                if (total > 0) {
                    r.comfyui_vram_total = total;
                    r.comfyui_vram_used = total - free;
                    break;
                }
            }
        }
    }
    return r;
}

std::string GpuReport::text() const {
    std::string out;
    if (llamacpp_running) {
        out += "llamacpp: ";
        if (llamacpp_models.empty()) out += "running, no model resident\n";
        else {
            out += "holds ";
            for (size_t i = 0; i < llamacpp_models.size(); ++i) out += (i ? ", " : "") + llamacpp_models[i];
            out += " (maic gpu free llamacpp unloads; it reloads on the next request)\n";
        }
    } else {
        out += "llamacpp: not running\n";
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
    if (what == "all" || what == "llamacpp") {
        if (const auto* lc = by_name(services, "llamacpp"); lc && service_status(*lc).state == ServiceState::Running) {
            auto freed = unload_resident("http://127.0.0.1:" + std::to_string(lc->port));
            std::string names;
            for (const auto& f : freed) names += (names.empty() ? "" : ", ") + f;
            out += freed.empty() ? "llamacpp: nothing was resident\n" : "llamacpp: unloaded " + names + "\n";
        } else if (what == "llamacpp") {
            out += "llamacpp: not running\n";
        }
    }
    if (what == "all" || what == "comfyui") {
        if (const auto* cf = by_name(services, "comfyui"); cf && service_status(*cf).state == ServiceState::Running) {
            httplib::Client c("http://127.0.0.1:" + std::to_string(cf->port));
            c.set_connection_timeout(2);
            c.set_read_timeout(30);
            auto res = c.Post("/free", R"({"unload_models":true,"free_memory":true})", "application/json");
            out += res && res->status == 200 ? "comfyui: models unloaded and caches released\n" : "comfyui: /free failed\n";
        } else if (what == "comfyui") {
            out += "comfyui: not running\n";
        }
    }
    if (what != "all" && what != "llamacpp" && what != "comfyui") throw std::runtime_error("maic gpu free [all|llamacpp|comfyui]");
    return out;
}

std::string explain_exit(const ServiceDef& def, const std::vector<ServiceDef>& services) {
    std::ifstream in(service_log_path(def));
    if (!in) return "";
    std::deque<std::string> tail;
    for (std::string line; std::getline(in, line);) {
        tail.push_back(line);
        if (tail.size() > 80) tail.pop_front();
    }
    std::string text;
    for (const auto& l : tail) text += l + "\n";
    auto has = [&](const char* s) { return text.find(s) != std::string::npos; };
    if (has("out of memory") || has("CUDA_ERROR_OUT_OF_MEMORY") || has("cudaErrorMemoryAllocation") || has("failed to allocate")) {
        GpuReport g = gpu_report(services);
        std::string who;
        if (!g.llamacpp_models.empty()) {
            who = "llamacpp holds ";
            for (size_t i = 0; i < g.llamacpp_models.size(); ++i) who += (i ? ", " : "") + g.llamacpp_models[i];
        } else if (g.comfyui_running && def.name != "comfyui") {
            who = "comfyui holds its models";
        }
        return "CUDA out of memory: the card is full" + (who.empty() ? std::string(" (maic gpu shows who holds it)") : " (" + who + "). maic gpu free releases it, then maic up " + def.name + " again");
    }
    if (has("Address already in use")) return "port " + std::to_string(def.port) + " is already in use: another copy is running, or something else took the port (maic status)";
    if (has("ModuleNotFoundError") || has("No module named")) return "a Python module is missing: the venv is incomplete (maic vendor update " + def.name + " rebuilds it)";
    if (has("Driver/library version mismatch")) return "the NVIDIA driver in the kernel does not match the libraries on disk: a reboot fixes it";
    return "";
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
                if (!st.port_open) return def.name + " is starting (pid " + std::to_string(st.pid) + ") and not answering on port " + p + " yet: maic logs " + def.name;
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
