#pragma once

#include "economy/team_resources.h"
#include "smf.h"
#include "unitdef.h"

#include <string>

namespace bar {

struct ConstructionRules;

// alldefs_post replaces a non-commander wreck's metal with floor(metalcost * ratio)
// when the unit has health and both costs. The file's own metal is the fallback.
float wreck_metal_amount(const UnitWreck& corpse, float ratio);

// One wreck being reclaimed. It is not a simulated unit and it is not drawn.
struct Wreck {
    bool active = false;
    bool gone = false;
    int builder = 0;
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float left = 1;
    float metal = 0;
    float energy = 0;
    float remain_metal = 0;
    float remain_energy = 0;
    float reclaim_time = 0;
    float reclaim_speed = 0;
    float build_distance = 0;
    float power_per_frame = 0;
    float power_scale = 1;
    float reclaim_method = 0;
    float energy_factor = 0;
    float multi_reclaim = 0;
};

struct WreckTrace {
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float left = 1;
    float metal = 0;
    float energy = 0;
    float reclaim_time = 0;
    float reclaim_speed = 0;
    float build_distance = 0;
    float power_scale = 1;
    float reclaim_method = 0;
    float energy_factor = 0;
    float multi_reclaim = 0;
    int gone = 0;
};

// Places the named unit's corpse. The builder's build stance is not consulted, so
// reclaim starts on the order frame. The corpse metal follows the wreck-ratio rule.
bool reclaim_begin(Wreck& wreck, const std::string& game_root, const ConstructionRules& rules, int builder,
                   const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                   const std::string& unit_def, float wreck_x, float wreck_z, const TerrainMesh& terrain,
                   std::string& error);

// One frame after income. No-op when no wreck is open. An out-of-range builder does not reclaim.
void reclaim_step(Wreck& wreck, TeamResources& team, float builder_x, float builder_y, float builder_z);

WreckTrace wreck_trace(const Wreck& wreck);

}  // namespace bar
