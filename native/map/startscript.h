#pragma once

#include <string>
#include <vector>

namespace bar {

struct TeamStart {
    int team = 0;
    int ally_team = 0;
    std::string side;
    bool has_start = false;
    float start_x = 0;
    float start_z = 0;
    bool has_color = false;
    float color_r = 0.9f;
    float color_g = 0.15f;
    float color_b = 0.12f;
    // Empty means the side's commander. A named def is that unit instead.
    std::string unit_def;
    // Empty means a human team. SimpleAI is the layer-3 skirmish opponent.
    std::string lua_ai;
};

// A finished unit placed by the scenario. A nanoframe comes from a build or make order, not this block.
struct ScriptUnit {
    std::string def_name;
    int team = 0;
    bool has_x = false;
    bool has_z = false;
    float x = 0;
    float z = 0;
};

// Present only when the start script assigns that modoption. Empty means the file default.
struct ResourceOverride {
    bool has_start_metal = false;
    float start_metal = 0;
    bool has_start_energy = false;
    float start_energy = 0;
    bool has_start_metal_storage = false;
    float start_metal_storage = 0;
    bool has_start_energy_storage = false;
    float start_energy_storage = 0;
    bool has_energy_production = false;
    float energy_production = 1;
    bool has_resource_income = false;
    float resource_income = 1;
    bool has_metal_extraction = false;
    float metal_extraction = 1;
    bool has_reworks = false;
    bool reworks = false;
    bool has_bonus = false;
    bool bonus = false;
};

struct StartScript {
    std::string map_name;
    // 0 is fixed map positions. The gadget spawns the commander there.
    int start_pos_type = 0;
    std::vector<TeamStart> teams;
    std::vector<ScriptUnit> extras;
    ResourceOverride options;
};

bool parse_start_script(const std::string& path, StartScript& out, std::string& error);

}  // namespace bar
