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
# The TUI through a pty, read back with pyte (tests/test_tui.py). Exit 77 is a skip: no pyte and no uv to fetch it.
add_test(NAME tui COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/test_tui.py $<TARGET_FILE:maic>)
set_tests_properties(tui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
add_test(NAME workflow_edit COMMAND python3 -m unittest -q test_workflow_edit
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME storyboard COMMAND python3 -m unittest -q test_storyboard
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME danbooru_tags COMMAND python3 -m unittest -q test_danbooru_tags
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME panel_check COMMAND python3 -m unittest -q test_panel_check
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
# diction against a fake whisper-server and a fake scribe, from WAV files instead of the mic; MAIC_BIN for the two
# cases that run maic itself (model resolve, the diction pass-through).
add_test(NAME diction COMMAND python3 -m unittest -q diction.test_diction WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(diction PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;MAIC_BIN=$<TARGET_FILE:maic>")
set_tests_properties(comfy_node workflow_edit storyboard danbooru_tags panel_check PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# cai-tools, ported whole as tools/cai (docs/cai.md): its own suite as written, plus the MAIC tests beside it, through its runner.
add_test(NAME cai_tools COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/cai/run_tests.py)
set_tests_properties(cai_tools PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1 TIMEOUT 900)

install(TARGETS maic maic-server maic-relay RUNTIME DESTINATION bin)
install(PROGRAMS tools/comfyui/workflow_edit.py DESTINATION bin RENAME maic-workflow-edit)
install(PROGRAMS tools/comfyui/storyboard.py DESTINATION bin RENAME maic-storyboard)
install(PROGRAMS tools/comfyui/danbooru_tags.py DESTINATION bin RENAME maic-danbooru-tags)
install(PROGRAMS tools/comfyui/panel_check.py DESTINATION bin RENAME maic-panel-check)
# cai-tools (docs/cai.md): the `cai` wrapper, `maic-cai` a link to it, and the package under share/maic/tools/cai as in the tree.
install(PROGRAMS tools/cai/bin/cai DESTINATION bin)
install(CODE "file(CREATE_LINK cai \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/maic-cai\" SYMBOLIC)")
install(DIRECTORY tools/cai/src tools/cai/docs tools/cai/reference DESTINATION share/maic/tools/cai PATTERN "__pycache__" EXCLUDE)
install(FILES tools/cai/pyproject.toml tools/cai/README.md tools/cai/PROVENANCE.md tools/cai/IMPROVEMENTS.md DESTINATION share/maic/tools/cai)
# diction: the package under share/maic (its launcher finds it there), the launcher beside maic, and the version it
# reports as its stable build.
install(DIRECTORY diction/ DESTINATION share/maic/diction PATTERN "test_*" EXCLUDE PATTERN "__pycache__" EXCLUDE PATTERN "maic-diction" EXCLUDE)
install(PROGRAMS diction/maic-diction DESTINATION bin)
file(WRITE ${CMAKE_BINARY_DIR}/diction-VERSION "${MAIC_GIT_VERSION}\n")
install(FILES ${CMAKE_BINARY_DIR}/diction-VERSION DESTINATION share/maic/diction RENAME VERSION)
install(DIRECTORY server/web/ DESTINATION share/maic/server/web)
install(DIRECTORY services/ DESTINATION share/maic/services)
install(DIRECTORY themes/ DESTINATION share/maic/themes)
install(DIRECTORY models/ DESTINATION share/maic/models)
# Fallback root for a copy installed without the repository: the vendor ledger and scripts, not the checkouts.
install(FILES vendor/manifest.json vendor/comfyui.sh vendor/llamacpp.sh vendor/whispercpp.sh vendor/VENDORING DESTINATION share/maic/vendor)
