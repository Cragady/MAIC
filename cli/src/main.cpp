#include "maic/paths.hpp"
#include "maic/service.hpp"
#include "maic/tripwire.hpp"
#include "tui.hpp"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cerr << "usage: maic                 start the agent in this directory (MAIC_MODEL picks the model)\n"
                 "       maic <command> [args]\n"
                 "\n"
                 "  status                  show every service\n"
                 "  up <service...|all>     start services\n"
                 "  down <service...|all>   stop services MAIC started\n"
                 "  logs <service> [lines]  show the end of a service's log (default 40 lines)\n"
                 "  trip [reason]           trip the harness lock now (blocks all actions until unlocked)\n"
                 "  unlock                  reset the harness lock (asks for your sudo password)\n";
}

std::vector<maic::ServiceDef> select(const std::vector<maic::ServiceDef>& all, const std::vector<std::string>& names) {
    if (names.size() == 1 && names[0] == "all") {
        return all;
    }
    std::vector<maic::ServiceDef> out;
    for (const auto& name : names) {
        auto it = std::find_if(all.begin(), all.end(), [&](const auto& d) { return d.name == name; });
        if (it == all.end()) {
            throw std::runtime_error("unknown service: " + name);
        }
        out.push_back(*it);
    }
    return out;
}

int cmd_status(const std::vector<maic::ServiceDef>& services) {
    if (auto state = maic::tripwire_state()) {
        std::cout << "harness: TRIPPED\n" << *state << "\n";
    } else {
        std::cout << "harness: armed\n";
    }
    for (const auto& def : services) {
        auto st = maic::service_status(def);
        std::string state;
        switch (st.state) {
            case maic::ServiceState::Running:
                state = "running (pid " + std::to_string(st.pid) + (st.port_open ? ")" : ", port not open yet)");
                break;
            case maic::ServiceState::Foreign:
                state = "port " + std::to_string(def.port) + " in use, not started by MAIC";
                break;
            case maic::ServiceState::Stopped:
                state = "stopped";
                break;
        }
        std::cout << def.name << ": " << state << "\n";
    }
    return 0;
}

int cmd_up(const std::vector<maic::ServiceDef>& services) {
    maic::require_armed("start services");
    int rc = 0;
    for (const auto& def : services) {
        try {
            std::cout << def.name << ": starting..." << std::flush;
            bool ready = maic::start_service(def);
            std::cout << (ready ? " ready on port " + std::to_string(def.port) : " still starting, check `maic status`") << "\n";
        } catch (const std::exception& e) {
            std::cout << " failed\n";
            std::cerr << "maic: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_down(const std::vector<maic::ServiceDef>& services) {
    int rc = 0;
    for (const auto& def : services) {
        try {
            maic::stop_service(def);
            std::cout << def.name << ": stopped\n";
        } catch (const std::exception& e) {
            std::cerr << "maic: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

int cmd_trip(const std::vector<std::string>& words) {
    std::string reason = "manual trip";
    for (size_t i = 0; i < words.size(); ++i) {
        reason += (i == 0 ? ": " : " ") + words[i];
    }
    maic::trip_tripwire(reason);
    std::cout << "harness: TRIPPED. Nothing will run until `maic unlock`.\n";
    return 0;
}

int cmd_unlock() {
    if (!maic::tripwire_state()) {
        std::cout << "harness: not tripped\n";
        return 0;
    }
    // Drop any cached sudo login first so unlocking always needs the password.
    return std::system("sudo -k && sudo /usr/local/sbin/maic-lock reset") == 0 ? 0 : 1;
}

int cmd_logs(const maic::ServiceDef& def, size_t lines) {
    std::ifstream in(maic::service_log_path(def));
    if (!in) {
        std::cerr << "maic: no log yet for " << def.name << "\n";
        return 1;
    }
    std::deque<std::string> tail;
    for (std::string line; std::getline(in, line);) {
        tail.push_back(std::move(line));
        if (tail.size() > lines) {
            tail.pop_front();
        }
    }
    for (const auto& line : tail) {
        std::cout << line << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        const char* model = std::getenv("MAIC_MODEL");
        return maic::run_tui(model && *model ? model : "qwen3.5:4b");
    }
    try {
        const std::string& cmd = args[0];
        std::vector<std::string> rest(args.begin() + 1, args.end());

        // The lock commands come first so a broken service file can never block them.
        if (cmd == "trip") {
            return cmd_trip(rest);
        }
        if (cmd == "unlock" && rest.empty()) {
            return cmd_unlock();
        }

        auto services = maic::load_services(maic::root_dir() / "services");
        if (cmd == "status") {
            return cmd_status(services);
        }
        if ((cmd == "up" || cmd == "down") && !rest.empty()) {
            auto selected = select(services, rest);
            return cmd == "up" ? cmd_up(selected) : cmd_down(selected);
        }
        if (cmd == "logs" && (rest.size() == 1 || rest.size() == 2)) {
            size_t lines = rest.size() == 2 ? std::stoul(rest[1]) : 40;
            return cmd_logs(select(services, {rest[0]}).front(), lines);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "maic: " << e.what() << "\n";
        return 1;
    }
}
