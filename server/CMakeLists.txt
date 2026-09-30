find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(maic_server src/server.cpp src/auth.cpp src/tls.cpp src/command.cpp)
target_include_directories(maic_server PUBLIC src)
target_link_libraries(maic_server PUBLIC maic_core PRIVATE httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(maic_server PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_compile_definitions(maic_server PRIVATE MAIC_VERSION="${MAIC_GIT_VERSION}")

add_executable(maic-server src/main.cpp)
target_link_libraries(maic-server PRIVATE maic_server)
target_compile_options(maic-server PRIVATE -Wall -Wextra -Wpedantic)
