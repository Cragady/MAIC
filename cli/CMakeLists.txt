find_package(ftxui CONFIG REQUIRED)

add_executable(maic src/main.cpp src/tui.cpp)
target_link_libraries(maic PRIVATE maic_core ftxui::component ftxui::dom ftxui::screen)
target_compile_options(maic PRIVATE -Wall -Wextra -Wpedantic)
