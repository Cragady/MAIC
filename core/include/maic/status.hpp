#pragma once

#include "maic/service.hpp"

#include <string>
#include <vector>

namespace maic {

// One line of `maic status` / `:status`, with what the user could do about it.
struct ServiceReport {
    std::string name;
    std::string state;    // running, stopped, foreign, starting
    std::string runtime;  // host (a process MAIC started), docker (planned)
    std::string where;    // "pid 1234 · http://127.0.0.1:11434", or the port for a foreign process
    std::string log;      // path of the log MAIC keeps for it
    std::vector<std::string> actions;  // quick actions, as commands the user can run
};

std::vector<ServiceReport> service_reports(const std::vector<ServiceDef>& services);

struct StatusReport {
    bool tripped = false;
    std::string tripwire;  // contents of the lock file when tripped
    std::vector<ServiceReport> services;
    std::vector<std::string> actions;  // harness-level quick actions
};

StatusReport status_report(const std::vector<ServiceDef>& services);

// Plain-text rendering shared by the CLI command and the TUI.
std::string format_status(const StatusReport& report);

}  // namespace maic
