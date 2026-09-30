find_package(httplib CONFIG REQUIRED)

add_library(maic_core
    src/agent.cpp
    src/anthropic.cpp
    src/artifacts.cpp
    src/clipboard.cpp
    src/harness.cpp
    src/instructions.cpp
    src/llm.cpp
    src/lua.cpp
    src/lua_tools.cpp
    src/markdown.cpp
    src/ollama.cpp
    src/openai.cpp
    src/paths.cpp
    src/sandbox.cpp
    src/service.cpp
    src/session.cpp
    src/settings.cpp
    src/status.cpp
    src/tools.cpp
    src/tripwire.cpp
    src/vendor.cpp
)
target_include_directories(maic_core PUBLIC include)
target_link_libraries(maic_core PUBLIC nlohmann_json::nlohmann_json PRIVATE httplib::httplib luajit)
target_compile_options(maic_core PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
# Where services/ and vendor/ live at run time: the source tree, or a checkout a release build names.
set(MAIC_SOURCE_ROOT "${PROJECT_SOURCE_DIR}" CACHE PATH "MAIC repository the built binary uses as its root")
target_compile_definitions(maic_core PRIVATE MAIC_VERSION="${MAIC_GIT_VERSION}" MAIC_ROOT="${MAIC_SOURCE_ROOT}")

foreach(t harness robustness llm agent lua_tools)
    add_executable(${t}_test tests/${t}_test.cpp)
    target_link_libraries(${t}_test PRIVATE maic_core httplib::httplib)
    target_compile_options(${t}_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
