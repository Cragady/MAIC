# maid-lock runs as root through sudo, so it stays tiny and depends on nothing but the standard library.
add_executable(maid-lock src/maid_lock.cpp)
target_compile_options(maid-lock PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
