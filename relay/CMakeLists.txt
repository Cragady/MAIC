find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# The relay shares only the TLS helper with the server: it must not pull the core (Lua, the harness) onto a VPS.
add_library(maid_relay src/relay.cpp ../server/src/tls.cpp)
target_include_directories(maid_relay PUBLIC src ../server/src ../core/include)
target_link_libraries(maid_relay PUBLIC httplib::httplib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(maid_relay PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

add_executable(maid-relay src/main.cpp)
target_link_libraries(maid-relay PRIVATE maid_relay)
target_compile_options(maid-relay PRIVATE -Wall -Wextra -Wpedantic)
