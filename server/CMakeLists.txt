find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_library(maic_server src/server.cpp src/auth.cpp src/tls.cpp src/command.cpp src/tunnel.cpp src/home_link.cpp src/artifacts.cpp src/artifact_notify.cpp)
target_include_directories(maic_server PUBLIC src)
target_link_libraries(maic_server PUBLIC maic_core PRIVATE httplib::httplib OpenSSL::SSL OpenSSL::Crypto unofficial-sodium::sodium)
target_compile_options(maic_server PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_compile_definitions(maic_server PRIVATE MAIC_VERSION="${MAIC_GIT_VERSION}")

add_executable(maic-server src/main.cpp)
target_link_libraries(maic-server PRIVATE maic_server)
target_compile_options(maic-server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(server_test tests/server_test.cpp)
target_link_libraries(server_test PRIVATE maic_server httplib::httplib)
target_include_directories(server_test PRIVATE ../core/tests)
target_compile_options(server_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME server COMMAND server_test)

# The relay, the tunnel crypto and the end-to-end path: a maic-server dialling a loopback relay, a fake phone pairing then asking through it.
add_executable(relay_test tests/relay_test.cpp)
target_link_libraries(relay_test PRIVATE maic_server maic_relay httplib::httplib unofficial-sodium::sodium)
target_include_directories(relay_test PRIVATE ../core/tests)
target_compile_options(relay_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME relay COMMAND relay_test)

# The web client's copy of the tunnel crypto, against the same vectors, when node is on the machine.
find_program(NODE node)
if(NODE)
    add_test(NAME tunnel_js COMMAND ${NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tunnel_js_test.mjs ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
endif()
