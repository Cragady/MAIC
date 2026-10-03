// maid-server: the same thing as `maid server ...`, as its own binary.
#include "server.hpp"

#include "maid/llm.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // A `cli` agent's MCP server, when this binary started the agent (core/src/cli_provider.cpp).
    if (argc == 3 && std::string(argv[1]) == "mcp-bridge") return maid::run_mcp_bridge(argv[2]);
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return maid::server::run_server_command(args);
    } catch (const std::exception& e) {
        std::cerr << "maid-server: " << e.what() << "\n";
        return 1;
    }
}
