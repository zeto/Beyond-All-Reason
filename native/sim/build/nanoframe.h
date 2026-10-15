#pragma once

#include "economy/team_resources.h"
#include "smf.h"

#include <string>

namespace bar {

// Construction and reclaim numbers from gamedata/modrules.lua, plus the build multipliers
// from modoptions.lua. A start script does not override the multipliers in this slice.
struct ConstructionRules {
    bool decay = false;
    float decay_time = 0;
    float decay_speed = 0;
    float multi_reclaim = 0;
    float reclaim_method = 0;
    float unit_method = 0;
    float unit_energy_cost_factor = 0;
    float unit_efficiency = 0;
    float feature_energy_cost_factor = 0;
    bool allow_enemies = false;
    bool allow_allies = false;
    float repair_energy_cost_factor = 0;
    float resurrect_energy_cost_factor = 0;
    float capture_energy_cost_factor = 0;
    float build_power_scale = 1;
    float build_distance_scale = 1;
    float wreck_metal_ratio = 0.6f;
};

// One unfinished building. It is not a simulated unit and it is not drawn.
struct Nanoframe {
    bool active = false;
    bool finished = false;
    int builder = 0;
    // Spring unit id reserved for this incomplete building (not drawn until finished).
    int unit_id = 0;
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float progress = 0;
    float health = 0;
    float max_health = 0;
    float build_time = 0;
    float metal_cost = 0;
    float energy_cost = 0;
    float worker_time = 0;
    float build_distance = 0;
    float power_per_frame = 0;
    float power_scale = 1;
    bool decay = false;
    float decay_time = 0;
    float decay_speed = 0;
    int last_nano_frame = -1;
    // A factory pad ignores build distance. A builder nanoframe leaves this false.
    bool pad = false;
};

struct NanoTrace {
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float progress = 0;
    float health = 0;
    float build_time = 0;
    float metal_cost = 0;
    float energy_cost = 0;
    float worker_time = 0;
    float build_distance = 0;
    float power_scale = 1;
    int finished = 0;
    int decay = 0;
    float decay_time = 0;
    float decay_speed = 0;
};

bool load_construction_rules(const std::string& game_root, ConstructionRules& rules, std::string& error);

// Snaps the click, checks the builder can make the def, and leaves progress at 0.
// Ground leveling is not done: the frame is placed on the current height and progress
// starts on the next spend. The builder script's build stance is not consulted.
bool nano_begin(Nanoframe& nano, const std::string& game_root, const ConstructionRules& rules, int builder,
                const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                const std::string& build_def, float click_x, float click_z, const TerrainMesh& terrain,
                std::string& error);

// Places the product on the factory. The yard script, the build stance, the pad-piece
// offset, and the walk out of the yard are not run, so production starts this frame.
// The finished unit stays at the factory position and is not added to the sim.
bool factory_begin(Nanoframe& nano, const std::string& game_root, const ConstructionRules& rules, int builder,
                   const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                   const std::string& build_def, std::string& error);

// One frame after income. No-op when no frame is open. A full step is spent or nothing is.
// An out-of-range builder does not spend. A factory pad skips that range test. Decay is
// not stepped: a working builder resets the nano timer every frame, and this slice does
// not stop the builder.
void nano_step(Nanoframe& nano, TeamResources& team, float builder_x, float builder_y, float builder_z, int frame);

NanoTrace nano_trace(const Nanoframe& nano);

}  // namespace bar
