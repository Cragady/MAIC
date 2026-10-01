cmake_minimum_required(VERSION 3.25)
project(maic VERSION 0.2.0 LANGUAGES CXX)

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
add_subdirectory(relay)
add_subdirectory(cli)
add_subdirectory(harness)

# MAIC's ComfyUI chat node, tested against a fake llama-server without ComfyUI.
add_test(NAME comfy_node COMMAND python3 -m unittest -v test_node
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/comfyui-maic-llamacpp)
add_test(NAME lint_includes COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/lint_includes.py)
add_test(NAME cli_smoke COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/cli_smoke.py $<TARGET_FILE:maic>)
add_test(NAME workflow_edit COMMAND python3 -m unittest -q test_workflow_edit
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME storyboard COMMAND python3 -m unittest -q test_storyboard
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME danbooru_tags COMMAND python3 -m unittest -q test_danbooru_tags
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME panel_check COMMAND python3 -m unittest -q test_panel_check
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
set_tests_properties(comfy_node workflow_edit storyboard danbooru_tags panel_check PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)

install(TARGETS maic maic-server maic-relay RUNTIME DESTINATION bin)
install(PROGRAMS tools/comfyui/workflow_edit.py DESTINATION bin RENAME maic-workflow-edit)
install(PROGRAMS tools/comfyui/storyboard.py DESTINATION bin RENAME maic-storyboard)
install(PROGRAMS tools/comfyui/danbooru_tags.py DESTINATION bin RENAME maic-danbooru-tags)
install(PROGRAMS tools/comfyui/panel_check.py DESTINATION bin RENAME maic-panel-check)
install(DIRECTORY server/web/ DESTINATION share/maic/server/web)
install(DIRECTORY services/ DESTINATION share/maic/services)
# Fallback root for a copy installed without the repository: the vendor ledger and scripts, not the checkouts.
install(FILES vendor/manifest.json vendor/comfyui.sh vendor/llamacpp.sh vendor/VENDORING DESTINATION share/maic/vendor)
