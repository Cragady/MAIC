#include "maic/status.hpp"

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
