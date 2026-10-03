# LuaJIT from the pinned lua-pins submodule, built out of tree so the submodule stays pristine.
# Provides the imported target `luajit` (static, with include dir) for maid_core.
#
# The source is exactly the revision Neovim v0.12.2 builds against; see vendor/VENDORING.
set(LUAJIT_SRC "${PROJECT_SOURCE_DIR}/vendor/lua-pins/luajit-2.1.1774638290/luajit")
set(LUAJIT_BUILD "${CMAKE_BINARY_DIR}/luajit")
set(LUAJIT_LIB "${LUAJIT_BUILD}/src/libluajit.a")

if(NOT EXISTS "${LUAJIT_SRC}/src/lua.h")
    message(FATAL_ERROR "LuaJIT source missing at ${LUAJIT_SRC}. Run: git submodule update --init vendor/lua-pins")
endif()

file(GLOB_RECURSE LUAJIT_SOURCES CONFIGURE_DEPENDS "${LUAJIT_SRC}/src/*.c" "${LUAJIT_SRC}/src/*.h" "${LUAJIT_SRC}/src/Makefile" "${LUAJIT_SRC}/dynasm/*")

# Copy the tree, then run its own Makefile: static library only, position independent so it links into maid.
add_custom_command(
    OUTPUT "${LUAJIT_LIB}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LUAJIT_BUILD}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LUAJIT_SRC}" "${LUAJIT_BUILD}"
    COMMAND $(MAKE) -C "${LUAJIT_BUILD}/src" -s libluajit.a BUILDMODE=static XCFLAGS=-fPIC
    DEPENDS ${LUAJIT_SOURCES}
    COMMENT "Building LuaJIT (pinned, static) from vendor/lua-pins"
    VERBATIM)
add_custom_target(luajit_build DEPENDS "${LUAJIT_LIB}")

add_library(luajit STATIC IMPORTED GLOBAL)
set_target_properties(luajit PROPERTIES IMPORTED_LOCATION "${LUAJIT_LIB}")
# The headers are read from the build copy, which exists once the library does.
file(MAKE_DIRECTORY "${LUAJIT_BUILD}/src")
target_include_directories(luajit INTERFACE "${LUAJIT_BUILD}/src")
target_link_libraries(luajit INTERFACE dl m)
add_dependencies(luajit luajit_build)
