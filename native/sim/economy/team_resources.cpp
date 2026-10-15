#include "economy/team_resources.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace bar {
namespace {

constexpr float kSimHz = 30.f;

bool lua_option_number(lua_State* state, const char* key, float& out) {
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

bool lua_option_bool(lua_State* state, const char* key, bool& out) {
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
        const bool ok = lua_isboolean(state, -1) != 0;
        if (ok) {
            out = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 2);
        return ok;
    }
    return false;
}

bool local_number(const std::string& text, const char* name, float& out) {
    const std::string key = std::string("local ") + name;
    const auto at = text.find(key);
    if (at == std::string::npos) {
        return false;
    }
    const auto eq = text.find('=', at + key.size());
    if (eq == std::string::npos || eq > at + key.size() + 8) {
        return false;
    }
    char* end = nullptr;
    out = std::strtof(text.c_str() + eq + 1, &end);
    return end != text.c_str() + eq + 1;
}

}  // namespace

bool load_resource_rules(const std::string& game_root, const StartScript& script, ResourceRules& rules,
                         std::string& error) {
    rules = ResourceRules{};
    lua_State* state = luaL_newstate();
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
    if (!lua_istable(state, -1)) {
        error = "modoptions did not return a table";
        lua_close(state);
        return false;
    }
    float energy_production = 1.f;
    float resource_income = 1.f;
    float metal_extraction = 1.f;
    bool reworks = false;
    bool bonus = false;
    const bool numbers = lua_option_number(state, "startmetal", rules.start_metal) &&
                         lua_option_number(state, "startenergy", rules.start_energy) &&
                         lua_option_number(state, "startmetalstorage", rules.start_metal_storage) &&
                         lua_option_number(state, "startenergystorage", rules.start_energy_storage) &&
                         lua_option_number(state, "multiplier_energyproduction", energy_production) &&
                         lua_option_number(state, "multiplier_resourceincome", resource_income) &&
                         lua_option_number(state, "multiplier_metalextraction", metal_extraction);
    const bool flags = lua_option_bool(state, "proposed_unit_reworks", reworks) &&
                       lua_option_bool(state, "bonusstartresourcemultiplier", bonus);
    lua_close(state);
    if (!numbers || !flags) {
        error = "modoptions is missing a resource default";
        return false;
    }

    std::ifstream gadget(game_root + "/luarules/gadgets/game_team_resources.lua");
    if (!gadget) {
        error = "could not read game_team_resources.lua";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(gadget)), std::istreambuf_iterator<char>());
    if (!local_number(text, "minStorageMetal", rules.min_metal_storage) ||
        !local_number(text, "minStorageEnergy", rules.min_energy_storage)) {
        error = "game_team_resources.lua is missing a storage floor";
        return false;
    }

    const ResourceOverride& over = script.options;
    if (over.has_start_metal) {
        rules.start_metal = over.start_metal;
    }
    if (over.has_start_energy) {
        rules.start_energy = over.start_energy;
    }
    if (over.has_start_metal_storage) {
        rules.start_metal_storage = over.start_metal_storage;
    }
    if (over.has_start_energy_storage) {
        rules.start_energy_storage = over.start_energy_storage;
    }
    if (over.has_energy_production) {
        energy_production = over.energy_production;
    }
    if (over.has_resource_income) {
        resource_income = over.resource_income;
    }
    if (over.has_metal_extraction) {
        metal_extraction = over.metal_extraction;
    }
    if (over.has_reworks) {
        reworks = over.reworks;
    }
    if (over.has_bonus) {
        bonus = over.bonus;
    }
    if (reworks) {
        error = "proposed unit reworks change the commander def, which this slice does not load";
        return false;
    }
    if (bonus) {
        error = "bonus start resources need a team multiplier this slice does not read";
        return false;
    }
    rules.energy_scale = energy_production * resource_income;
    rules.metal_scale = metal_extraction * resource_income;
    return true;
}

bool team_resources_open(TeamResources& team, int team_id, const ResourceRules& rules, const UnitYield& yield,
                         std::string& error) {
    (void)error;
    const float metal = rules.start_metal;
    const float energy = rules.start_energy;
    // The gadget's GameStart call puts storage back to this max. The commander's own
    // storage is one of the inputs, not a second addition after the max.
    const float energy_store =
        yield.energy_make > 0.f ? yield.energy_storage * rules.energy_scale : yield.energy_storage;
    team.team = team_id;
    team.metal = metal;
    team.energy = energy;
    team.metal_storage =
        std::max(std::max(rules.min_metal_storage, rules.start_metal_storage), std::max(metal, yield.metal_storage));
    team.energy_storage = std::max(std::max(rules.min_energy_storage, rules.start_energy_storage),
                                   std::max(energy, energy_store));
    team.metal_income = yield.metal_make;
    team.energy_income = yield.energy_make * rules.energy_scale;
    return true;
}

void team_resources_step(TeamResources& team) {
    const float make_metal = team.metal_income / kSimHz;
    // Adding zero solar and zero wind leaves the quotient unchanged for a finite income.
    const float make_energy = (team.energy_income + team.solar_income + team.wind_income) / kSimHz;
    const float pull = team.energy_upkeep / kSimHz;
    const float mex = team.mex_income / kSimHz;
    team.metal = std::clamp(team.metal + make_metal, 0.f, team.metal_storage);
    // Income and upkeep land in the same frame, then the stock clamps. A zero upkeep
    // subtracts nothing, so a commander-only team matches the previous tick.
    if (pull <= team.energy + make_energy) {
        team.energy = std::clamp(team.energy + make_energy - pull, 0.f, team.energy_storage);
        team.metal = std::clamp(team.metal + mex, 0.f, team.metal_storage);
    } else {
        team.energy = std::clamp(team.energy + make_energy, 0.f, team.energy_storage);
    }
}

}  // namespace bar
