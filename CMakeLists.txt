cmake_minimum_required(VERSION 3.25)
project(maic VERSION 0.1.0 LANGUAGES CXX)

# The version shown by `maic --version`: the git tag when there is one, else the project version plus commit.
# A release build passes -DMAIC_GIT_VERSION=<tag> (the archive has no .git).
if(NOT MAIC_GIT_VERSION)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE MAIC_GIT_VERSION
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
if(NOT MAIC_GIT_VERSION)
    set(MAIC_GIT_VERSION "v${PROJECT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json CONFIG REQUIRED)

enable_testing()
include(vendor/luajit.cmake)

add_subdirectory(core)
add_subdirectory(server)
add_subdirectory(cli)
add_subdirectory(harness)

install(TARGETS maic maic-server RUNTIME DESTINATION bin)
install(DIRECTORY server/web/ DESTINATION share/maic/server/web)
install(DIRECTORY services/ DESTINATION share/maic/services)
# Fallback root for a copy installed without the repository: the vendor ledger and scripts, not the checkouts.
install(FILES vendor/manifest.json vendor/comfyui.sh vendor/ollama.sh vendor/VENDORING DESTINATION share/maic/vendor)
install(DIRECTORY vendor/patches/ DESTINATION share/maic/vendor/patches)
