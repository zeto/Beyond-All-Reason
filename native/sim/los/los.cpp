#include "los/los.h"

#include "move/unit_sim.h"
#include "smf.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace bar {
namespace {

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool open_unit_table(lua_State* state, const std::string& game_root, const std::string& def_name, std::string& error) {
    const std::filesystem::path root = std::filesystem::path(game_root) / "units";
    const std::filesystem::path direct = root / (def_name + ".lua");
    std::filesystem::path path;
    std::error_code ec;
    if (std::filesystem::is_regular_file(direct, ec)) {
        path = direct;
    } else {
        const std::string file_name = def_name + ".lua";
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() == file_name) {
                path = it->path();
                break;
            }
        }
    }
    if (path.empty()) {
        error = "unit def not found: " + def_name;
        return false;
    }
    if (luaL_loadfile(state, path.string().c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "unit def failed";
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "unit def did not return a table: " + def_name;
        return false;
    }
    lua_getfield(state, -1, def_name.c_str());
    if (!lua_istable(state, -1)) {
        error = "unit def has no " + def_name + " table";
        return false;
    }
    return true;
}

bool lua_flag(lua_State* state, const char* key) {
    lua_getfield(state, -1, key);
    bool value = false;
    if (lua_isboolean(state, -1)) {
        value = lua_toboolean(state, -1) != 0;
    } else if (lua_isnumber(state, -1)) {
        value = lua_tonumber(state, -1) != 0.0;
    } else if (lua_isstring(state, -1)) {
        const std::string text = lower_copy(lua_tostring(state, -1));
        value = text == "true" || text == "1";
    }
    lua_pop(state, 1);
    return value;
}

bool load_ground_unit(const std::string& game_root, const std::string& def_name, bool& can_fly, float& sight, float& lift) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        return false;
    }
    luaL_openlibs(state);
    std::string error;
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    can_fly = lua_flag(state, "canfly");
    lua_getfield(state, -1, "sightdistance");
    const bool have_sight = lua_isnumber(state, -1) != 0 && lua_tonumber(state, -1) > 0.0;
    sight = have_sight ? static_cast<float>(lua_tonumber(state, -1)) : 0.f;
    lua_pop(state, 1);
    lua_getfield(state, -1, "collisionvolumetype");
    const char* type_text = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    const std::string type = lower_copy(type_text);
    const bool cylinder = type == "cyly" || type == "cylx" || type == "cylz" || type == "cylinder";
    if (type != "box" && !cylinder) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "collisionvolumescales");
    const char* scales = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    lua_getfield(state, -1, "collisionvolumeoffsets");
    const char* offsets = lua_isstring(state, -1) ? lua_tostring(state, -1) : "0 0 0";
    lua_pop(state, 1);
    std::istringstream scale_text(scales);
    std::istringstream offset_text(offsets);
    float sx = 0;
    float sy = 0;
    float sz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
    if (!(scale_text >> sx >> sy >> sz) || !(sy > 0.f)) {
        lua_close(state);
        return false;
    }
    if (!(offset_text >> ox >> oy >> oz)) {
        oy = 0;
    }
    lift = oy + sy * 0.5f;
    lua_close(state);
    return have_sight || can_fly;
}

bool los_square(const std::string& game_root, const TerrainMesh& terrain, float& square) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        return false;
    }
    luaL_openlibs(state);
    const char* stub =
        "VFS = { Include = function() return nil end, DirList = function() return {} end }\n"
        "Script = { IsEngineMinVersion = function() return true end }\n"
        "Spring = { GetModOptions = function() return {} end, Echo = function() end }\n";
    const std::string path = game_root + "/gamedata/modrules.lua";
    if (luaL_dostring(state, stub) != 0 || luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0 ||
        !lua_istable(state, -1)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "sensors");
    if (!lua_istable(state, -1)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "los");
    if (!lua_istable(state, -1)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "losMipLevel");
    if (!lua_isnumber(state, -1)) {
        lua_close(state);
        return false;
    }
    const float mip_f = static_cast<float>(lua_tonumber(state, -1));
    lua_close(state);
    const int mip = static_cast<int>(mip_f);
    if (mip < 0 || mip > 6 || static_cast<float>(mip) != mip_f || terrain.square_size <= 0) {
        return false;
    }
    square = static_cast<float>(terrain.square_size * (1 << mip));
    return square > 0.f;
}

}  // namespace

bool los_map_square(const std::string& game_root, const TerrainMesh& terrain, float& square) {
    return los_square(game_root, terrain, square);
}

bool los_consider(const std::string& game_root, int attacker, int target, const UnitSim& sim, const TerrainMesh& terrain,
                  bool& applied, LosCheck& check, std::string& error) {
    applied = false;
    check = LosCheck{};
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "los order has no unit";
        return false;
    }
    const size_t from = static_cast<size_t>(attacker);
    const size_t to = static_cast<size_t>(target);
    if (sim.y[from] < 0.f || sim.y[to] < 0.f) {
        return true;
    }
    bool attacker_fly = false;
    bool target_fly = false;
    float sight = 0;
    float target_sight = 0;
    float attacker_lift = 0;
    float target_lift = 0;
    if (!load_ground_unit(game_root, sim.def_name[from], attacker_fly, sight, attacker_lift) ||
        !load_ground_unit(game_root, sim.def_name[to], target_fly, target_sight, target_lift)) {
        return true;
    }
    (void)target_sight;
    if (attacker_fly || target_fly || !(sight > 0.f)) {
        return true;
    }
    float square = 0;
    if (!los_square(game_root, terrain, square)) {
        return true;
    }
    const float dx = sim.x[to] - sim.x[from];
    const float dz = sim.z[to] - sim.z[from];
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float eye_s = sim.y[from] + attacker_lift;
    const float eye_t = sim.y[to] + target_lift;
    const int steps = dist > 0.f ? static_cast<int>(std::floor(dist / square)) : 0;
    bool blocked = false;
    check.heights.reserve(static_cast<size_t>(std::max(0, steps - 1)));
    for (int i = 1; i < steps; ++i) {
        const float t = (static_cast<float>(i) * square) / dist;
        const float x = sim.x[from] + dx * t;
        const float z = sim.z[from] + dz * t;
        const float ground = terrain_height_at(terrain, x, z);
        const float line = eye_s + (eye_t - eye_s) * t;
        check.heights.push_back(ground);
        if (ground > line) {
            blocked = true;
        }
    }
    const bool far = !(dist > 0.f) ? false : dist > sight;
    check.tag = far ? "far" : (blocked ? "block" : "open");
    check.attacker_def = sim.def_name[from];
    check.target_def = sim.def_name[to];
    check.attacker = attacker;
    check.target = target;
    check.see = far || blocked ? 0 : 1;
    check.far = far ? 1 : 0;
    check.weapon = 0;
    check.dist = dist;
    check.sight = sight;
    check.square = square;
    check.attacker_lift = attacker_lift;
    check.target_lift = target_lift;
    applied = true;
    return true;
}

}  // namespace bar
