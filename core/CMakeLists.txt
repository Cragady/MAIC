find_package(httplib CONFIG REQUIRED)

add_library(maic_core
    src/agent.cpp
    src/harness.cpp
    src/ollama.cpp
    src/paths.cpp
    src/sandbox.cpp
    src/service.cpp
    src/tools.cpp
    src/tripwire.cpp
)
target_include_directories(maic_core PUBLIC include)
target_link_libraries(maic_core PUBLIC nlohmann_json::nlohmann_json PRIVATE httplib::httplib)
target_compile_options(maic_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(maic_core PRIVATE MAIC_ROOT="${PROJECT_SOURCE_DIR}")

add_executable(harness_test tests/harness_test.cpp)
target_link_libraries(harness_test PRIVATE maic_core)
add_test(NAME harness COMMAND harness_test)
