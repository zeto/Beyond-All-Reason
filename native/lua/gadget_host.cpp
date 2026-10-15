#include "gadget_host.h"

#include "los/los.h"
#include "widget_host.h"
#include "smf.h"
#include "unitdef.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace bar {
namespace {

struct RuleValue {
    bool number = false;
    double numeric = 0;
    std::string text;
};

struct SpawnAsk {
    std::string def_name;
    int team = -1;
    float x = 0;
    float y = 0;
    float z = 0;
    int unit = -1;
    int hide = 0;
    int paralyze = 0;
    int movectrl = 0;
};

struct AiOrder {
    int unit = 0;
    int cmd = 0;
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
};

// One queued command. State toggles never enter this list. A build keeps its nano
// until the frame finishes, so GetUnitCommandCount stays above zero while it is built.
struct LiveOrder {
    int unit = 0;
    int cmd = 0;
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    bool known = false;
    bool factory = false;
    bool failed = false;
    float reach = 0;
    float site_x = 0;
    float site_y = 0;
    float site_z = 0;
    Nanoframe nano;
};

struct Host {
    const StartScript* script = nullptr;
    const ResourceRules* rules = nullptr;
    std::vector<TeamResources>* teams = nullptr;
    UnitSim* sim = nullptr;
    const TerrainMesh* terrain = nullptr;
    std::string game_root;
    std::map<int, std::string> def_names;
    std::map<std::string, int> def_ids;
    int next_def = 1;
    // False until the spawn seal. GameStart binds. Later CreateUnit adds a unit.
    bool create_units = false;
    std::vector<std::string> mode_keys;
    // Health from a lua file that returns several unit tables. Keyed by the base name.
    std::map<std::string, float> bundle_health;
    std::vector<std::string> bundle_tried;
    std::map<std::string, UnitSensors> sensors;
    bool los_square_ready = false;
    float los_square = 0;
    // Experience keyed by sim id. The scavenger spawner rolls this onto a new burrow.
    std::map<int, float> experience;
    std::map<int, std::map<std::string, RuleValue>> team_rules;
    std::map<std::string, RuleValue> game_rules;
    std::vector<SpawnAsk> spawns;
    float metal = 0;
    float energy = 0;
    float metal_store = 0;
    float energy_store = 0;
    // Bits 1 metal, 2 energy, 4 metal storage, 8 energy storage. Each script team needs 15.
    std::map<int, int> resource_mask;
    std::string ai_name;
    bool ai_enabled = false;
    bool ai_loaded = false;
    std::vector<AiOrder> ai_orders;
    std::vector<LiveOrder> live;
};

Host* host_from(lua_State* state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "bar_host");
    auto* host = static_cast<Host*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return host;
}

TeamResources* resources_for(Host& host, int team) {
    if (host.teams == nullptr) {
        return nullptr;
    }
    for (TeamResources& row : *host.teams) {
        if (row.team == team) {
            return &row;
        }
    }
    return nullptr;
}

int front_team(const Host& host) {
    if (host.script == nullptr || host.script->teams.empty()) {
        return -1;
    }
    return host.script->teams.front().team;
}

bool state_command(int cmd) {
    // Cloak, fire state, and move state are toggles. They must not wipe a build.
    return cmd == 37382 || cmd == 45 || cmd == 50;
}

bool order_shift(lua_State* state) {
    if (!lua_istable(state, 4)) {
        return false;
    }
    const int count = static_cast<int>(lua_objlen(state, 4));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, 4, i);
        const bool shift = lua_isstring(state, -1) != 0 && std::string(lua_tostring(state, -1)) == "shift";
        lua_pop(state, 1);
        if (shift) {
            return true;
        }
    }
    return false;
}

bool safe_relative(const std::string& path) {
    return !path.empty() && path.front() != '/' && path.find("..") == std::string::npos;
}

int vfs_include(lua_State* state) {
    Host* host = host_from(state);
    const char* relative = luaL_checkstring(state, 1);
    const int args = lua_gettop(state);
    if (host == nullptr || !safe_relative(relative)) {
        return luaL_error(state, "vfs path refused");
    }
    const std::string full = host->game_root + "/" + relative;
    if (luaL_loadfile(state, full.c_str()) != 0) {
        return lua_error(state);
    }
    if (lua_pcall(state, 0, LUA_MULTRET, 0) != 0) {
        return lua_error(state);
    }
    // Extra arguments, including a nil archive mode, stay below the chunk results.
    return lua_gettop(state) - args;
}

int vfs_zlib(lua_State* state) {
    return luaL_error(state, "zlib is not hosted");
}

void push_rule(lua_State* state, const RuleValue& value) {
    if (value.number) {
        lua_pushnumber(state, value.numeric);
    } else {
        lua_pushlstring(state, value.text.data(), value.text.size());
    }
}

bool read_rule(lua_State* state, int index, RuleValue& value) {
    if (lua_isnil(state, index)) {
        return false;
    }
    if (lua_isnumber(state, index)) {
        value.number = true;
        value.numeric = lua_tonumber(state, index);
        return true;
    }
    if (lua_isstring(state, index)) {
        value.number = false;
        value.text = lua_tostring(state, index);
        return true;
    }
    return false;
}

int spring_get_mod_options(lua_State* state) {
    const Host* host = host_from(state);
    lua_newtable(state);
    if (host == nullptr || host->rules == nullptr) {
        lua_pushstring(state, "");
        lua_setfield(state, -2, "debugcommands");
        return 1;
    }
    const ResourceRules& rules = *host->rules;
    lua_pushnumber(state, rules.start_metal);
    lua_setfield(state, -2, "startmetal");
    lua_pushnumber(state, rules.start_energy);
    lua_setfield(state, -2, "startenergy");
    lua_pushnumber(state, rules.start_metal_storage);
    lua_setfield(state, -2, "startmetalstorage");
    lua_pushnumber(state, rules.start_energy_storage);
    lua_setfield(state, -2, "startenergystorage");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "proposed_unit_reworks");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "bonusstartresourcemultiplier");
    // File defaults from modoptions.lua. Absent keys only. Restriction flags stay nil.
    lua_pushstring(state, "normal");
    lua_setfield(state, -2, "scav_difficulty");
    lua_pushstring(state, "initialbox");
    lua_setfield(state, -2, "scav_scavstart");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "scav_endless");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "scav_bosstimemult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "scav_boss_count");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "scav_spawncountmult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "scav_spawntimemult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "scav_graceperiodmult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "multiplier_resourceincome");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "multiplier_metalextraction");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "multiplier_energyconversion");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "multiplier_energyproduction");
    lua_pushstring(state, "normal");
    lua_setfield(state, -2, "raptor_difficulty");
    lua_pushstring(state, "initialbox");
    lua_setfield(state, -2, "raptor_raptorstart");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "raptor_endless");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_queentimemult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_queen_count");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_spawncountmult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_firstwavesboost");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_spawntimemult");
    lua_pushnumber(state, 1);
    lua_setfield(state, -2, "raptor_graceperiodmult");
    // gui_top_bar checks this string for "invertmap". The file default is empty.
    lua_pushstring(state, "");
    lua_setfield(state, -2, "debugcommands");
    return 1;
}

int spring_get_game_frame(lua_State* state) {
    lua_pushnumber(state, 0);
    return 1;
}

int spring_get_game_seconds(lua_State* state) {
    lua_pushnumber(state, 0);
    return 1;
}

int spring_get_gaia(lua_State* state) {
    lua_pushnumber(state, 255);
    return 1;
}

int spring_log(lua_State* state) {
    const char* section = lua_tostring(state, 1);
    const char* level = lua_tostring(state, 2);
    const char* message = lua_tostring(state, 3);
    std::cout << "gadget log " << (section != nullptr ? section : "") << " " << (level != nullptr ? level : "") << " "
              << (message != nullptr ? message : "") << "\n";
    return 0;
}

int spring_echo(lua_State* state) {
    const int count = lua_gettop(state);
    std::cout << "gadget echo";
    for (int i = 1; i <= count; ++i) {
        const char* text = lua_tostring(state, i);
        std::cout << " " << (text != nullptr ? text : "?");
    }
    std::cout << "\n";
    return 0;
}

int spring_noop(lua_State* state) {
    (void)state;
    return 0;
}

const TeamStart* find_team(const Host& host, int team) {
    if (host.script == nullptr) {
        return nullptr;
    }
    for (const TeamStart& one : host.script->teams) {
        if (one.team == team) {
            return &one;
        }
    }
    return nullptr;
}

int spring_get_team_list(lua_State* state) {
    const Host* host = host_from(state);
    const bool filter = lua_gettop(state) >= 1 && !lua_isnil(state, 1);
    const int ally = filter ? static_cast<int>(luaL_checknumber(state, 1)) : -1;
    lua_newtable(state);
    int count = 0;
    if (host != nullptr && host->script != nullptr) {
        for (const TeamStart& team : host->script->teams) {
            if (filter && team.ally_team != ally) {
                continue;
            }
            lua_pushinteger(state, team.team);
            lua_rawseti(state, -2, ++count);
        }
    }
    // Gaia is not a script team. The unfiltered list still includes it, and callers that
    // pass an ally team do not. Resources and the spawn gadget already skip this id.
    if (!filter) {
        lua_pushinteger(state, 255);
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int spring_get_ally_list(lua_State* state) {
    const Host* host = host_from(state);
    lua_newtable(state);
    int count = 0;
    std::vector<int> seen;
    if (host != nullptr && host->script != nullptr) {
        for (const TeamStart& team : host->script->teams) {
            bool have = false;
            for (int ally : seen) {
                if (ally == team.ally_team) {
                    have = true;
                }
            }
            if (have) {
                continue;
            }
            seen.push_back(team.ally_team);
            lua_pushinteger(state, team.ally_team);
            lua_rawseti(state, -2, ++count);
        }
    }
    return 1;
}

int spring_get_player_list(lua_State* state) {
    const Host* host = host_from(state);
    const bool filter = lua_gettop(state) >= 1 && !lua_isnil(state, 1);
    const int team = filter ? static_cast<int>(luaL_checknumber(state, 1)) : 0;
    lua_newtable(state);
    if (host != nullptr && host->script != nullptr && !host->script->teams.empty()) {
        const int home = host->script->teams.front().team;
        if (!filter || team == home) {
            lua_pushinteger(state, 0);
            lua_rawseti(state, -2, 1);
        }
    }
    return 1;
}

int spring_get_player_info(lua_State* state) {
    const Host* host = host_from(state);
    const int player = static_cast<int>(luaL_checknumber(state, 1));
    if (host == nullptr || host->script == nullptr || host->script->teams.empty() || player != 0) {
        return 0;
    }
    const TeamStart& team = host->script->teams.front();
    lua_pushstring(state, "Player");
    lua_pushboolean(state, 1);
    lua_pushboolean(state, 0);
    lua_pushinteger(state, team.team);
    lua_pushinteger(state, team.ally_team);
    return 5;
}

int spring_get_team_info(lua_State* state) {
    const Host* host = host_from(state);
    const int team_id = static_cast<int>(luaL_checknumber(state, 1));
    const TeamStart* team = host == nullptr ? nullptr : find_team(*host, team_id);
    if (team == nullptr) {
        return 0;
    }
    lua_pushinteger(state, team->team);
    lua_pushinteger(state, 0);
    lua_pushboolean(state, 0);
    lua_pushboolean(state, 0);
    lua_pushstring(state, team->side.c_str());
    lua_pushinteger(state, team->ally_team);
    lua_pushnumber(state, 1);
    return 7;
}

int spring_get_team_lua_ai(lua_State* state) {
    const Host* host = host_from(state);
    const int team_id = static_cast<int>(luaL_checknumber(state, 1));
    const TeamStart* team = host == nullptr ? nullptr : find_team(*host, team_id);
    if (team == nullptr || team->lua_ai.empty()) {
        return 0;
    }
    lua_pushstring(state, team->lua_ai.c_str());
    return 1;
}

int spring_get_team_ally(lua_State* state) {
    const Host* host = host_from(state);
    const int team_id = static_cast<int>(luaL_checknumber(state, 1));
    const TeamStart* team = host == nullptr ? nullptr : find_team(*host, team_id);
    if (team == nullptr) {
        return 0;
    }
    lua_pushinteger(state, team->ally_team);
    return 1;
}

int spring_get_team_rules(lua_State* state) {
    Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const char* key = luaL_checkstring(state, 2);
    if (host == nullptr) {
        return 0;
    }
    const auto team_at = host->team_rules.find(team);
    if (team_at == host->team_rules.end()) {
        if (team == 255) {
            lua_pushnumber(state, 0);
            return 1;
        }
        return 0;
    }
    const auto key_at = team_at->second.find(key);
    if (key_at == team_at->second.end()) {
        if (team == 255) {
            lua_pushnumber(state, 0);
            return 1;
        }
        return 0;
    }
    push_rule(state, key_at->second);
    return 1;
}

int spring_set_team_rules(lua_State* state) {
    Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const char* key = luaL_checkstring(state, 2);
    if (host == nullptr) {
        return 0;
    }
    RuleValue value;
    if (!read_rule(state, 3, value)) {
        host->team_rules[team].erase(key);
        return 0;
    }
    host->team_rules[team][key] = value;
    return 0;
}

int spring_get_game_rules(lua_State* state) {
    Host* host = host_from(state);
    const char* key = luaL_checkstring(state, 1);
    if (host == nullptr) {
        return 0;
    }
    const auto at = host->game_rules.find(key);
    if (at == host->game_rules.end()) {
        return 0;
    }
    push_rule(state, at->second);
    return 1;
}

int spring_set_game_rules(lua_State* state) {
    Host* host = host_from(state);
    const char* key = luaL_checkstring(state, 1);
    if (host == nullptr) {
        return 0;
    }
    RuleValue value;
    if (!read_rule(state, 2, value)) {
        host->game_rules.erase(key);
        return 0;
    }
    host->game_rules[key] = value;
    return 0;
}

int spring_get_start_pos(lua_State* state) {
    const Host* host = host_from(state);
    const int team_id = static_cast<int>(luaL_checknumber(state, 1));
    const TeamStart* team = host == nullptr ? nullptr : find_team(*host, team_id);
    if (team == nullptr || host->terrain == nullptr) {
        return 0;
    }
    const float y = terrain_height_at(*host->terrain, team->start_x, team->start_z);
    lua_pushnumber(state, team->start_x);
    lua_pushnumber(state, y);
    lua_pushnumber(state, team->start_z);
    return 3;
}

int spring_get_ground_height(lua_State* state) {
    const Host* host = host_from(state);
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float z = static_cast<float>(luaL_checknumber(state, 2));
    if (host == nullptr || host->terrain == nullptr) {
        lua_pushnumber(state, 0);
        return 1;
    }
    lua_pushnumber(state, terrain_height_at(*host->terrain, x, z));
    return 1;
}

int spring_get_start_box(lua_State* state) {
    const Host* host = host_from(state);
    (void)luaL_checknumber(state, 1);
    const float width = host != nullptr && host->terrain != nullptr ? host->terrain->world_x : 0.f;
    const float height = host != nullptr && host->terrain != nullptr ? host->terrain->world_z : 0.f;
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, width);
    lua_pushnumber(state, height);
    return 4;
}

bool unit_is_alive(const UnitSim& sim, size_t index) {
    return index >= sim.alive.size() || sim.alive[index] != 0;
}

const UnitSensors& sensors_for(Host& host, const std::string& def_name) {
    const auto found = host.sensors.find(def_name);
    if (found != host.sensors.end()) {
        return found->second;
    }
    UnitSensors loaded;
    std::string error;
    if (!load_unit_sensors(host.game_root, def_name, loaded, error)) {
        loaded = UnitSensors{};
    }
    return host.sensors.insert({def_name, loaded}).first->second;
}

bool sensor_square(Host& host) {
    if (host.los_square_ready) {
        return host.los_square > 0.f;
    }
    host.los_square_ready = true;
    if (host.terrain == nullptr) {
        return false;
    }
    return los_map_square(host.game_root, *host.terrain, host.los_square);
}

// Same los-square walk as the weapon check. The eye is the unit plus its emit height.
bool sight_line_clear(const TerrainMesh& terrain, float square, float x0, float eye, float z0, float x1, float y1,
                      float z1) {
    const float dx = x1 - x0;
    const float dz = z1 - z0;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (!(dist > 0.f) || !(square > 0.f)) {
        return true;
    }
    const int steps = static_cast<int>(std::floor(dist / square));
    for (int i = 1; i < steps; ++i) {
        const float t = (static_cast<float>(i) * square) / dist;
        const float ground = terrain_height_at(terrain, x0 + dx * t, z0 + dz * t);
        const float line = eye + (y1 - eye) * t;
        if (ground > line) {
            return false;
        }
    }
    return true;
}

enum class SensorKind { Los, Air, Radar };

bool ally_sees(Host& host, float x, float y, float z, int ally, SensorKind kind) {
    if (host.sim == nullptr) {
        return false;
    }
    const UnitSim& sim = *host.sim;
    const bool ray = kind != SensorKind::Air && sensor_square(host) && host.terrain != nullptr;
    for (size_t i = 0; i < sim.x.size(); ++i) {
        if (!unit_is_alive(sim, i)) {
            continue;
        }
        const TeamStart* team = find_team(host, sim.team[i]);
        if (team == nullptr || team->ally_team != ally) {
            continue;
        }
        const UnitSensors& sensors = sensors_for(host, sim.def_name[i]);
        float radius = 0;
        float emit = 0;
        const bool circle = kind == SensorKind::Air || (kind == SensorKind::Radar && y < 0.f);
        if (kind == SensorKind::Los) {
            radius = sensors.sight;
            emit = sensors.sight_emit;
        } else if (kind == SensorKind::Air) {
            radius = sensors.air;
        } else if (y < 0.f) {
            radius = sensors.sonar;
        } else {
            radius = sensors.radar;
            emit = sensors.radar_emit;
        }
        if (!(radius > 0.f)) {
            continue;
        }
        const float dx = x - sim.x[i];
        const float dz = z - sim.z[i];
        if (std::sqrt(dx * dx + dz * dz) > radius) {
            continue;
        }
        if (circle || !ray) {
            return true;
        }
        const float eye = std::max(sim.y[i] + emit, 0.f);
        if (sight_line_clear(*host.terrain, host.los_square, sim.x[i], eye, sim.z[i], x, y, z)) {
            return true;
        }
    }
    return false;
}

int spring_pos_in_sensor(lua_State* state, SensorKind kind) {
    Host* host = host_from(state);
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float y = static_cast<float>(luaL_checknumber(state, 2));
    const float z = static_cast<float>(luaL_checknumber(state, 3));
    const int ally = static_cast<int>(luaL_checknumber(state, 4));
    lua_pushboolean(state, host != nullptr && ally_sees(*host, x, y, z, ally, kind));
    return 1;
}

int spring_is_pos_in_los(lua_State* state) {
    return spring_pos_in_sensor(state, SensorKind::Los);
}

int spring_is_pos_in_air_los(lua_State* state) {
    return spring_pos_in_sensor(state, SensorKind::Air);
}

int spring_is_pos_in_radar(lua_State* state) {
    return spring_pos_in_sensor(state, SensorKind::Radar);
}

int find_unit(const UnitSim& sim, const std::string& def_name, int team) {
    for (size_t i = 0; i < sim.def_name.size(); ++i) {
        if (!unit_is_alive(sim, i)) {
            continue;
        }
        if (sim.team[i] == team && sim.def_name[i] == def_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool spawn_extra_unit(Host& host, const std::string& def_name, float x, float y, float z, int team, int& unit_id,
                      std::string& error) {
    if (host.sim == nullptr) {
        error = "createunit has no sim";
        return false;
    }
    UnitMotion motion;
    std::string motion_name = def_name;
    if (!load_unit_motion(host.game_root, motion_name, motion, error)) {
        const std::string suffix = "_scav";
        if (def_name.size() <= suffix.size() ||
            def_name.compare(def_name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
        // The scavenger copy is generated from the base file. Movement numbers are the base file's.
        motion_name = def_name.substr(0, def_name.size() - suffix.size());
        if (!load_unit_motion(host.game_root, motion_name, motion, error)) {
            return false;
        }
    }
    float slope = 20.f;
    float water = 35.f;
    float min_water = -1.0e6f;
    bool hover = false;
    if (!motion.move_class.empty()) {
        MoveClass move_class;
        if (!load_move_class(host.game_root, motion.move_class, move_class, error)) {
            return false;
        }
        slope = move_class.max_slope_deg;
        water = move_class.max_water_depth;
        min_water = move_class.min_water_depth;
        hover = move_class.hover;
    }
    int next_id = 1;
    for (int id : host.sim->id) {
        if (id >= next_id) {
            next_id = id + 1;
        }
    }
    UnitSpawn spawn;
    spawn.id = next_id;
    spawn.team = team;
    spawn.def_name = def_name;
    spawn.x = x;
    spawn.y = y;
    spawn.z = z;
    const float ground = host.terrain != nullptr ? terrain_height_at(*host.terrain, x, z) : y;
    const float cruise = motion.can_fly ? motion.cruise_altitude : 0.f;
    if (!sim_add_unit(*host.sim, spawn, motion, slope, water, min_water, ground, hover, cruise, error)) {
        return false;
    }
    unit_id = next_id;
    std::cout << "createunit " << def_name << " id " << next_id << " team " << team << " at " << x << " " << y << " "
              << z << "\n";
    return true;
}

int spring_create_unit(lua_State* state) {
    Host* host = host_from(state);
    if (host == nullptr || host->sim == nullptr) {
        return 0;
    }
    std::string def_name;
    if (lua_isnumber(state, 1)) {
        const int def_id = static_cast<int>(lua_tonumber(state, 1));
        const auto at = host->def_names.find(def_id);
        if (at == host->def_names.end()) {
            return 0;
        }
        def_name = at->second;
    } else {
        const char* name = lua_tostring(state, 1);
        if (name == nullptr) {
            return 0;
        }
        def_name = name;
    }
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    const float y = static_cast<float>(luaL_checknumber(state, 3));
    const float z = static_cast<float>(luaL_checknumber(state, 4));
    const int team = static_cast<int>(luaL_checknumber(state, 6));
    if (host->create_units) {
        int created = 0;
        std::string error;
        if (!spawn_extra_unit(*host, def_name, x, y, z, team, created, error)) {
            std::cerr << "createunit: " << error << "\n";
            return 0;
        }
        lua_pushinteger(state, created);
        return 1;
    }
    const int unit = find_unit(*host->sim, def_name, team);
    SpawnAsk ask;
    ask.def_name = def_name;
    ask.team = team;
    ask.x = x;
    ask.y = y;
    ask.z = z;
    ask.unit = unit;
    host->spawns.push_back(ask);
    if (unit < 0) {
        return 0;
    }
    lua_pushinteger(state, unit);
    return 1;
}

SpawnAsk* spawn_for(Host& host, int unit) {
    for (SpawnAsk& ask : host.spawns) {
        if (ask.unit == unit) {
            return &ask;
        }
    }
    return nullptr;
}

int spring_move_enable(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host != nullptr) {
        if (SpawnAsk* ask = spawn_for(*host, unit)) {
            ask->movectrl = 1;
        }
    }
    return 0;
}

int spring_set_no_draw(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    const bool hide = lua_toboolean(state, 2) != 0;
    if (host != nullptr && hide) {
        if (SpawnAsk* ask = spawn_for(*host, unit)) {
            ask->hide = 1;
        }
    }
    return 0;
}

int spring_get_unit_health(lua_State* state) {
    (void)luaL_checknumber(state, 1);
    lua_pushnumber(state, 1);
    lua_pushnumber(state, 1);
    lua_pushnumber(state, 0);
    return 3;
}

int spring_set_unit_health(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host != nullptr && lua_istable(state, 2)) {
        lua_getfield(state, 2, "paralyze");
        const bool paralyzes = lua_isnumber(state, -1) && lua_tonumber(state, -1) > 0;
        lua_pop(state, 1);
        if (paralyzes) {
            if (SpawnAsk* ask = spawn_for(*host, unit)) {
                ask->paralyze = 1;
            }
        }
    }
    return 0;
}

int spring_get_blocking(lua_State* state) {
    (void)state;
    lua_pushboolean(state, 0);
    return 1;
}

int spring_teams_allied(lua_State* state) {
    const int left = static_cast<int>(luaL_checknumber(state, 1));
    const int right = static_cast<int>(luaL_checknumber(state, 2));
    lua_pushboolean(state, left == right ? 1 : 0);
    return 1;
}

int sim_slot(const UnitSim& sim, int unit) {
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] == unit) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int spring_set_unit_experience(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    const float xp = static_cast<float>(luaL_checknumber(state, 2));
    if (host == nullptr || host->sim == nullptr || sim_slot(*host->sim, unit) < 0) {
        return 0;
    }
    host->experience[unit] = xp;
    return 0;
}

int spring_valid_unit(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    bool valid = false;
    if (host != nullptr && host->sim != nullptr) {
        const int index = sim_slot(*host->sim, unit);
        valid = index >= 0 && unit_is_alive(*host->sim, static_cast<size_t>(index));
    }
    lua_pushboolean(state, valid ? 1 : 0);
    return 1;
}

int spring_destroy_unit(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host == nullptr || host->sim == nullptr) {
        return 0;
    }
    const int index = sim_slot(*host->sim, unit);
    if (index < 0) {
        return 0;
    }
    const size_t at = static_cast<size_t>(index);
    if (at < host->sim->alive.size()) {
        host->sim->alive[at] = 0;
    }
    host->sim->has_goal[at] = 0;
    host->sim->speed[at] = 0;
    std::cout << "destroyunit id " << unit << " def " << host->sim->def_name[at] << " team " << host->sim->team[at]
              << "\n";
    return 0;
}

int spring_unit_is_dead(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    bool dead = true;
    if (host != nullptr && host->sim != nullptr) {
        const int index = sim_slot(*host->sim, unit);
        if (index >= 0 && unit_is_alive(*host->sim, static_cast<size_t>(index))) {
            dead = false;
        }
    }
    lua_pushboolean(state, dead ? 1 : 0);
    return 1;
}

int spring_team_unit_count(lua_State* state) {
    const Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    int count = 0;
    if (host != nullptr && host->sim != nullptr) {
        const UnitSim& sim = *host->sim;
        for (size_t i = 0; i < sim.id.size(); ++i) {
            if (sim.team[i] == team && unit_is_alive(sim, i)) {
                ++count;
            }
        }
    }
    lua_pushinteger(state, count);
    return 1;
}

int spring_team_unit_def_count(lua_State* state) {
    const Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const int def_id = static_cast<int>(luaL_checknumber(state, 2));
    int count = 0;
    if (host != nullptr && host->sim != nullptr) {
        const auto named = host->def_names.find(def_id);
        if (named != host->def_names.end()) {
            const UnitSim& sim = *host->sim;
            for (size_t i = 0; i < sim.def_name.size(); ++i) {
                if (sim.team[i] == team && sim.def_name[i] == named->second && unit_is_alive(sim, i)) {
                    ++count;
                }
            }
        }
    }
    lua_pushinteger(state, count);
    return 1;
}

int sim_index(const UnitSim& sim, int unit) {
    const int index = sim_slot(sim, unit);
    if (index < 0 || !unit_is_alive(sim, static_cast<size_t>(index))) {
        return -1;
    }
    return index;
}

int spring_unit_base_position(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host == nullptr || host->sim == nullptr) {
        return 0;
    }
    const int index = sim_index(*host->sim, unit);
    if (index < 0) {
        return 0;
    }
    lua_pushnumber(state, host->sim->x[static_cast<size_t>(index)]);
    lua_pushnumber(state, host->sim->y[static_cast<size_t>(index)]);
    lua_pushnumber(state, host->sim->z[static_cast<size_t>(index)]);
    return 3;
}

int spring_unit_position(lua_State* state) {
    return spring_unit_base_position(state);
}

int spring_unit_def(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host == nullptr || host->sim == nullptr) {
        return 0;
    }
    const int index = sim_index(*host->sim, unit);
    if (index < 0) {
        return 0;
    }
    const auto at = host->def_ids.find(host->sim->def_name[static_cast<size_t>(index)]);
    if (at == host->def_ids.end()) {
        return 0;
    }
    lua_pushinteger(state, at->second);
    return 1;
}

int spring_team_units(lua_State* state) {
    const Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    lua_newtable(state);
    if (host == nullptr || host->sim == nullptr) {
        return 1;
    }
    int count = 0;
    const UnitSim& sim = *host->sim;
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.team[i] != team || !unit_is_alive(sim, i)) {
            continue;
        }
        lua_pushinteger(state, sim.id[i]);
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int spring_units_in_cylinder(lua_State* state) {
    const Host* host = host_from(state);
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float z = static_cast<float>(luaL_checknumber(state, 2));
    const float radius = static_cast<float>(luaL_checknumber(state, 3));
    const float reach = radius * radius;
    lua_newtable(state);
    if (host == nullptr || host->sim == nullptr) {
        return 1;
    }
    int count = 0;
    const UnitSim& sim = *host->sim;
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (!unit_is_alive(sim, i)) {
            continue;
        }
        const float dx = sim.x[i] - x;
        const float dz = sim.z[i] - z;
        if (dx * dx + dz * dz > reach) {
            continue;
        }
        lua_pushinteger(state, sim.id[i]);
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int spring_nearest_enemy(lua_State* state) {
    (void)state;
    return 0;
}

int spring_command_count(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    int count = 0;
    if (host != nullptr) {
        for (const LiveOrder& order : host->live) {
            if (order.unit == unit) {
                ++count;
            }
        }
    }
    lua_pushinteger(state, count);
    return 1;
}

int spring_get_unit_team(lua_State* state) {
    const Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (host == nullptr || host->sim == nullptr) {
        return 0;
    }
    const int index = sim_index(*host->sim, unit);
    if (index < 0) {
        return 0;
    }
    lua_pushinteger(state, host->sim->team[static_cast<size_t>(index)]);
    return 1;
}

int spring_units_in_rectangle(lua_State* state) {
    const Host* host = host_from(state);
    const float x1 = static_cast<float>(luaL_checknumber(state, 1));
    const float z1 = static_cast<float>(luaL_checknumber(state, 2));
    const float x2 = static_cast<float>(luaL_checknumber(state, 3));
    const float z2 = static_cast<float>(luaL_checknumber(state, 4));
    const float min_x = std::min(x1, x2);
    const float max_x = std::max(x1, x2);
    const float min_z = std::min(z1, z2);
    const float max_z = std::max(z1, z2);
    lua_newtable(state);
    if (host == nullptr || host->sim == nullptr) {
        return 1;
    }
    int count = 0;
    const UnitSim& sim = *host->sim;
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (!unit_is_alive(sim, i)) {
            continue;
        }
        if (sim.x[i] < min_x || sim.x[i] > max_x || sim.z[i] < min_z || sim.z[i] > max_z) {
            continue;
        }
        lua_pushinteger(state, sim.id[i]);
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int spring_test_build_order(lua_State* state) {
    const Host* host = host_from(state);
    (void)luaL_checknumber(state, 1);
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    (void)luaL_checknumber(state, 3);
    const float z = static_cast<float>(luaL_checknumber(state, 4));
    const float width = host != nullptr && host->terrain != nullptr ? host->terrain->world_x : 0.f;
    const float height = host != nullptr && host->terrain != nullptr ? host->terrain->world_z : 0.f;
    // 2 means the yard can be placed. 0 means the point is off the map. Occupancy is a separate query.
    const bool inside = x >= 0.f && z >= 0.f && x <= width && z <= height;
    lua_pushinteger(state, inside ? 2 : 0);
    return 1;
}

int spring_full_build_queue(lua_State* state) {
    (void)luaL_checknumber(state, 1);
    // Called when the live queue is already empty. An empty table lets the factory order one unit.
    lua_newtable(state);
    return 1;
}

int spring_team_resources(lua_State* state) {
    Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const char* kind = luaL_checkstring(state, 2);
    float current = 0;
    float storage = 0;
    float income = 0;
    TeamResources* row = host == nullptr ? nullptr : resources_for(*host, team);
    if (row != nullptr) {
        const std::string key = kind;
        if (key == "m" || key == "metal") {
            current = row->metal;
            storage = row->metal_storage;
            income = row->metal_income;
        } else if (key == "e" || key == "energy") {
            current = row->energy;
            storage = row->energy_storage;
            income = row->energy_income;
        }
    }
    lua_pushnumber(state, current);
    lua_pushnumber(state, storage);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, income);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 1);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    return 8;
}

int spring_give_order(lua_State* state) {
    Host* host = host_from(state);
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    const int cmd = static_cast<int>(luaL_checknumber(state, 2));
    if (host == nullptr) {
        return 0;
    }
    AiOrder order;
    order.unit = unit;
    order.cmd = cmd;
    if (cmd < 0) {
        const auto at = host->def_names.find(-cmd);
        if (at != host->def_names.end()) {
            order.def_name = at->second;
        }
    }
    if (lua_istable(state, 3)) {
        lua_rawgeti(state, 3, 1);
        if (lua_isnumber(state, -1)) {
            order.x = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        lua_rawgeti(state, 3, 2);
        if (lua_isnumber(state, -1)) {
            order.y = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        lua_rawgeti(state, 3, 3);
        if (lua_isnumber(state, -1)) {
            order.z = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
    }
    host->ai_orders.push_back(order);
    std::cout << "simpleai order " << (order.def_name.empty() ? std::to_string(cmd) : order.def_name) << " unit "
              << unit << " at " << order.x << " " << order.y << " " << order.z << "\n";
    if (state_command(cmd)) {
        return 0;
    }
    if (!order_shift(state)) {
        host->live.erase(std::remove_if(host->live.begin(), host->live.end(),
                                         [&](const LiveOrder& live) { return live.unit == unit; }),
                         host->live.end());
    }
    LiveOrder live;
    live.unit = unit;
    live.cmd = cmd;
    live.def_name = order.def_name;
    live.x = order.x;
    live.y = order.y;
    live.z = order.z;
    host->live.push_back(live);
    return 0;
}

int spring_set_resource(lua_State* state) {
    Host* host = host_from(state);
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const char* kind = luaL_checkstring(state, 2);
    const float value = static_cast<float>(luaL_checknumber(state, 3));
    if (host == nullptr) {
        return 0;
    }
    TeamResources* row = resources_for(*host, team);
    if (row == nullptr) {
        return 0;
    }
    const bool report = team == front_team(*host);
    const std::string key = kind;
    if (key == "m" || key == "metal") {
        row->metal = value;
        host->resource_mask[team] |= 1;
        if (report) {
            host->metal = value;
        }
    } else if (key == "e" || key == "energy") {
        row->energy = value;
        host->resource_mask[team] |= 2;
        if (report) {
            host->energy = value;
        }
    } else if (key == "ms") {
        row->metal_storage = value;
        host->resource_mask[team] |= 4;
        if (report) {
            host->metal_store = value;
        }
    } else if (key == "es") {
        row->energy_storage = value;
        host->resource_mask[team] |= 8;
        if (report) {
            host->energy_store = value;
        }
    }
    return 0;
}

bool bundle_was_tried(const Host& host, const std::string& path) {
    for (const std::string& tried : host.bundle_tried) {
        if (tried == path) {
            return true;
        }
    }
    return false;
}

void harvest_bundle_file(lua_State* state, Host& host, const std::filesystem::path& path) {
    const std::string text = path.string();
    if (bundle_was_tried(host, text)) {
        return;
    }
    host.bundle_tried.push_back(text);
    if (luaL_loadfile(state, text.c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        lua_pop(state, 1);
        return;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING && lua_istable(state, -1)) {
            const char* key = lua_tostring(state, -2);
            lua_getfield(state, -1, "health");
            if (key != nullptr && lua_isnumber(state, -1)) {
                host.bundle_health[key] = static_cast<float>(lua_tonumber(state, -1));
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

bool bundled_health(lua_State* state, Host& host, const std::string& name, float& health) {
    const auto cached = host.bundle_health.find(name);
    if (cached != host.bundle_health.end()) {
        health = cached->second;
        return true;
    }
    const std::filesystem::path root = std::filesystem::path(host.game_root) / "units";
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->path().extension() != ".lua") {
            continue;
        }
        const std::string stem = it->path().stem().string();
        const bool exact = stem == name;
        const bool prefix = !stem.empty() && name.size() > stem.size() + 1 &&
                            name.compare(0, stem.size(), stem) == 0 && name[stem.size()] == '_';
        if (!exact && !prefix) {
            continue;
        }
        harvest_bundle_file(state, host, it->path());
        const auto found = host.bundle_health.find(name);
        if (found != host.bundle_health.end()) {
            health = found->second;
            return true;
        }
    }
    // Some defs are built in a loop, so the file never contains the full name.
    std::string needle = name;
    while (!needle.empty() && std::isdigit(static_cast<unsigned char>(needle.back())) != 0) {
        needle.pop_back();
    }
    if (needle.size() < 4) {
        needle = name;
    }
    ec.clear();
    it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied,
                                                       ec);
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->path().extension() != ".lua") {
            continue;
        }
        if (bundle_was_tried(host, it->path().string())) {
            continue;
        }
        std::ifstream in(it->path());
        if (!in) {
            continue;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find(name) == std::string::npos && (needle == name || text.find(needle) == std::string::npos)) {
            continue;
        }
        harvest_bundle_file(state, host, it->path());
        const auto found = host.bundle_health.find(name);
        if (found != host.bundle_health.end()) {
            health = found->second;
            return true;
        }
    }
    return false;
}

bool unit_health_for(lua_State* state, Host& host, const std::string& name, float& health) {
    UnitWreck wreck;
    std::string error;
    if (load_unit_wreck(host.game_root, name, wreck, error) && wreck.has_health) {
        health = wreck.health;
        return true;
    }
    const bool scav = name.size() > 5 && name.compare(name.size() - 5, 5, "_scav") == 0;
    const std::string base = scav ? name.substr(0, name.size() - 5) : name;
    if (scav && load_unit_wreck(host.game_root, base, wreck, error) && wreck.has_health) {
        health = wreck.health;
        if (base.find("scavbeacon") == std::string::npos) {
            health *= 1.25f;
        }
        return true;
    }
    if (!bundled_health(state, host, base, health)) {
        std::cerr << "unitdef miss " << name << " " << error << "\n";
        return false;
    }
    if (scav && base.find("scavbeacon") == std::string::npos) {
        health *= 1.25f;
    }
    return true;
}

int spring_ensure_unit_def(lua_State* state) {
    Host* host = host_from(state);
    const char* raw = luaL_checkstring(state, 1);
    if (host == nullptr || raw == nullptr) {
        return 0;
    }
    const std::string name = raw;
    const auto existing = host->def_ids.find(name);
    if (existing != host->def_ids.end()) {
        lua_getglobal(state, "UnitDefNames");
        lua_pushlstring(state, name.data(), name.size());
        lua_rawget(state, -2);
        lua_remove(state, -2);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            return 0;
        }
        return 1;
    }
    float health = 0.f;
    if (!unit_health_for(state, *host, name, health)) {
        return 0;
    }
    const int id = host->next_def++;
    host->def_names[id] = name;
    host->def_ids[name] = id;
    lua_getglobal(state, "UnitDefNames");
    lua_newtable(state);
    lua_pushinteger(state, id);
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, name.data(), name.size());
    lua_setfield(state, -2, "name");
    lua_pushnumber(state, health);
    lua_setfield(state, -2, "health");
    lua_newtable(state);
    lua_setfield(state, -2, "customParams");
    lua_newtable(state);
    lua_setfield(state, -2, "modCategories");
    lua_pushvalue(state, -1);
    lua_pushlstring(state, name.data(), name.size());
    lua_insert(state, -2);
    lua_rawset(state, -4);
    lua_getglobal(state, "UnitDefs");
    lua_pushvalue(state, -2);
    lua_rawseti(state, -2, id);
    lua_pop(state, 1);
    lua_remove(state, -2);
    return 1;
}

void set_function(lua_State* state, const char* name, lua_CFunction function) {
    lua_pushcfunction(state, function);
    lua_setfield(state, -2, name);
}

bool call_method(lua_State* state, const char* key, const char* method, std::string& error) {
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    if (!lua_istable(state, -1)) {
        error = std::string(key) + " is not loaded";
        return false;
    }
    lua_getfield(state, -1, method);
    lua_pushvalue(state, -2);
    if (lua_pcall(state, 1, 0, 0) != 0) {
        error = std::string(method) + ": " + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "lua error");
        return false;
    }
    lua_pop(state, 1);
    return true;
}

bool run_prelude(lua_State* state, std::string& error) {
    const char* source = R"LUA(
math.clamp = function(value, low, high)
  if value < low then return low end
  if value > high then return high end
  return value
end
table.getKeyOf = function(tbl, value)
  for key, item in pairs(tbl) do
    if item == value then return key end
  end
end
table.contains = function(tbl, value)
  return table.getKeyOf(tbl, value) ~= nil
end
table.filterArray = function(tbl, callback)
  local result = {}
  for index, value in ipairs(tbl) do
    if callback(value, index, tbl) then
      result[#result + 1] = value
    end
  end
  return result
end
if not table.copy then
  function table.copy(tbl)
    local copy = {}
    for key, value in pairs(tbl) do
      if type(value) == "table" then
        copy[key] = table.copy(value)
      else
        copy[key] = value
      end
    end
    return copy
  end
end
if not table.append then
  function table.append(appendTarget, appendData)
    local n = #appendTarget
    for i = 1, #appendData do
      n = n + 1
      appendTarget[n] = appendData[i]
    end
  end
end
LOG = { INFO = "info", ERROR = "error", WARNING = "warning" }
GG = {}
-- No scum patch is recorded, so the stock bin test returns nil. Tree features are not simulated.
function GG.IsPosInRaptorScum()
  return nil
end
function GG.ComSpawnDefoliate() end
Game = Game or {}
Game.gameSpeed = 30
Game.maxUnits = 32000
Game.spawnInitialFrame = 2 * Game.gameSpeed
Game.spawnWarpInFrame = 3 * Game.gameSpeed
CMD = {
  STOP = 0,
  MOVE = 10,
  PATROL = 15,
  FIGHT = 16,
  ATTACK = 20,
  GUARD = 25,
  REPAIR = 40,
  FIRE_STATE = 45,
  MOVE_STATE = 50,
  RECLAIM = 90,
  DGUN = 105,
}
gadgetHandler = {
  IsSyncedCode = function() return true end,
  RemoveGadget = function() end,
  AddSyncAction = function() end,
  RemoveSyncAction = function() end,
  AddChatAction = function() end,
  RemoveChatAction = function() end,
}
function SendToUnsynced() end
include = VFS.Include
Spring.MoveCtrl = { Enable = Spring._moveEnable, Disable = function() end }
local sides = VFS.Include("gamedata/sidedata.lua")
function Spring.GetSideData(name)
  if type(name) ~= "string" then return nil end
  local want = string.lower(name)
  for _, side in ipairs(sides) do
    if string.lower(side.name) == want then
      return side.startunit, side.name
    end
  end
end
BAR = BAR or {}
local team = VFS.Include("common/springUtilities/teamFunctions.lua")
BAR.Utilities = team
Json = Json or VFS.Include("common/luaUtilities/json.lua")
setmetatable(UnitDefNames, {
  __index = function(_, key)
    if type(key) ~= "string" then return nil end
    return Spring._ensureUnitDef(key)
  end
})
)LUA";
    if (luaL_loadstring(state, source) != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "gadget prelude failed";
        return false;
    }
    return true;
}

bool push_def(lua_State* state, int id, const char* name, float metal_store, float energy_store) {
    lua_getglobal(state, "UnitDefs");
    lua_getglobal(state, "UnitDefNames");
    lua_newtable(state);
    lua_pushinteger(state, id);
    lua_setfield(state, -2, "id");
    lua_pushstring(state, name);
    lua_setfield(state, -2, "name");
    lua_pushnumber(state, metal_store);
    lua_setfield(state, -2, "metalStorage");
    lua_pushnumber(state, energy_store);
    lua_setfield(state, -2, "energyStorage");
    lua_newtable(state);
    lua_setfield(state, -2, "customParams");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "isBuilding");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "isFactory");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "isBuilder");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "canMove");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "transportCapacity");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "extractsMetal");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "energyMake");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "windGenerator");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "tidalGenerator");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "xsize");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "zsize");
    lua_newtable(state);
    lua_setfield(state, -2, "modCategories");
    lua_newtable(state);
    lua_setfield(state, -2, "weapons");
    lua_newtable(state);
    lua_setfield(state, -2, "buildOptions");
    lua_pushvalue(state, -1);
    lua_rawseti(state, -4, id);
    lua_setfield(state, -2, name);
    lua_pop(state, 2);
    return true;
}

bool load_one(lua_State* state, const std::string& path, const char* key, std::string& error) {
    lua_newtable(state);
    lua_setglobal(state, "gadget");
    if (luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "gadget load failed";
        return false;
    }
    lua_getglobal(state, "gadget");
    if (!lua_istable(state, -1)) {
        error = path + " did not leave a gadget table";
        return false;
    }
    lua_setfield(state, LUA_REGISTRYINDEX, key);
    return call_method(state, key, "Initialize", error);
}

struct BehaviorGadget {
    std::string name;
    int ref = LUA_NOREF;
    int layer = 0;
};

struct Session {
    lua_State* state = nullptr;
    std::unique_ptr<Host> host;
    std::vector<BehaviorGadget> behaviors;
    int calls = 0;
    std::vector<std::string> rewrites;
    std::string call_error;
};

Session* g_session = nullptr;
std::vector<BehaviorGadget> g_loaded_behaviors;

bool install_weapon_tables(lua_State* state, std::string& error) {
    const char* source = R"LUA(
Script = { SetWatchProjectile = function() end, SetWatchWeapon = function() end }
local function weapon(id, name, kind)
  return {
    id = id,
    name = name,
    type = kind,
    customParams = {},
    damages = {},
    range = 0,
    projectilespeed = 1,
    cylinderTargeting = 0,
  }
end
WeaponDefs = {
  [0] = weapon(0, "none", "none"),
  [1] = weapon(1, "commanderexplosion", "Cannon"),
  [2] = weapon(2, "mine_light", "Cannon"),
  [3] = weapon(3, "mine_medium", "Cannon"),
  [4] = weapon(4, "mine_heavy", "Cannon"),
}
WeaponDefNames = {
  commanderexplosion = WeaponDefs[1],
  mine_light = WeaponDefs[2],
  mine_medium = WeaponDefs[3],
  mine_heavy = WeaponDefs[4],
}
)LUA";
    if (luaL_loadstring(state, source) != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "weapon tables failed";
        return false;
    }
    return true;
}

bool adopt_behavior(lua_State* state, const std::string& path, std::string& error) {
    lua_newtable(state);
    lua_setglobal(state, "gadget");
    if (luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        error = path + ": " + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "behavior load failed");
        return false;
    }
    lua_getglobal(state, "gadget");
    if (!lua_istable(state, -1)) {
        error = path + " did not leave a gadget table";
        return false;
    }
    lua_getfield(state, -1, "GetInfo");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        return true;
    }
    lua_pushvalue(state, -2);
    if (lua_pcall(state, 1, 1, 0) != 0) {
        error = path + ": " + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "GetInfo failed");
        return false;
    }
    std::string name = path;
    int layer = 0;
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "name");
        if (lua_isstring(state, -1)) {
            name = lua_tostring(state, -1);
        }
        lua_pop(state, 1);
        lua_getfield(state, -1, "layer");
        if (lua_isnumber(state, -1)) {
            layer = static_cast<int>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, "UnitPreDamaged");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        return true;
    }
    lua_pop(state, 1);
    BehaviorGadget gadget;
    gadget.name = name;
    gadget.layer = layer;
    gadget.ref = luaL_ref(state, LUA_REGISTRYINDEX);
    g_loaded_behaviors.push_back(gadget);
    return true;
}

bool load_behavior_gadgets(lua_State* state, std::string& error) {
    g_loaded_behaviors.clear();
    if (!install_weapon_tables(state, error)) {
        return false;
    }
    const char* files[] = {
        "luarules/gadgets/unit_paralyze_damage_multiplier.lua",
        "luarules/gadgets/unit_no_land_damage.lua",
        "luarules/gadgets/unit_combomb_full_damage.lua",
        "luarules/gadgets/unit_stomp.lua",
        "luarules/gadgets/unit_dgun_behaviour.lua",
        "luarules/gadgets/unit_paralyze_damage_limit.lua",
    };
    Host* host = host_from(state);
    if (host == nullptr) {
        error = "behavior host is missing";
        return false;
    }
    for (const char* file : files) {
        if (!adopt_behavior(state, host->game_root + "/" + file, error)) {
            return false;
        }
    }
    if (g_loaded_behaviors.size() != 6) {
        error = "unit behavior gadgets did not all expose UnitPreDamaged";
        return false;
    }
    return true;
}

void add_build_options(Host& host, const std::vector<std::string>& roots, int& next_def,
                       std::vector<std::string>* added) {
    for (const std::string& name : roots) {
        UnitBuilder builder;
        std::string builder_error;
        if (!load_unit_builder(host.game_root, name, builder, builder_error)) {
            continue;
        }
        for (const std::string& option : builder.options) {
            if (option.empty() || host.def_ids.find(option) != host.def_ids.end()) {
                continue;
            }
            host.def_names[next_def] = option;
            host.def_ids[option] = next_def;
            ++next_def;
            if (added != nullptr) {
                added->push_back(option);
            }
        }
    }
}

void expand_build_options(Host& host) {
    std::vector<std::string> roots;
    roots.reserve(host.def_names.size());
    for (const auto& entry : host.def_names) {
        roots.push_back(entry.second);
    }
    int next_def = 1;
    for (const auto& entry : host.def_names) {
        if (entry.first >= next_def) {
            next_def = entry.first + 1;
        }
    }
    std::vector<std::string> added;
    add_build_options(host, roots, next_def, &added);
    // A lab's products are one level past the commander. Constructors and mobile units
    // have to exist or the post-mex RandomChoice reads an empty table.
    std::vector<std::string> factories;
    for (const std::string& name : added) {
        UnitRole role;
        std::string role_error;
        if (!load_unit_role(host.game_root, name, role, role_error) || !role.factory) {
            continue;
        }
        factories.push_back(name);
    }
    add_build_options(host, factories, next_def, nullptr);
}

void set_bool_field(lua_State* state, const char* key, bool value) {
    lua_pushboolean(state, value ? 1 : 0);
    lua_setfield(state, -2, key);
}

void set_num_field(lua_State* state, const char* key, double value) {
    lua_pushnumber(state, value);
    lua_setfield(state, -2, key);
}

void annotate_roles(lua_State* state, Host& host) {
    for (const auto& entry : host.def_names) {
        UnitRole role;
        std::string role_error;
        if (!load_unit_role(host.game_root, entry.second, role, role_error) || !role.ok) {
            continue;
        }
        lua_getglobal(state, "UnitDefs");
        lua_rawgeti(state, -1, entry.first);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            continue;
        }
        set_bool_field(state, "isBuilder", role.builder);
        set_bool_field(state, "canMove", role.can_move);
        set_bool_field(state, "isBuilding", role.building);
        set_num_field(state, "extractsMetal", role.extracts_metal);
        set_num_field(state, "energyMake", role.energy_make);
        // A missing energyupkeep stays nil. Lua treats 0 as true, so a stored 0 would
        // change `not energyUpkeep` in the generator test.
        if (role.has_upkeep) {
            set_num_field(state, "energyUpkeep", role.energy_upkeep);
        }
        set_num_field(state, "windGenerator", role.wind_generator);
        set_num_field(state, "tidalGenerator", role.tidal_generator);
        set_num_field(state, "transportCapacity", role.transport_capacity);
        set_num_field(state, "xsize", static_cast<double>(role.footprint_x));
        set_num_field(state, "zsize", static_cast<double>(role.footprint_z));
        lua_getfield(state, -1, "customParams");
        if (lua_istable(state, -1)) {
            if (role.commander) {
                set_bool_field(state, "iscommander", true);
            }
            if (role.metal_extractor) {
                set_bool_field(state, "metal_extractor", true);
            }
            if (role.solar) {
                set_bool_field(state, "solar", true);
            }
            if (role.converter) {
                set_bool_field(state, "energyconv_capacity", true);
                set_bool_field(state, "energyconv_efficiency", true);
            }
        }
        lua_pop(state, 1);
        lua_newtable(state);
        int count = 0;
        for (const std::string& option : role.options) {
            const auto at = host.def_ids.find(option);
            if (at == host.def_ids.end()) {
                continue;
            }
            lua_pushinteger(state, at->second);
            lua_rawseti(state, -2, ++count);
        }
        lua_setfield(state, -2, "buildOptions");
        set_bool_field(state, "isFactory", role.factory);
        if (role.has_weapon) {
            lua_newtable(state);
            lua_newtable(state);
            lua_pushinteger(state, 0);
            lua_setfield(state, -2, "weaponDef");
            lua_rawseti(state, -2, 1);
            lua_setfield(state, -2, "weapons");
        }
        lua_pop(state, 2);
    }
}

void install_spots(lua_State* state, const TerrainMesh& terrain) {
    lua_getglobal(state, "GG");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setglobal(state, "GG");
    }
    lua_newtable(state);
    lua_newtable(state);
    int count = 0;
    for (const TerrainMesh::MetalSpot& spot : terrain.metal_spots) {
        lua_newtable(state);
        lua_pushnumber(state, spot.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, terrain_height_at(terrain, spot.x, spot.z));
        lua_setfield(state, -2, "y");
        lua_pushnumber(state, spot.z);
        lua_setfield(state, -2, "z");
        lua_rawseti(state, -2, ++count);
    }
    lua_setfield(state, -2, "metalSpotsList");
    lua_setfield(state, -2, "resource_spot_finder");
    lua_pop(state, 1);
}

bool load_simple_ai(lua_State* state, Host& host, std::string& error) {
    const std::string path = host.game_root + "/luarules/gadgets/ai_simpleai.lua";
    lua_newtable(state);
    lua_setglobal(state, "gadget");
    if (luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        error = std::string("simpleai: ") + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "load failed");
        return false;
    }
    lua_getglobal(state, "gadget");
    if (!lua_istable(state, -1)) {
        error = "simpleai did not leave a gadget table";
        return false;
    }
    lua_getfield(state, -1, "GetInfo");
    if (!lua_isfunction(state, -1)) {
        error = "simpleai has no GetInfo";
        return false;
    }
    lua_pushvalue(state, -2);
    if (lua_pcall(state, 1, 1, 0) != 0) {
        error = std::string("simpleai: ") + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "GetInfo failed");
        return false;
    }
    std::string name = "SimpleAI";
    bool enabled = false;
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "name");
        if (lua_isstring(state, -1)) {
            name = lua_tostring(state, -1);
        }
        lua_pop(state, 1);
        lua_getfield(state, -1, "enabled");
        enabled = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "bar_simpleai");
    host.ai_name = name;
    host.ai_enabled = enabled;
    host.ai_loaded = true;
    std::cout << "simpleai " << name << " enabled " << (enabled ? 1 : 0) << "\n";
    return true;
}

void keep_gadget_session(lua_State* state, std::unique_ptr<Host> host) {
    if (g_session != nullptr) {
        lua_close(g_session->state);
        delete g_session;
        g_session = nullptr;
    }
    auto* session = new Session;
    session->state = state;
    session->host = std::move(host);
    session->behaviors = std::move(g_loaded_behaviors);
    g_session = session;
}

}  // namespace

void gadget_session_close() {
    if (g_session == nullptr) {
        return;
    }
    lua_close(g_session->state);
    delete g_session;
    g_session = nullptr;
}

float behavior_adjust_damage(const UnitSim& sim, int attacker, int target, float damage) {
    if (g_session == nullptr || g_session->state == nullptr || target < 0 ||
        target >= static_cast<int>(sim.id.size())) {
        return damage;
    }
    Session& session = *g_session;
    ++session.calls;
    const Host& host = *session.host;
    const auto def_of = [&](int index) {
        if (index < 0 || index >= static_cast<int>(sim.def_name.size())) {
            return 0;
        }
        const auto at = host.def_ids.find(sim.def_name[static_cast<size_t>(index)]);
        return at == host.def_ids.end() ? 0 : at->second;
    };
    const int target_id = sim.id[static_cast<size_t>(target)];
    const int target_def = def_of(target);
    const int target_team = sim.team[static_cast<size_t>(target)];
    const bool have_attacker = attacker >= 0 && attacker < static_cast<int>(sim.id.size());
    const int attacker_id = have_attacker ? sim.id[static_cast<size_t>(attacker)] : 0;
    const int attacker_def = def_of(attacker);
    const int attacker_team = have_attacker ? sim.team[static_cast<size_t>(attacker)] : 0;
    lua_State* state = session.state;
    for (const BehaviorGadget& gadget : session.behaviors) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, gadget.ref);
        lua_getfield(state, -1, "UnitPreDamaged");
        lua_pushvalue(state, -2);
        lua_pushinteger(state, target_id);
        lua_pushinteger(state, target_def);
        lua_pushinteger(state, target_team);
        lua_pushnumber(state, damage);
        lua_pushboolean(state, 0);
        lua_pushinteger(state, 0);
        lua_pushinteger(state, -1);
        lua_pushinteger(state, attacker_id);
        lua_pushinteger(state, attacker_def);
        lua_pushinteger(state, attacker_team);
        if (lua_pcall(state, 11, 2, 0) != 0) {
            const char* message = lua_tostring(state, -1);
            session.call_error = gadget.name + ": " + (message != nullptr ? message : "lua error");
            std::cerr << "behavior: " << session.call_error << "\n";
            if (session.rewrites.size() < 8) {
                session.rewrites.push_back(gadget.name);
            }
            lua_pop(state, 2);
            continue;
        }
        float dealt = damage;
        if (lua_isnumber(state, -2)) {
            dealt = static_cast<float>(lua_tonumber(state, -2));
        }
        lua_pop(state, 3);
        if (std::fabs(dealt - damage) > 0.0001f && session.rewrites.size() < 8) {
            session.rewrites.push_back(gadget.name);
        }
    }
    return damage;
}

bool write_behavior_report(const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write behavior report " + path;
        return false;
    }
    const Session* session = g_session;
    out << "{\"calls\":" << (session != nullptr ? session->calls : 0) << ",\"names\":[";
    if (session != nullptr) {
        for (size_t i = 0; i < session->behaviors.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << '"' << session->behaviors[i].name << '"';
        }
    }
    out << "],\"rewrites\":[";
    if (session != nullptr) {
        for (size_t i = 0; i < session->rewrites.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << '"' << session->rewrites[i] << '"';
        }
    }
    out << "],\"deferred\":\"dgun-projectile\"}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool simpleai_game_frame(int frame, std::string& error) {
    if (g_session == nullptr || g_session->state == nullptr || g_session->host == nullptr ||
        !g_session->host->ai_loaded) {
        return true;
    }
    lua_State* state = g_session->state;
    lua_getfield(state, LUA_REGISTRYINDEX, "bar_simpleai");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    lua_getfield(state, -1, "GameFrame");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        return true;
    }
    lua_pushvalue(state, -2);
    lua_pushinteger(state, frame);
    if (lua_pcall(state, 2, 0, 0) != 0) {
        const char* message = lua_tostring(state, -1);
        error = message != nullptr ? message : "lua error";
        std::cerr << "simpleai: " << error << "\n";
        lua_pop(state, 2);
        return false;
    }
    lua_pop(state, 1);
    return true;
}

bool mode_game_frame(int frame, std::string& error) {
    if (g_session == nullptr || g_session->state == nullptr || g_session->host == nullptr) {
        return true;
    }
    lua_State* state = g_session->state;
    for (const std::string& key : g_session->host->mode_keys) {
        lua_getfield(state, LUA_REGISTRYINDEX, key.c_str());
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        lua_getfield(state, -1, "GameFrame");
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 2);
            continue;
        }
        lua_pushvalue(state, -2);
        lua_pushinteger(state, frame);
        if (lua_pcall(state, 2, 0, 0) != 0) {
            const char* message = lua_tostring(state, -1);
            error = message != nullptr ? message : "lua error";
            std::cerr << key << ": " << error << "\n";
            lua_pop(state, 2);
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

float snap_build(float click, int footprint) {
    const int size = std::max(footprint, 1) * 2;
    if ((size & 2) != 0) {
        return std::floor(click / 16.f) * 16.f + 8.f;
    }
    return std::floor((click + 8.f) / 16.f) * 16.f;
}

void stop_moving(UnitSim& sim, size_t index) {
    sim.has_goal[index] = 0;
    sim.speed[index] = 0;
    sim.path_x[index].clear();
    sim.path_z[index].clear();
    sim.path_at[index] = 0;
}

bool seen_unit(const std::vector<int>& seen, int unit) {
    for (int id : seen) {
        if (id == unit) {
            return true;
        }
    }
    return false;
}

bool notify_unit(const char* method, int unit_id, int def_id, int team, std::string& error) {
    if (g_session == nullptr || g_session->state == nullptr) {
        return true;
    }
    lua_State* state = g_session->state;
    lua_getfield(state, LUA_REGISTRYINDEX, "bar_simpleai");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    lua_getfield(state, -1, method);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        return true;
    }
    lua_pushvalue(state, -2);
    lua_pushinteger(state, unit_id);
    lua_pushinteger(state, def_id);
    lua_pushinteger(state, team);
    if (lua_pcall(state, 4, 0, 0) != 0) {
        const char* message = lua_tostring(state, -1);
        error = message != nullptr ? message : "lua error";
        std::cerr << "simpleai: " << error << "\n";
        lua_pop(state, 2);
        return false;
    }
    lua_pop(state, 1);
    return true;
}

bool prepare_order(LiveOrder& order, const AiEconomy& eco, const std::string& builder_def, std::string& error) {
    if (order.known) {
        return true;
    }
    UnitBuilder maker;
    if (!load_unit_builder(eco.game_root, builder_def, maker, error)) {
        return false;
    }
    UnitRole role;
    std::string role_error;
    if (load_unit_role(eco.game_root, builder_def, role, role_error)) {
        order.factory = role.factory;
    }
    const float scale = eco.construction != nullptr ? eco.construction->build_distance_scale : 1.f;
    order.reach = std::max(38.f, maker.build_distance * scale);
    UnitBuild build;
    if (!load_unit_build(eco.game_root, order.def_name, build, error)) {
        return false;
    }
    order.site_x = snap_build(order.x, build.footprint_x);
    order.site_z = snap_build(order.z, build.footprint_z);
    order.site_y = eco.terrain != nullptr ? terrain_height_at(*eco.terrain, order.site_x, order.site_z) : order.y;
    order.known = true;
    return true;
}

void credit_building(const AiEconomy& eco, TeamResources& row, const std::string& def_name, float x, float z) {
    std::string ignored;
    UnitExtractor extractor;
    if (eco.claims != nullptr && eco.terrain != nullptr && eco.mex != nullptr &&
        load_unit_extractor(eco.game_root, def_name, extractor, ignored) && extractor.active && extractor.depth > 0.f) {
        float depth = extractor.depth;
        if (extractor.metal_extractor > 0.f) {
            depth *= eco.metal_scale;
        }
        const MexTrace claim = metal_claim(*eco.claims, *eco.terrain, def_name, x, z, depth);
        eco.mex->push_back(claim);
        row.mex_income += claim.rate;
        row.energy_upkeep += extractor.energy_upkeep;
        std::cout << "simpleai mex " << def_name << " team " << row.team << " at " << claim.x << " " << claim.z
                  << " rate " << claim.rate << "\n";
    }
    UnitPower power;
    if (eco.generators == nullptr || !load_unit_power(eco.game_root, def_name, power, ignored) || !power.active) {
        return;
    }
    GenTrace gen;
    gen.def_name = def_name;
    gen.x = x;
    gen.z = z;
    gen.upkeep = power.energy_upkeep;
    gen.wind = eco.wind_strength;
    bool produces = false;
    if (power.energy_upkeep < 0.f) {
        gen.production = -power.energy_upkeep * eco.energy_scale;
        row.solar_income += gen.production;
        produces = true;
        std::cout << "simpleai solar " << def_name << " team " << row.team << " make " << gen.production << "\n";
    }
    if (power.wind_generator > 0.f) {
        gen.wind_cap = power.wind_generator * eco.energy_scale;
        gen.rate = capped_wind_energy(eco.wind_strength, gen.wind_cap);
        row.wind_income += gen.rate;
        produces = true;
        std::cout << "simpleai wind " << def_name << " team " << row.team << " rate " << gen.rate << "\n";
    }
    if (produces) {
        eco.generators->push_back(gen);
    }
}

bool add_built_unit(const AiEconomy& eco, const LiveOrder& order, int team, int& unit_id, int& def_id,
                    std::string& error) {
    UnitMotion motion;
    if (!load_unit_motion(eco.game_root, order.def_name, motion, error)) {
        return false;
    }
    float slope = 20.f;
    float water = 35.f;
    float min_water = -1.0e6f;
    bool hover = false;
    if (!motion.move_class.empty()) {
        MoveClass move_class;
        if (!load_move_class(eco.game_root, motion.move_class, move_class, error)) {
            return false;
        }
        slope = move_class.max_slope_deg;
        water = move_class.max_water_depth;
        min_water = move_class.min_water_depth;
        hover = move_class.hover;
    }
    int next_id = 1;
    for (int id : eco.sim->id) {
        if (id >= next_id) {
            next_id = id + 1;
        }
    }
    UnitSpawn spawn;
    spawn.id = next_id;
    spawn.team = team;
    spawn.def_name = order.def_name;
    spawn.x = order.nano.x;
    spawn.y = order.nano.y;
    spawn.z = order.nano.z;
    const float cruise = motion.can_fly ? motion.cruise_altitude : 0.f;
    if (!sim_add_unit(*eco.sim, spawn, motion, slope, water, min_water, order.nano.y, hover, cruise, error)) {
        return false;
    }
    unit_id = next_id;
    def_id = 0;
    if (g_session != nullptr && g_session->host != nullptr) {
        const auto at = g_session->host->def_ids.find(order.def_name);
        if (at != g_session->host->def_ids.end()) {
            def_id = at->second;
        }
    }
    return true;
}

bool simpleai_drive(const AiEconomy& eco, int frame, std::string& error) {
    if (g_session == nullptr || g_session->host == nullptr || eco.sim == nullptr || eco.teams == nullptr ||
        eco.construction == nullptr || eco.terrain == nullptr) {
        return true;
    }
    Host& host = *g_session->host;
    std::vector<LiveOrder>& live = host.live;
    auto row_for = [&](int team) -> TeamResources* {
        for (TeamResources& row : *eco.teams) {
            if (row.team == team) {
                return &row;
            }
        }
        return nullptr;
    };
    std::vector<int> seen;
    std::vector<int> finished;
    for (int i = 0; i < static_cast<int>(live.size()); ++i) {
        LiveOrder& order = live[static_cast<size_t>(i)];
        if (seen_unit(seen, order.unit)) {
            continue;
        }
        seen.push_back(order.unit);
        if (!order.nano.active) {
            continue;
        }
        const int builder = sim_index(*eco.sim, order.unit);
        if (builder < 0) {
            continue;
        }
        TeamResources* row = row_for(eco.sim->team[static_cast<size_t>(builder)]);
        if (row == nullptr) {
            continue;
        }
        const size_t at = static_cast<size_t>(builder);
        nano_step(order.nano, *row, eco.sim->x[at], eco.sim->y[at], eco.sim->z[at], frame);
        if (order.nano.finished) {
            finished.push_back(i);
        }
    }
    for (int n = static_cast<int>(finished.size()) - 1; n >= 0; --n) {
        const int index = finished[static_cast<size_t>(n)];
        LiveOrder order = live[static_cast<size_t>(index)];
        const int builder = sim_index(*eco.sim, order.unit);
        if (builder < 0) {
            live.erase(live.begin() + index);
            continue;
        }
        const int team = eco.sim->team[static_cast<size_t>(builder)];
        TeamResources* row = row_for(team);
        int unit_id = 0;
        int def_id = 0;
        if (!add_built_unit(eco, order, team, unit_id, def_id, error)) {
            return false;
        }
        if (row != nullptr) {
            credit_building(eco, *row, order.def_name, order.nano.x, order.nano.z);
        }
        stop_moving(*eco.sim, static_cast<size_t>(builder));
        live.erase(live.begin() + index);
        std::cout << "simpleai finish " << order.def_name << " id " << unit_id << " team " << team << "\n";
        if (def_id != 0 &&
            (!notify_unit("UnitCreated", unit_id, def_id, team, error) ||
             !notify_unit("UnitFinished", unit_id, def_id, team, error))) {
            return false;
        }
    }
    seen.clear();
    std::vector<int> drop;
    for (int i = 0; i < static_cast<int>(live.size()); ++i) {
        LiveOrder& order = live[static_cast<size_t>(i)];
        if (seen_unit(seen, order.unit)) {
            continue;
        }
        seen.push_back(order.unit);
        if (order.nano.active || order.failed) {
            continue;
        }
        const int builder = sim_index(*eco.sim, order.unit);
        if (builder < 0) {
            drop.push_back(i);
            continue;
        }
        const size_t at = static_cast<size_t>(builder);
        if (order.cmd >= 0) {
            if (order.cmd != 10 && order.cmd != 16) {
                std::cout << "simpleai skip " << order.cmd << " unit " << order.unit << "\n";
                drop.push_back(i);
                continue;
            }
            const float dx = eco.sim->x[at] - order.x;
            const float dz = eco.sim->z[at] - order.z;
            if (dx * dx + dz * dz <= 4.f) {
                stop_moving(*eco.sim, at);
                drop.push_back(i);
                continue;
            }
            if (eco.sim->has_goal[at] == 0 &&
                !sim_order_move(*eco.sim, builder, order.x, order.z, *eco.terrain, error)) {
                std::cout << "simpleai path " << error << "\n";
                error.clear();
                order.failed = true;
                drop.push_back(i);
            }
            continue;
        }
        if (order.def_name.empty()) {
            drop.push_back(i);
            continue;
        }
        if (!prepare_order(order, eco, eco.sim->def_name[at], error)) {
            return false;
        }
        if (order.factory) {
            if (!factory_begin(order.nano, eco.game_root, *eco.construction, builder, eco.sim->def_name[at],
                               eco.sim->x[at], eco.sim->y[at], eco.sim->z[at], order.def_name, error)) {
                std::cout << "simpleai build " << error << "\n";
                error.clear();
                drop.push_back(i);
            }
            continue;
        }
        const float dx = eco.sim->x[at] - order.site_x;
        const float dy = eco.sim->y[at] - order.site_y;
        const float dz = eco.sim->z[at] - order.site_z;
        if (dx * dx + dy * dy + dz * dz <= order.reach * order.reach) {
            if (!nano_begin(order.nano, eco.game_root, *eco.construction, builder, eco.sim->def_name[at],
                            eco.sim->x[at], eco.sim->y[at], eco.sim->z[at], order.def_name, order.x, order.z,
                            *eco.terrain, error)) {
                std::cout << "simpleai build " << error << "\n";
                error.clear();
                drop.push_back(i);
                continue;
            }
            stop_moving(*eco.sim, at);
            std::cout << "simpleai build " << order.def_name << " unit " << order.unit << " at " << order.nano.x << " "
                      << order.nano.z << "\n";
            continue;
        }
        if (eco.sim->has_goal[at] == 0 &&
            !sim_order_move(*eco.sim, builder, order.x, order.z, *eco.terrain, error)) {
            std::cout << "simpleai path " << error << "\n";
            error.clear();
            order.failed = true;
            drop.push_back(i);
        }
    }
    for (int n = static_cast<int>(drop.size()) - 1; n >= 0; --n) {
        live.erase(live.begin() + drop[static_cast<size_t>(n)]);
    }
    return true;
}

bool write_ai_report(const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write ai report " + path;
        return false;
    }
    const Session* session = g_session;
    const Host* host = session != nullptr ? session->host.get() : nullptr;
    const std::string name = host != nullptr ? host->ai_name : "";
    const bool enabled = host != nullptr && host->ai_enabled;
    out << "{\"name\":\"" << name << "\",\"enabled\":" << (enabled ? 1 : 0) << ",\"orders\":[";
    if (host != nullptr) {
        const size_t written = std::min(host->ai_orders.size(), static_cast<size_t>(64));
        for (size_t i = 0; i < written; ++i) {
            const AiOrder& order = host->ai_orders[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"unit\":" << order.unit << ",\"cmd\":" << order.cmd << ",\"def\":\"" << order.def_name
                << "\",\"x\":" << order.x << ",\"y\":" << order.y << ",\"z\":" << order.z << '}';
        }
    }
    out << "],\"spots\":[";
    if (host != nullptr && host->terrain != nullptr) {
        for (size_t i = 0; i < host->terrain->metal_spots.size(); ++i) {
            const TerrainMesh::MetalSpot& spot = host->terrain->metal_spots[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"x\":" << spot.x << ",\"z\":" << spot.z << '}';
        }
    }
    out << "]}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool pve_exempt(const std::string& lua_ai) {
    return lua_ai.find("Scavengers") != std::string::npos || lua_ai == "RaptorsAI";
}

bool backfill_unit_health(lua_State* state, Host& host) {
    lua_getglobal(state, "UnitDefs");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    const int count = static_cast<int>(lua_objlen(state, -1));
    for (int id = 1; id <= count; ++id) {
        lua_rawgeti(state, -1, id);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        lua_getfield(state, -1, "health");
        const bool missing = lua_isnumber(state, -1) == 0;
        lua_pop(state, 1);
        if (missing) {
            lua_getfield(state, -1, "name");
            const char* raw = lua_tostring(state, -1);
            const std::string name = raw != nullptr ? raw : "";
            lua_pop(state, 1);
            float health = 0.f;
            UnitWreck wreck;
            std::string wreck_error;
            if (!name.empty() && load_unit_wreck(host.game_root, name, wreck, wreck_error) && wreck.has_health) {
                health = wreck.health;
            }
            lua_pushnumber(state, health);
            lua_setfield(state, -2, "health");
        }
        lua_getfield(state, -1, "customParams");
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_setfield(state, -2, "customParams");
        } else {
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool load_mode_gadget(lua_State* state, Host& host, const std::string& path, const char* key, std::string& error) {
    lua_newtable(state);
    lua_setglobal(state, "gadget");
    if (luaL_loadfile(state, path.c_str()) != 0) {
        error = std::string(key) + ": " + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "load failed");
        return false;
    }
    if (lua_pcall(state, 0, 1, 0) != 0) {
        error = std::string(key) + ": " + (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "lua error");
        return false;
    }
    const bool disabled = lua_isboolean(state, -1) != 0 && lua_toboolean(state, -1) == 0;
    lua_pop(state, 1);
    if (disabled) {
        std::cout << key << " disabled\n";
        return true;
    }
    lua_getglobal(state, "gadget");
    if (!lua_istable(state, -1)) {
        error = std::string(key) + " did not leave a gadget table";
        return false;
    }
    lua_setfield(state, LUA_REGISTRYINDEX, key);
    host.mode_keys.emplace_back(key);
    std::cout << key << " enabled\n";
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    lua_getfield(state, -1, "GameStart");
    if (lua_isfunction(state, -1)) {
        lua_pushvalue(state, -2);
        if (lua_pcall(state, 1, 0, 0) != 0) {
            error = std::string(key) + " GameStart: " +
                    (lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "lua error");
            return false;
        }
        lua_pop(state, 1);
    } else {
        lua_pop(state, 2);
    }
    return true;
}

bool load_spawn_gadgets(const std::string& game_root, const StartScript& script, const ResourceRules& rules,
                        std::vector<TeamResources>& teams, UnitSim& sim, const TerrainMesh& terrain,
                        GadgetReport& report, std::string& error) {
    report = GadgetReport{};
    if (script.teams.empty()) {
        error = "start script has no team";
        return false;
    }
    auto host_owner = std::make_unique<Host>();
    Host& host = *host_owner;
    host.script = &script;
    host.rules = &rules;
    host.teams = &teams;
    host.sim = &sim;
    host.terrain = &terrain;
    host.game_root = game_root;

    struct NamedDef {
        const char* name;
        int id;
    };
    const NamedDef defs[] = {{"armcom", 1}, {"corcom", 2}, {"legcom", 3}, {"dummycom", 4}};
    for (const NamedDef& def : defs) {
        UnitYield yield;
        std::string yield_error;
        if (!load_unit_yield(game_root, def.name, yield, yield_error)) {
            if (std::string(def.name) != "dummycom") {
                error = yield_error;
                return false;
            }
            yield = UnitYield{};
        }
        host.def_names[def.id] = def.name;
        host.def_ids[def.name] = def.id;
        (void)yield;
    }
    int next_def = 5;
    for (const std::string& name : sim.def_name) {
        if (name.empty() || host.def_ids.find(name) != host.def_ids.end()) {
            continue;
        }
        host.def_names[next_def] = name;
        host.def_ids[name] = next_def;
        ++next_def;
    }
    expand_build_options(host);
    host.next_def = 1;
    for (const auto& entry : host.def_names) {
        if (entry.first >= host.next_def) {
            host.next_def = entry.first + 1;
        }
    }
    std::cout << "unitdefs " << host.def_names.size() << "\n";

    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    lua_pushlightuserdata(state, host_owner.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "bar_host");

    lua_newtable(state);
    lua_pushcfunction(state, vfs_include);
    lua_setfield(state, -2, "Include");
    lua_pushcfunction(state, vfs_zlib);
    lua_setfield(state, -2, "ZlibDecompress");
    lua_setglobal(state, "VFS");

    lua_newtable(state);
    set_function(state, "GetModOptions", spring_get_mod_options);
    set_function(state, "GetGameFrame", spring_get_game_frame);
    set_function(state, "GetGameSeconds", spring_get_game_seconds);
    set_function(state, "GetGaiaTeamID", spring_get_gaia);
    set_function(state, "GetTeamList", spring_get_team_list);
    set_function(state, "GetAllyTeamList", spring_get_ally_list);
    set_function(state, "GetPlayerList", spring_get_player_list);
    set_function(state, "GetPlayerInfo", spring_get_player_info);
    set_function(state, "GetTeamInfo", spring_get_team_info);
    set_function(state, "GetTeamLuaAI", spring_get_team_lua_ai);
    set_function(state, "GetTeamAllyTeamID", spring_get_team_ally);
    set_function(state, "GetTeamRulesParam", spring_get_team_rules);
    set_function(state, "SetTeamRulesParam", spring_set_team_rules);
    set_function(state, "GetGameRulesParam", spring_get_game_rules);
    set_function(state, "SetGameRulesParam", spring_set_game_rules);
    set_function(state, "GetTeamStartPosition", spring_get_start_pos);
    set_function(state, "SetTeamStartPosition", spring_noop);
    set_function(state, "GetGroundHeight", spring_get_ground_height);
    set_function(state, "IsPosInLos", spring_is_pos_in_los);
    set_function(state, "IsPosInAirLos", spring_is_pos_in_air_los);
    set_function(state, "IsPosInRadar", spring_is_pos_in_radar);
    set_function(state, "SetUnitExperience", spring_set_unit_experience);
    set_function(state, "SpawnCEG", spring_noop);
    set_function(state, "PlaySoundFile", spring_noop);
    set_function(state, "GetAllyTeamStartBox", spring_get_start_box);
    set_function(state, "CreateUnit", spring_create_unit);
    set_function(state, "DestroyUnit", spring_destroy_unit);
    set_function(state, "SetUnitNoDraw", spring_set_no_draw);
    set_function(state, "GetUnitHealth", spring_get_unit_health);
    set_function(state, "SetUnitHealth", spring_set_unit_health);
    set_function(state, "GetUnitBlocking", spring_get_blocking);
    set_function(state, "SetUnitBlocking", spring_noop);
    set_function(state, "SetTeamResource", spring_set_resource);
    set_function(state, "AreTeamsAllied", spring_teams_allied);
    set_function(state, "ValidUnitID", spring_valid_unit);
    set_function(state, "GetUnitBasePosition", spring_unit_base_position);
    set_function(state, "GetUnitPosition", spring_unit_position);
    set_function(state, "GetUnitDefID", spring_unit_def);
    set_function(state, "GetTeamUnits", spring_team_units);
    set_function(state, "GetTeamUnitCount", spring_team_unit_count);
    set_function(state, "GetTeamUnitDefCount", spring_team_unit_def_count);
    set_function(state, "GetUnitIsDead", spring_unit_is_dead);
    set_function(state, "GetUnitsInCylinder", spring_units_in_cylinder);
    set_function(state, "GetUnitNearestEnemy", spring_nearest_enemy);
    set_function(state, "GetUnitCommandCount", spring_command_count);
    set_function(state, "GetUnitTeam", spring_get_unit_team);
    set_function(state, "GetUnitsInRectangle", spring_units_in_rectangle);
    set_function(state, "TestBuildOrder", spring_test_build_order);
    set_function(state, "GetFullBuildQueue", spring_full_build_queue);
    set_function(state, "GetTeamResources", spring_team_resources);
    set_function(state, "GiveOrderToUnit", spring_give_order);
    set_function(state, "SetTeamShareLevel", spring_noop);
    set_function(state, "SetPlayerReadyState", spring_noop);
    set_function(state, "SetLogSectionFilterLevel", spring_noop);
    set_function(state, "Log", spring_log);
    set_function(state, "Echo", spring_echo);
    set_function(state, "_moveEnable", spring_move_enable);
    set_function(state, "_ensureUnitDef", spring_ensure_unit_def);
    lua_setglobal(state, "Spring");

    lua_newtable(state);
    lua_setglobal(state, "UnitDefs");
    lua_newtable(state);
    lua_setglobal(state, "UnitDefNames");
    for (const auto& entry : host.def_names) {
        UnitYield yield;
        std::string yield_error;
        if (!load_unit_yield(game_root, entry.second, yield, yield_error)) {
            yield = UnitYield{};
        }
        push_def(state, entry.first, entry.second.c_str(), yield.metal_storage, yield.energy_storage);
    }
    annotate_roles(state, host);

    const std::string package = game_root + "/?.lua;" + game_root + "/?/init.lua";
    lua_getglobal(state, "package");
    lua_pushlstring(state, package.data(), package.size());
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);

    if (!run_prelude(state, error)) {
        lua_close(state);
        return false;
    }
    lua_getglobal(state, "Game");
    lua_pushinteger(state, script.start_pos_type);
    lua_setfield(state, -2, "startPosType");
    lua_pushnumber(state, terrain.world_x);
    lua_setfield(state, -2, "mapSizeX");
    lua_pushnumber(state, terrain.world_z);
    lua_setfield(state, -2, "mapSizeZ");
    lua_pushnumber(state, terrain.max_wind);
    lua_setfield(state, -2, "windMax");
    lua_pushnumber(state, terrain.min_wind);
    lua_setfield(state, -2, "windMin");
    lua_pushlstring(state, script.map_name.data(), script.map_name.size());
    lua_setfield(state, -2, "mapName");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "waterDamage");
    lua_pop(state, 1);

    const std::string spawn_path = game_root + "/luarules/gadgets/game_initial_spawn.lua";
    const std::string resource_path = game_root + "/luarules/gadgets/game_team_resources.lua";
    if (!load_one(state, spawn_path, "bar_spawn", error) || !load_one(state, resource_path, "bar_resources", error) ||
        !call_method(state, "bar_spawn", "GameStart", error) ||
        !call_method(state, "bar_resources", "GameStart", error)) {
        lua_close(state);
        return false;
    }
    if (!load_behavior_gadgets(state, error)) {
        lua_close(state);
        return false;
    }
    install_spots(state, terrain);
    if (!load_simple_ai(state, host, error)) {
        lua_close(state);
        return false;
    }

    int required = 0;
    bool want_mode = false;
    for (const TeamStart& team : script.teams) {
        if (pve_exempt(team.lua_ai)) {
            want_mode = true;
        } else {
            ++required;
        }
    }
    bool spawn_ready = static_cast<int>(host.spawns.size()) == required;
    for (const TeamStart& team : script.teams) {
        const auto mask = host.resource_mask.find(team.team);
        if (mask == host.resource_mask.end() || mask->second != 15) {
            spawn_ready = false;
        }
        int asks = 0;
        bool bound = false;
        bool replaced = false;
        for (const SpawnAsk& ask : host.spawns) {
            if (ask.team != team.team) {
                continue;
            }
            ++asks;
            if (ask.unit >= 0) {
                bound = true;
            }
            if (ask.unit < 0 && !team.unit_def.empty() && team.unit_def != ask.def_name) {
                replaced = true;
            }
        }
        if (pve_exempt(team.lua_ai)) {
            if (asks != 0) {
                spawn_ready = false;
            }
        } else if (asks != 1 || (!bound && !replaced)) {
            spawn_ready = false;
        }
    }
    if (!spawn_ready || required == 0 || host.spawns.empty()) {
        error = "spawn gadgets did not set one commander and the team stocks";
        lua_close(state);
        return false;
    }
    const SpawnAsk& ask = host.spawns.front();
    report.loaded = true;
    report.def_name = ask.def_name;
    report.team = ask.team;
    report.x = ask.x;
    report.y = ask.y;
    report.z = ask.z;
    report.unit = ask.unit;
    report.hide = ask.hide;
    report.paralyze = ask.paralyze;
    report.movectrl = ask.movectrl;
    report.metal = host.metal;
    report.energy = host.energy;
    report.metal_store = host.metal_store;
    report.energy_store = host.energy_store;
    report.start_pos_type = script.start_pos_type;
    host.create_units = true;
    if (want_mode && !backfill_unit_health(state, host)) {
        lua_close(state);
        return false;
    }
    const std::string scav_path = game_root + "/luarules/gadgets/scav_spawner_defense.lua";
    const std::string raptor_path = game_root + "/luarules/gadgets/raptor_spawner_defense.lua";
    if (!load_mode_gadget(state, host, scav_path, "bar_scav", error) ||
        !load_mode_gadget(state, host, raptor_path, "bar_raptor", error)) {
        lua_close(state);
        return false;
    }
    WidgetEnv widgets;
    widgets.game_root = game_root;
    widgets.map_name = script.map_name;
    widgets.min_height = terrain.min_height;
    widgets.max_height = terrain.max_height;
    widgets.min_wind = terrain.min_wind;
    widgets.max_wind = terrain.max_wind;
    widgets.tidal = 0.f;
    widgets.view_w = 1280;
    widgets.view_h = 720;
    if (!widgets_load(state, widgets, error)) {
        lua_close(state);
        return false;
    }
    keep_gadget_session(state, std::move(host_owner));
    return true;
}

bool write_gadget_report(const std::string& path, const GadgetReport& report, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write gadget report " + path;
        return false;
    }
    out << "{\"def\":\"" << report.def_name << "\",\"team\":" << report.team << ",\"x\":" << report.x
        << ",\"y\":" << report.y << ",\"z\":" << report.z << ",\"unit\":" << report.unit << ",\"hide\":" << report.hide
        << ",\"paralyze\":" << report.paralyze << ",\"movectrl\":" << report.movectrl << ",\"metal\":" << report.metal
        << ",\"energy\":" << report.energy << ",\"metal_store\":" << report.metal_store
        << ",\"energy_store\":" << report.energy_store << ",\"pos\":" << report.start_pos_type << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar
