#pragma once

// Lua values <-> JSON, shared by the REPL state (settings.lua) and the sandboxed tool states.

extern "C" {
#include <lua.h>
}

#include <nlohmann/json.hpp>

namespace maid {

// Sequences (1..n) become arrays, other tables objects; functions and userdata are dropped.
nlohmann::json lua_to_json(lua_State* L, int idx);

// Pushes `j` as a Lua value: objects and arrays become tables, null becomes nil.
void json_to_lua(lua_State* L, const nlohmann::json& j);

}  // namespace maid
