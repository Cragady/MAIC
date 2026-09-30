# maic-lock runs as root through sudo, so it stays tiny and depends on nothing but the standard library.
add_executable(maic-lock src/maic_lock.cpp)
target_compile_options(maic-lock PRIVATE -Wall -Wextra -Wpedantic)
