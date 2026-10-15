#include "unitdef.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <filesystem>

namespace bar {
namespace {

bool open_unit_table(lua_State* state, const std::string& game_root, const std::string& def_name, std::string& error) {
    const std::filesystem::path root = std::filesystem::path(game_root) / "units";
    const std::filesystem::path direct = root / (def_name + ".lua");
    std::filesystem::path path;
    std::error_code ec;
    if (std::filesystem::is_regular_file(direct, ec)) {
        path = direct;
    } else {
        const std::string file_name = def_name + ".lua";
        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec) || it->path().filename() != file_name) {
                continue;
            }
            path = it->path();
            break;
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

float lua_number(lua_State* state, const char* key, float fallback) {
    lua_getfield(state, -1, key);
    const float value = lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
    lua_pop(state, 1);
    return value;
}

}  // namespace

bool load_unit_object(const std::string& game_root, const std::string& def_name, std::string& object_name,
                      std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "objectname");
    if (!lua_isstring(state, -1)) {
        error = def_name + " has no objectname";
        lua_close(state);
        return false;
    }
    object_name = lua_tostring(state, -1);
    lua_close(state);
    return !object_name.empty();
}

bool load_unit_yield(const std::string& game_root, const std::string& def_name, UnitYield& yield,
                     std::string& error) {
    yield = UnitYield{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    yield.metal_make = lua_number(state, "metalmake", 0);
    yield.energy_make = lua_number(state, "energymake", 0);
    yield.metal_storage = lua_number(state, "metalstorage", 0);
    yield.energy_storage = lua_number(state, "energystorage", 0);
    lua_close(state);
    return true;
}

bool load_unit_extractor(const std::string& game_root, const std::string& def_name, UnitExtractor& extractor,
                         std::string& error) {
    extractor = UnitExtractor{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "activatewhenbuilt");
    extractor.active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    extractor.depth = lua_number(state, "extractsmetal", 0);
    extractor.energy_upkeep = lua_number(state, "energyupkeep", 0);
    lua_getfield(state, -1, "customparams");
    if (lua_istable(state, -1)) {
        extractor.metal_extractor = lua_number(state, "metal_extractor", 0);
    }
    lua_pop(state, 1);
    lua_close(state);
    return true;
}

bool load_unit_power(const std::string& game_root, const std::string& def_name, UnitPower& power, std::string& error) {
    power = UnitPower{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "activatewhenbuilt");
    power.active = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    power.energy_upkeep = lua_number(state, "energyupkeep", 0);
    power.wind_generator = lua_number(state, "windgenerator", 0);
    lua_close(state);
    return true;
}

bool load_unit_build(const std::string& game_root, const std::string& def_name, UnitBuild& build, std::string& error) {
    build = UnitBuild{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    build.build_time = lua_number(state, "buildtime", 0);
    build.metal_cost = lua_number(state, "metalcost", 0);
    build.energy_cost = lua_number(state, "energycost", 0);
    build.health = lua_number(state, "health", 0);
    build.footprint_x = static_cast<int>(lua_number(state, "footprintx", 0));
    build.footprint_z = static_cast<int>(lua_number(state, "footprintz", 0));
    lua_close(state);
    return true;
}

bool load_unit_builder(const std::string& game_root, const std::string& def_name, UnitBuilder& builder,
                       std::string& error) {
    builder = UnitBuilder{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "builder");
    builder.builder = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    builder.worker_time = lua_number(state, "workertime", 0);
    builder.build_distance = lua_number(state, "builddistance", 0);
    lua_getfield(state, -1, "buildoptions");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_isstring(state, -1)) {
                builder.options.emplace_back(lua_tostring(state, -1));
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    lua_close(state);
    return true;
}

bool load_unit_role(const std::string& game_root, const std::string& def_name, UnitRole& role, std::string& error) {
    role = UnitRole{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    auto flag = [&](const char* key) {
        lua_getfield(state, -1, key);
        const bool value = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        return value;
    };
    auto present = [&](const char* key) {
        lua_getfield(state, -1, key);
        const bool value = lua_isnil(state, -1) == 0 && lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        return value;
    };
    role.builder = flag("builder");
    role.can_move = flag("canmove");
    role.extracts_metal = lua_number(state, "extractsmetal", 0);
    role.energy_make = lua_number(state, "energymake", 0);
    lua_getfield(state, -1, "energyupkeep");
    role.has_upkeep = lua_isnumber(state, -1) != 0;
    if (role.has_upkeep) {
        role.energy_upkeep = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    role.wind_generator = lua_number(state, "windgenerator", 0);
    role.tidal_generator = lua_number(state, "tidalgenerator", 0);
    role.transport_capacity = lua_number(state, "transportcapacity", 0);
    role.footprint_x = static_cast<int>(lua_number(state, "footprintx", 0));
    role.footprint_z = static_cast<int>(lua_number(state, "footprintz", 0));
    role.building = !role.can_move && role.footprint_x > 0;
    role.speed = lua_number(state, "speed", 0);
    role.worker_time = lua_number(state, "workertime", 0);
    role.can_fly = flag("canfly");
    lua_getfield(state, -1, "yardmap");
    role.yardmap = lua_isnil(state, -1) == 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "weapons");
    role.has_weapon = lua_istable(state, -1) != 0 && lua_objlen(state, -1) > 0;
    lua_pop(state, 1);
    // Pin rule: a factory is an immobile yardmap builder. A mex has a yardmap and is not a builder.
    role.factory = role.builder && role.worker_time > 0.f && !role.can_fly && role.speed <= 0.f && role.yardmap &&
                   role.footprint_x > 0;
    lua_getfield(state, -1, "customparams");
    if (lua_istable(state, -1)) {
        role.commander = present("iscommander");
        role.metal_extractor = present("metal_extractor");
        role.solar = present("solar");
        const bool capacity = present("energyconv_capacity");
        const bool efficiency = present("energyconv_efficiency");
        role.converter = capacity && efficiency;
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "buildoptions");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_isstring(state, -1)) {
                role.options.emplace_back(lua_tostring(state, -1));
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    lua_close(state);
    role.ok = true;
    return true;
}


bool load_unit_health(const std::string& game_root, const std::string& def_name, float& health, std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "health");
    if (!lua_isnumber(state, -1) || !(lua_tonumber(state, -1) > 0.0)) {
        error = def_name + " has no health";
        lua_close(state);
        return false;
    }
    health = static_cast<float>(lua_tonumber(state, -1));
    lua_close(state);
    return true;
}

bool load_unit_max_weapon_range(const std::string& game_root, const std::string& def_name, float& range,
                                std::string& error) {
    range = 0.f;
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    // BAR stores ranges under weapondefs = { name = { range = N } }.
    lua_getfield(state, -1, "weapondefs");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_istable(state, -1)) {
                lua_getfield(state, -1, "range");
                if (lua_isnumber(state, -1)) {
                    const float r = static_cast<float>(lua_tonumber(state, -1));
                    if (r > range) {
                        range = r;
                    }
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
    }
    lua_close(state);
    return true;
}

bool load_unit_wreck(const std::string& game_root, const std::string& def_name, UnitWreck& wreck, std::string& error) {
    wreck = UnitWreck{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "customparams");
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "iscommander");
        wreck.commander = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getfield(state, -1, "iseffigy");
        wreck.effigy = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "health");
    wreck.has_health = lua_isnumber(state, -1) != 0;
    if (wreck.has_health) {
        wreck.health = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "metalcost");
    wreck.has_metal_cost = lua_isnumber(state, -1) != 0;
    if (wreck.has_metal_cost) {
        wreck.metal_cost = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "energycost");
    wreck.has_energy_cost = lua_isnumber(state, -1) != 0;
    if (wreck.has_energy_cost) {
        wreck.energy_cost = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "featuredefs");
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "dead");
        if (lua_istable(state, -1)) {
            wreck.present = true;
            wreck.metal = lua_number(state, "metal", 0);
            wreck.energy = lua_number(state, "energy", 0);
            lua_getfield(state, -1, "reclaimable");
            wreck.reclaimable = lua_isnil(state, -1) != 0 || lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            lua_getfield(state, -1, "reclaimtime");
            if (lua_isnumber(state, -1) == 0) {
                lua_pop(state, 1);
                lua_getfield(state, -1, "reclaimTime");
            }
            wreck.has_reclaim_time = lua_isnumber(state, -1) != 0;
            if (wreck.has_reclaim_time) {
                wreck.reclaim_time = static_cast<float>(lua_tonumber(state, -1));
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    lua_close(state);
    return true;
}

bool load_unit_sensors(const std::string& game_root, const std::string& def_name, UnitSensors& sensors,
                       std::string& error) {
    sensors = UnitSensors{};
    std::string file_name = def_name;
    const std::string suffix = "_scav";
    if (file_name.size() > suffix.size() && file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        file_name.resize(file_name.size() - suffix.size());
    }
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, file_name, error)) {
        lua_close(state);
        return false;
    }
    sensors.sight = lua_number(state, "sightdistance", 0);
    lua_getfield(state, -1, "airsightdistance");
    if (lua_isnumber(state, -1)) {
        sensors.air = static_cast<float>(lua_tonumber(state, -1));
    } else {
        sensors.air = sensors.sight * 1.5f;
    }
    lua_pop(state, 1);
    sensors.radar = lua_number(state, "radardistance", 0);
    sensors.sonar = lua_number(state, "sonardistance", 0);
    lua_getfield(state, -1, "sightemitheight");
    if (lua_isnumber(state, -1)) {
        sensors.sight_emit = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "radaremitheight");
    if (lua_isnumber(state, -1)) {
        sensors.radar_emit = static_cast<float>(lua_tonumber(state, -1));
    } else {
        sensors.radar_emit = sensors.sight_emit;
    }
    lua_pop(state, 1);
    lua_close(state);
    return true;
}

bool load_unit_reclaim(const std::string& game_root, const std::string& def_name, UnitReclaim& reclaim,
                       std::string& error) {
    reclaim = UnitReclaim{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "builder");
    const bool builder = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    reclaim.worker_time = lua_number(state, "workertime", 0);
    reclaim.build_distance = lua_number(state, "builddistance", 0);
    lua_getfield(state, -1, "reclaimspeed");
    if (lua_isnumber(state, -1) == 0) {
        lua_pop(state, 1);
        lua_getfield(state, -1, "reclaimSpeed");
    }
    reclaim.explicit_speed = lua_isnumber(state, -1) != 0;
    if (reclaim.explicit_speed) {
        reclaim.reclaim_speed = static_cast<float>(lua_tonumber(state, -1));
    } else {
        reclaim.reclaim_speed = reclaim.worker_time;
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "canreclaim");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_getfield(state, -1, "canReclaim");
    }
    const bool listed = lua_isnil(state, -1) == 0;
    const bool allowed = listed ? lua_toboolean(state, -1) != 0 : builder;
    lua_pop(state, 1);
    reclaim.can_reclaim = allowed && reclaim.reclaim_speed > 0.f;
    lua_close(state);
    return true;
}

bool load_unit_motion(const std::string& game_root, const std::string& def_name, UnitMotion& motion,
                      std::string& error) {
    motion = UnitMotion{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "canmove");
    motion.can_move = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    motion.speed_per_sec = lua_number(state, "speed", 0);
    motion.max_acc = lua_number(state, "maxacc", 0);
    motion.max_dec = lua_number(state, "maxdec", motion.max_acc);
    motion.turn_rate_raw = lua_number(state, "turnrate", 0);
    motion.waterline = lua_number(state, "waterline", 0);
    lua_getfield(state, -1, "floater");
    motion.floater = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "canfly");
    motion.can_fly = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "hoverattack");
    motion.hover_attack = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    motion.cruise_altitude = lua_number(state, "cruisealtitude", 0);
    motion.max_rudder = lua_number(state, "maxrudder", 0);
    motion.max_aileron = lua_number(state, "maxaileron", 0);
    motion.max_bank = lua_number(state, "maxbank", 0);
    motion.max_pitch = lua_number(state, "maxpitch", 0);
    motion.turn_radius = lua_number(state, "turnradius", 0);
    lua_getfield(state, -1, "movementclass");
    if (lua_isstring(state, -1)) {
        motion.move_class = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
    lua_close(state);
    return true;
}

bool load_move_class(const std::string& game_root, const std::string& class_name, MoveClass& move_class,
                     std::string& error) {
    const std::filesystem::path path = std::filesystem::path(game_root) / "gamedata" / "movedefs.lua";
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    const char* stub =
        "Game = { speedModClasses = {} }\n"
        "function table.any(list, pred)\n"
        "  for _, value in ipairs(list) do\n"
        "    if pred(value) then return true end\n"
        "  end\n"
        "  return false\n"
        "end\n";
    if (luaL_dostring(state, stub) != 0) {
        error = "move class stub failed";
        lua_close(state);
        return false;
    }
    if (luaL_loadfile(state, path.string().c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "movedefs failed";
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "movedefs did not return a table";
        lua_close(state);
        return false;
    }
    const int count = static_cast<int>(lua_objlen(state, -1));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, i);
        lua_getfield(state, -1, "name");
        const bool match = lua_isstring(state, -1) && class_name == lua_tostring(state, -1);
        lua_pop(state, 1);
        if (match) {
            lua_getfield(state, -1, "maxslope");
            const bool has_slope = lua_isnumber(state, -1) != 0;
            // movedefs divides by 1.5 and notes that the engine multiplies it back.
            const float slope = has_slope ? static_cast<float>(lua_tonumber(state, -1)) * 1.5f : 0.f;
            lua_pop(state, 1);
            lua_getfield(state, -1, "maxwaterdepth");
            const bool has_max = lua_isnumber(state, -1) != 0;
            const float max_water = has_max ? static_cast<float>(lua_tonumber(state, -1)) : 1.0e6f;
            lua_pop(state, 1);
            lua_getfield(state, -1, "minwaterdepth");
            const bool has_min = lua_isnumber(state, -1) != 0;
            const float min_water = has_min ? static_cast<float>(lua_tonumber(state, -1)) : -1.0e6f;
            lua_pop(state, 2);
            move_class.max_water_depth = max_water;
            move_class.min_water_depth = min_water;
            // A ship omits maxslope. 90 keeps an underwater ridge from blocking a hull that is not on the floor.
            move_class.max_slope_deg = has_slope ? slope : (min_water > 0.f ? 90.f : 0.f);
            move_class.hover = class_name.find("HOVER") != std::string::npos;
            lua_close(state);
            return true;
        }
        lua_pop(state, 1);
    }
    error = "move class not found: " + class_name;
    lua_close(state);
    return false;
}

}  // namespace bar
