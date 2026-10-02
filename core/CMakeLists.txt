find_package(httplib CONFIG REQUIRED)

add_library(maic_core
    src/agent.cpp
    src/anthropic.cpp
    src/artifacts.cpp
    src/audit_trail.cpp
    src/bans.cpp
    src/clipboard.cpp
    src/engine.cpp
    src/full_output.cpp
    src/harness.cpp
    src/import.cpp
    src/instructions.cpp
    src/jsonschema.cpp
    src/lazy_lock.cpp
    src/nvim_keymaps.cpp
    src/nvim_run.cpp
    src/nvim_setup.cpp
    src/llm.cpp
    src/lua.cpp
    src/lua_tools.cpp
    src/markdown.cpp
    src/nvim_host.cpp
    src/models.cpp
    src/openai.cpp
    src/paths.cpp
    src/agent_def.cpp
    src/places.cpp
    src/protocol.cpp
    src/image.cpp
    src/redact.cpp
    src/sandbox.cpp
    src/script_tools.cpp
    src/service.cpp
    src/session.cpp
    src/settings.cpp
    src/status.cpp
    src/theme.cpp
    src/tools.cpp
    src/tripwire.cpp
    src/trust.cpp
    src/vendor.cpp
)
target_include_directories(maic_core PUBLIC include)

# protocol/ built into the binary (docs/design/engine-protocol.md section 15): the engine validates against it at run
# time with no file lookup. The generated source holds the files' text; it is rewritten when one of them changes.
set(MAIC_PROTOCOL_FILES maic.openrpc.json schemas/maic.schema.json schemas/event.schema.json ordering.json openai/subset.json)
list(TRANSFORM MAIC_PROTOCOL_FILES PREPEND ${PROJECT_SOURCE_DIR}/protocol/ OUTPUT_VARIABLE protocol_deps)
list(JOIN MAIC_PROTOCOL_FILES "," protocol_list)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/protocol_files.cpp
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/protocol -DOUT=${CMAKE_CURRENT_BINARY_DIR}/protocol_files.cpp
            -DFILES=${protocol_list} -P ${PROJECT_SOURCE_DIR}/cmake/embed_protocol.cmake
    DEPENDS ${protocol_deps} ${PROJECT_SOURCE_DIR}/cmake/embed_protocol.cmake
    COMMENT "Embedding protocol/ into protocol_files.cpp")
target_sources(maic_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/protocol_files.cpp)
target_include_directories(maic_core PRIVATE src)
# httplib is PUBLIC on purpose: its OpenSSL support changes the layout of httplib::ClientImpl, so every
# translation unit that includes <httplib.h> must see the same compile definitions as the core does.
target_link_libraries(maic_core PUBLIC nlohmann_json::nlohmann_json httplib::httplib PRIVATE luajit)
target_compile_options(maic_core PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
# Where services/ and vendor/ live at run time: the source tree, or a checkout a release build names.
set(MAIC_SOURCE_ROOT "${PROJECT_SOURCE_DIR}" CACHE PATH "MAIC repository the built binary uses as its root")
target_compile_definitions(maic_core PRIVATE MAIC_VERSION="${MAIC_GIT_VERSION}" MAIC_ROOT="${MAIC_SOURCE_ROOT}")

foreach(t harness robustness llm agent session lua_tools script_tools models lazy_lock trust instructions jsonschema tool_output audit_trail protocol_schema protocol_conformance)
    add_executable(${t}_test tests/${t}_test.cpp)
    target_link_libraries(${t}_test PRIVATE maic_core httplib::httplib)
    target_compile_options(${t}_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
foreach(t session llm jsonschema)
    target_compile_definitions(${t}_test PRIVATE MAIC_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endforeach()
target_compile_definitions(agent_test PRIVATE MAIC_EXAMPLES="${PROJECT_SOURCE_DIR}/tools/examples")
target_compile_definitions(audit_trail_test PRIVATE MAIC_CONTRIB="${PROJECT_SOURCE_DIR}/contrib")
# OpenAI's pinned description (protocol/openai/): jsonschema_test validates against it, agent_test checks FakeServer's chunks.
foreach(t agent jsonschema protocol_schema)
    target_compile_definitions(${t}_test PRIVATE MAIC_PROTOCOL="${PROJECT_SOURCE_DIR}/protocol")
endforeach()
# protocol_conformance_test records every exchange it drives into build/protocol-streams/ for `maic protocol check`.
target_compile_definitions(protocol_conformance_test PRIVATE MAIC_PROTOCOL_STREAMS="${CMAKE_BINARY_DIR}/protocol-streams")
set_tests_properties(protocol_conformance PROPERTIES TIMEOUT 300)
# protocol_schema_test checks every JSON example in docs/design/engine-protocol.md.
target_compile_definitions(protocol_schema_test PRIVATE MAIC_DOCS="${PROJECT_SOURCE_DIR}/docs")
# subset.json is what extract.py makes of the pinned openapi.json, and openapi.json is the pinned file.
add_test(NAME openai_subset COMMAND python3 ${PROJECT_SOURCE_DIR}/protocol/openai/extract.py --check)

# The parser fuzzer: a seeded mutation loop, 2 s per target by default (FUZZ_SECONDS=60 for a long run).
add_executable(fuzz_parsers tests/fuzz_parsers.cpp)
target_link_libraries(fuzz_parsers PRIVATE maic_core httplib::httplib)
target_compile_options(fuzz_parsers PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME fuzz COMMAND fuzz_parsers)
