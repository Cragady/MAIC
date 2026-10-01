find_package(ftxui CONFIG REQUIRED)

add_executable(maic src/main.cpp src/tui.cpp src/editor.cpp src/view.cpp src/style.cpp src/headless.cpp src/commands.cpp src/doctor.cpp src/setup.cpp src/msgpack.cpp src/highlight.cpp)
target_link_libraries(maic PRIVATE maic_core maic_server ftxui::component ftxui::dom ftxui::screen)
target_compile_options(maic PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_compile_definitions(maic PRIVATE MAIC_VERSION="${MAIC_GIT_VERSION}")

add_executable(editor_test tests/editor_test.cpp src/editor.cpp src/commands.cpp src/view.cpp src/style.cpp src/msgpack.cpp src/highlight.cpp)
target_link_libraries(editor_test PRIVATE maic_core ftxui::component ftxui::dom ftxui::screen)
target_include_directories(editor_test PRIVATE src ../core/tests)
target_compile_options(editor_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME editor COMMAND editor_test)
