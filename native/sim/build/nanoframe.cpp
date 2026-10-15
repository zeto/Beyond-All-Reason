#include "build/nanoframe.h"

#include "unitdef.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <string>

namespace bar {
namespace {

constexpr float kSimHz = 30.f;
constexpr float kSquare = 8.f;
constexpr float kBuildSquare = 16.f;
constexpr float kMinBuildDistance = 38.f;
// A new nanoframe starts here. The file's health is the finished maximum.
constexpr float kNanoHealth = 0.1f;

bool option_number(lua_State* state, const char* key, float& out) {
    const int count = static_cast<int>(lua_objlen(state, -1));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, i);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        lua_getfield(state, -1, "key");
        const bool match = lua_isstring(state, -1) != 0 && std::string(lua_tostring(state, -1)) == key;
        lua_pop(state, 1);
        if (!match) {
            lua_pop(state, 1);
            continue;
        }
        lua_getfield(state, -1, "def");
        const bool ok = lua_isnumber(state, -1) != 0;
        if (ok) {
            out = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 2);
        return ok;
    }
    return false;
}

bool field_number(lua_State* state, const char* key, float& out, std::string& error) {
    lua_getfield(state, -1, key);
    if (!lua_isnumber(state, -1)) {
        error = std::string("modrules missing ") + key;
        lua_pop(state, 1);
        return false;
    }
    out = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    return true;
}

bool field_bool(lua_State* state, const char* key, bool& out, std::string& error) {
    lua_getfield(state, -1, key);
    if (!lua_isboolean(state, -1)) {
        error = std::string("modrules missing ") + key;
        lua_pop(state, 1);
        return false;
    }
    out = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return true;
}

bool open_group(lua_State* state, const char* key, std::string& error) {
    lua_getfield(state, -1, key);
    if (!lua_istable(state, -1)) {
        error = std::string("modrules missing ") + key;
        return false;
    }
    return true;
}

float snap_axis(float click, int footprint) {
    const int size = footprint * 2;
    if ((size & 2) != 0) {
        return std::floor(click / kBuildSquare) * kBuildSquare + kSquare;
    }
    return std::floor((click + kSquare) / kBuildSquare) * kBuildSquare;
}

bool listed(const UnitBuilder& builder, const std::string& def_name) {
    for (const std::string& option : builder.options) {
        if (option == def_name) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool load_construction_rules(const std::string& game_root, ConstructionRules& rules, std::string& error) {
    rules = ConstructionRules{};
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    const char* stub =
        "VFS = { Include = function() return nil end, DirList = function() return {} end }\n"
        "Script = { IsEngineMinVersion = function() return true end }\n"
        "Spring = { GetModOptions = function() return {} end, Echo = function() end }\n";
    if (luaL_dostring(state, stub) != 0) {
        error = "modrules stub failed";
        lua_close(state);
        return false;
    }
    const std::string path = game_root + "/gamedata/modrules.lua";
    if (luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "modrules failed";
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "modrules did not return a table";
        lua_close(state);
        return false;
    }
    if (!open_group(state, "construction", error) || !field_bool(state, "constructionDecay", rules.decay, error) ||
        !field_number(state, "constructionDecayTime", rules.decay_time, error) ||
        !field_number(state, "constructionDecaySpeed", rules.decay_speed, error)) {
        lua_close(state);
        return false;
    }
    lua_pop(state, 1);
    if (!open_group(state, "reclaim", error) || !field_number(state, "multiReclaim", rules.multi_reclaim, error) ||
        !field_number(state, "reclaimMethod", rules.reclaim_method, error) ||
        !field_number(state, "unitMethod", rules.unit_method, error) ||
        !field_number(state, "unitEnergyCostFactor", rules.unit_energy_cost_factor, error) ||
        !field_number(state, "unitEfficiency", rules.unit_efficiency, error) ||
        !field_number(state, "featureEnergyCostFactor", rules.feature_energy_cost_factor, error) ||
        !field_bool(state, "allowEnemies", rules.allow_enemies, error) ||
        !field_bool(state, "allowAllies", rules.allow_allies, error)) {
        lua_close(state);
        return false;
    }
    lua_pop(state, 1);
    if (!open_group(state, "repair", error) ||
        !field_number(state, "energyCostFactor", rules.repair_energy_cost_factor, error)) {
        lua_close(state);
        return false;
    }
    lua_pop(state, 1);
    if (!open_group(state, "resurrect", error) ||
        !field_number(state, "energyCostFactor", rules.resurrect_energy_cost_factor, error)) {
        lua_close(state);
        return false;
    }
    lua_pop(state, 1);
    if (!open_group(state, "capture", error) ||
        !field_number(state, "energyCostFactor", rules.capture_energy_cost_factor, error)) {
        lua_close(state);
        return false;
    }
    lua_pop(state, 2);
    lua_close(state);

    state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    const std::string options_path = game_root + "/modoptions.lua";
    if (luaL_loadfile(state, options_path.c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "modoptions failed";
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1) || !option_number(state, "multiplier_buildpower", rules.build_power_scale) ||
        !option_number(state, "multiplier_builddistance", rules.build_distance_scale) ||
        !option_number(state, "wreck_metal_ratio", rules.wreck_metal_ratio)) {
        error = "modoptions is missing a build multiplier";
        lua_close(state);
        return false;
    }
    lua_close(state);
    return true;
}

bool nano_begin(Nanoframe& nano, const std::string& game_root, const ConstructionRules& rules, int builder,
                const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                const std::string& build_def, float click_x, float click_z, const TerrainMesh& terrain,
                std::string& error) {
    if (nano.active) {
        error = "a nanoframe is already under construction";
        return false;
    }
    UnitBuilder maker;
    if (!load_unit_builder(game_root, builder_def, maker, error)) {
        return false;
    }
    if (!maker.builder || !(maker.worker_time > 0.f)) {
        error = builder_def + " is not a builder";
        return false;
    }
    if (!listed(maker, build_def)) {
        error = builder_def + " cannot build " + build_def;
        return false;
    }
    UnitBuild build;
    if (!load_unit_build(game_root, build_def, build, error)) {
        return false;
    }
    if (!(build.build_time > 0.f) || !(build.health > 0.f) || build.footprint_x < 1 || build.footprint_z < 1) {
        error = build_def + " has no build time, health, or footprint";
        return false;
    }
    const float reach = std::max(kMinBuildDistance, maker.build_distance * rules.build_distance_scale);
    const float x = snap_axis(click_x, build.footprint_x);
    const float z = snap_axis(click_z, build.footprint_z);
    const float y = terrain_height_at(terrain, x, z);
    const float dx = builder_x - x;
    const float dy = builder_y - y;
    const float dz = builder_z - z;
    // Radius 0. A real buildee radius would only make this check looser.
    if (dx * dx + dy * dy + dz * dz > reach * reach) {
        error = "build site is out of range";
        return false;
    }
    nano = Nanoframe{};
    nano.active = true;
    nano.builder = builder;
    nano.def_name = build_def;
    nano.x = x;
    nano.y = y;
    nano.z = z;
    nano.progress = 0.f;
    nano.health = kNanoHealth;
    nano.max_health = build.health;
    nano.build_time = build.build_time;
    nano.metal_cost = build.metal_cost;
    nano.energy_cost = build.energy_cost;
    nano.worker_time = maker.worker_time * rules.build_power_scale;
    nano.build_distance = reach;
    nano.power_per_frame = nano.worker_time / kSimHz;
    nano.power_scale = rules.build_power_scale;
    nano.decay = rules.decay;
    nano.decay_time = rules.decay_time;
    nano.decay_speed = rules.decay_speed;
    return true;
}

bool factory_begin(Nanoframe& nano, const std::string& game_root, const ConstructionRules& rules, int builder,
                   const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                   const std::string& build_def, std::string& error) {
    if (nano.active) {
        error = "a nanoframe is already under construction";
        return false;
    }
    UnitBuilder maker;
    if (!load_unit_builder(game_root, builder_def, maker, error)) {
        return false;
    }
    if (!maker.builder || !(maker.worker_time > 0.f)) {
        error = builder_def + " is not a builder";
        return false;
    }
    if (!listed(maker, build_def)) {
        error = builder_def + " cannot build " + build_def;
        return false;
    }
    UnitBuild build;
    if (!load_unit_build(game_root, build_def, build, error)) {
        return false;
    }
    if (!(build.build_time > 0.f) || !(build.health > 0.f)) {
        error = build_def + " has no build time or health";
        return false;
    }
    nano = Nanoframe{};
    nano.active = true;
    nano.pad = true;
    nano.builder = builder;
    nano.def_name = build_def;
    nano.x = builder_x;
    nano.y = builder_y;
    nano.z = builder_z;
    nano.progress = 0.f;
    nano.health = kNanoHealth;
    nano.max_health = build.health;
    nano.build_time = build.build_time;
    nano.metal_cost = build.metal_cost;
    nano.energy_cost = build.energy_cost;
    nano.worker_time = maker.worker_time * rules.build_power_scale;
    // A factory has no build distance. The 38-elmo builder floor does not apply on the pad.
    nano.build_distance = maker.build_distance * rules.build_distance_scale;
    nano.power_per_frame = nano.worker_time / kSimHz;
    nano.power_scale = rules.build_power_scale;
    nano.decay = rules.decay;
    nano.decay_time = rules.decay_time;
    nano.decay_speed = rules.decay_speed;
    return true;
}

void nano_step(Nanoframe& nano, TeamResources& team, float builder_x, float builder_y, float builder_z, int frame) {
    if (!nano.active || nano.finished) {
        return;
    }
    if (!nano.pad) {
        const float dx = builder_x - nano.x;
        const float dy = builder_y - nano.y;
        const float dz = builder_z - nano.z;
        if (dx * dx + dy * dy + dz * dz > nano.build_distance * nano.build_distance) {
            return;
        }
    }
    const float room = 1.f - nano.progress;
    if (!(room > 0.f)) {
        return;
    }
    // In range, so the builder touched the frame this tick even if the team cannot pay.
    nano.last_nano_frame = frame;
    const float step = std::min(nano.power_per_frame / nano.build_time, room);
    const float metal = nano.metal_cost * step;
    const float energy = nano.energy_cost * step;
    if (team.metal < metal || team.energy < energy) {
        return;
    }
    team.metal = std::clamp(team.metal - metal, 0.f, team.metal_storage);
    team.energy = std::clamp(team.energy - energy, 0.f, team.energy_storage);
    nano.health = std::min(nano.max_health, nano.health + nano.max_health * step);
    nano.progress += step;
    if (nano.progress >= 1.f) {
        nano.finished = true;
        nano.progress = 1.f;
        // Storage and income stay off. A finished product is not walked out.
    }
}

NanoTrace nano_trace(const Nanoframe& nano) {
    NanoTrace trace;
    trace.def_name = nano.def_name;
    trace.x = nano.x;
    trace.y = nano.y;
    trace.z = nano.z;
    trace.progress = nano.progress;
    trace.health = nano.health;
    trace.build_time = nano.build_time;
    trace.metal_cost = nano.metal_cost;
    trace.energy_cost = nano.energy_cost;
    trace.worker_time = nano.worker_time;
    trace.build_distance = nano.build_distance;
    trace.power_scale = nano.power_scale;
    trace.finished = nano.finished ? 1 : 0;
    trace.decay = nano.decay ? 1 : 0;
    trace.decay_time = nano.decay_time;
    trace.decay_speed = nano.decay_speed;
    return trace;
}

}  // namespace bar
