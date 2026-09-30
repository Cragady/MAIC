// maic-server: the same thing as `maic server ...`, as its own binary.
#include "server.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return maic::server::run_server_command(args);
    } catch (const std::exception& e) {
        std::cerr << "maic-server: " << e.what() << "\n";
        return 1;
    }
}
