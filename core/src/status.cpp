#include "maic/status.hpp"

#include "maic/tripwire.hpp"

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

}  // namespace maic
