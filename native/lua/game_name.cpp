#include "game_name.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace bar {

bool load_game_name(const std::string& game_root, std::string& name, std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);

    const std::string path = game_root + "/modinfo.lua";
    if (luaL_loadfile(state, path.c_str()) != 0) {
        error = lua_tostring(state, -1);
        lua_close(state);
        return false;
    }
    if (lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1);
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "modinfo.lua did not return a table";
        lua_close(state);
        return false;
    }

    lua_getfield(state, -1, "name");
    if (!lua_isstring(state, -1)) {
        error = "modinfo.lua has no string name";
        lua_close(state);
        return false;
    }
    name = lua_tostring(state, -1);
    lua_close(state);
    return true;
}

}  // namespace bar
