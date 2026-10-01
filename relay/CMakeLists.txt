find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# The relay shares only the TLS helper with the server: it must not pull the core (Lua, the harness) onto a VPS.
add_library(maic_relay src/relay.cpp ../server/src/tls.cpp)
target_include_directories(maic_relay PUBLIC src ../server/src ../core/include)
target_link_libraries(maic_relay PUBLIC httplib::httplib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(maic_relay PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

add_executable(maic-relay src/main.cpp)
target_link_libraries(maic-relay PRIVATE maic_relay)
target_compile_options(maic-relay PRIVATE -Wall -Wextra -Wpedantic)
