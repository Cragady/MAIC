find_package(ftxui CONFIG REQUIRED)

add_executable(maic src/main.cpp src/tui.cpp src/editor.cpp src/view.cpp src/style.cpp src/headless.cpp)
target_link_libraries(maic PRIVATE maic_core ftxui::component ftxui::dom ftxui::screen)
target_compile_options(maic PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

add_executable(editor_test tests/editor_test.cpp src/editor.cpp)
target_link_libraries(editor_test PRIVATE maic_core ftxui::component ftxui::dom ftxui::screen)
target_include_directories(editor_test PRIVATE src ../core/tests)
target_compile_options(editor_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_test(NAME editor COMMAND editor_test)
