cmake_minimum_required(VERSION 3.25)
project(maid VERSION 0.2.0 LANGUAGES CXX)

# The version shown by `maid --version`: the git tag when there is one, else the project version plus commit.
# A release build passes -DMAID_GIT_VERSION=<tag> (the archive has no .git).
if(NOT MAID_GIT_VERSION)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE MAID_GIT_VERSION
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
endif()
if(NOT MAID_GIT_VERSION)
    set(MAID_GIT_VERSION "v${PROJECT_VERSION}")
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

# MAID's ComfyUI chat node, tested against a fake llama-server without ComfyUI.
add_test(NAME comfy_node COMMAND python3 -m unittest -v test_node
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/vendor/comfyui-maic-llamacpp)
add_test(NAME lint_includes COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/lint_includes.py)
add_test(NAME cli_smoke COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/cli_smoke.py $<TARGET_FILE:maid>)
# The streams protocol_conformance recorded, through `maid protocol check` and its OpenAI-only view.
add_test(NAME protocol_check COMMAND $<TARGET_FILE:maid> protocol check ${CMAKE_BINARY_DIR}/protocol-streams)
add_test(NAME protocol_check_openai COMMAND $<TARGET_FILE:maid> protocol check --openai ${CMAKE_BINARY_DIR}/protocol-streams)
set_tests_properties(protocol_check protocol_check_openai PROPERTIES FIXTURES_REQUIRED protocol_streams)
# The TUI through a pty, read back with pyte (tests/test_tui.py). Exit 77 is a skip: no pyte and no uv to fetch it.
add_test(NAME tui COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/test_tui.py $<TARGET_FILE:maid>)
set_tests_properties(tui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
add_test(NAME workflow_edit COMMAND python3 -m unittest -q test_workflow_edit
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME storyboard COMMAND python3 -m unittest -q test_storyboard
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME danbooru_tags COMMAND python3 -m unittest -q test_danbooru_tags
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
add_test(NAME panel_check COMMAND python3 -m unittest -q test_panel_check
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/comfyui)
# diction against a fake whisper-server and a fake scribe, from WAV files instead of the mic; MAID_BIN for the two
# cases that run maid itself (model resolve, the diction pass-through).
add_test(NAME diction COMMAND python3 -m unittest -q diction.test_diction WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(diction PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;MAID_BIN=$<TARGET_FILE:maid>")
# The leak audit against made-up sessions and a fake local judge (docs/leak-audit.md); never a real transcript.
add_test(NAME leak_audit COMMAND python3 -m unittest -q test_leak_audit
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools/audit)
set_tests_properties(comfy_node workflow_edit storyboard danbooru_tags panel_check leak_audit PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
# cai-tools, ported whole as tools/cai (docs/cai.md): its own suite as written, plus the MAID tests beside it, through its runner.
add_test(NAME cai_tools COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/cai/run_tests.py)
set_tests_properties(cai_tools PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1 TIMEOUT 900)

install(TARGETS maid maid-server maid-relay RUNTIME DESTINATION bin)
install(PROGRAMS tools/comfyui/workflow_edit.py DESTINATION bin RENAME maid-workflow-edit)
install(PROGRAMS tools/comfyui/storyboard.py DESTINATION bin RENAME maid-storyboard)
install(PROGRAMS tools/comfyui/danbooru_tags.py DESTINATION bin RENAME maid-danbooru-tags)
install(PROGRAMS tools/comfyui/panel_check.py DESTINATION bin RENAME maid-panel-check)
install(PROGRAMS tools/audit/leak_audit.py DESTINATION bin RENAME maid-leak-audit)
# cai-tools (docs/cai.md): the `cai` wrapper, `maid-cai` a link to it, and the package under share/maid/tools/cai as in the tree.
install(PROGRAMS tools/cai/bin/cai DESTINATION bin)
install(CODE "file(CREATE_LINK cai \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/maid-cai\" SYMBOLIC)")
install(DIRECTORY tools/cai/src tools/cai/docs tools/cai/reference DESTINATION share/maid/tools/cai PATTERN "__pycache__" EXCLUDE)
install(FILES tools/cai/pyproject.toml tools/cai/README.md tools/cai/PROVENANCE.md tools/cai/IMPROVEMENTS.md DESTINATION share/maid/tools/cai)
# diction: the package under share/maid (its launcher finds it there), the launcher beside maid, and the version it
# reports as its stable build.
install(DIRECTORY diction/ DESTINATION share/maid/diction PATTERN "test_*" EXCLUDE PATTERN "__pycache__" EXCLUDE PATTERN "maid-diction" EXCLUDE)
install(PROGRAMS diction/maid-diction DESTINATION bin)
file(WRITE ${CMAKE_BINARY_DIR}/diction-VERSION "${MAID_GIT_VERSION}\n")
install(FILES ${CMAKE_BINARY_DIR}/diction-VERSION DESTINATION share/maid/diction RENAME VERSION)
install(DIRECTORY server/web/ DESTINATION share/maid/server/web)
# Vue for artifacts, served by maid-server at /a/_vendor/vue/ (docs/artifacts.md).
install(FILES vendor/vue/vue.global.prod.js vendor/vue/LICENSE vendor/vue/PROVENANCE DESTINATION share/maid/vendor/vue)
# The engine protocol's description for clients that read it (maid.nvim, the web client; docs/design/engine-protocol.md
# section 15): MAID's files and OpenAI's subset, not the 4.6 MB description it is cut from or the script that cuts it.
install(DIRECTORY protocol/ DESTINATION share/maid/protocol PATTERN "openapi.json" EXCLUDE PATTERN "extract.py" EXCLUDE)
install(DIRECTORY services/ DESTINATION share/maid/services)
# The audit schedule's systemd units, which `maid audit-trail schedule install` fills in (docs/audit-trail.md).
install(DIRECTORY contrib/systemd/ DESTINATION share/maid/contrib/systemd)
install(DIRECTORY themes/ DESTINATION share/maid/themes)
# maid.nvim, for `maid --ui nvim` and `maid nvim keymaps` from an installed copy.
install(DIRECTORY maid.nvim/ DESTINATION share/maid/maid.nvim PATTERN "tests" EXCLUDE)
install(DIRECTORY models/ DESTINATION share/maid/models)
# Fallback root for a copy installed without the repository: the vendor ledger and scripts, not the checkouts.
install(FILES vendor/manifest.json vendor/comfyui.sh vendor/llamacpp.sh vendor/whispercpp.sh vendor/VENDORING DESTINATION share/maid/vendor)
