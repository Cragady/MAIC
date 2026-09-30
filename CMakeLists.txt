cmake_minimum_required(VERSION 3.25)
project(maic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json CONFIG REQUIRED)

enable_testing()
add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(harness)
