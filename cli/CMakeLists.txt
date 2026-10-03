find_package(ftxui CONFIG REQUIRED)

add_executable(maid src/main.cpp src/audit_trail.cpp src/tui.cpp src/editor.cpp src/view.cpp src/style.cpp src/headless.cpp src/rpc.cpp src/daemon.cpp src/commands.cpp src/doctor.cpp src/setup.cpp src/msgpack.cpp src/highlight.cpp src/nvim_host.cpp)
target_link_libraries(maid PRIVATE maid_core maid_server ftxui::component ftxui::dom ftxui::screen)
target_compile_options(maid PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_compile_definitions(maid PRIVATE MAID_VERSION="${MAID_GIT_VERSION}")

add_executable(editor_test tests/editor_test.cpp src/editor.cpp src/commands.cpp src/view.cpp src/style.cpp src/msgpack.cpp src/highlight.cpp)
target_link_libraries(editor_test PRIVATE maid_core ftxui::component ftxui::dom ftxui::screen)
target_include_directories(editor_test PRIVATE src ../core/tests)
target_compile_options(editor_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME editor COMMAND editor_test)

# maid.nvim against a headless nvim, and the plugin's own Lua tests. Exit 77 is a skip: nvim is not installed.
add_executable(nvim_test tests/nvim_test.cpp src/nvim_host.cpp src/msgpack.cpp)
target_link_libraries(nvim_test PRIVATE maid_core)
target_include_directories(nvim_test PRIVATE src ../core/tests)
target_compile_options(nvim_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_compile_definitions(nvim_test PRIVATE MAID_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/maid.nvim" MAID_BINARY="$<TARGET_FILE:maid>")
add_dependencies(nvim_test maid)
add_test(NAME nvim COMMAND nvim_test)
set_tests_properties(nvim PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
