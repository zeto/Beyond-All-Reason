#pragma once

#include "startscript.h"
#include "unitdef.h"

#include <string>

namespace bar {

// One team's stock. Income is metal and energy per second.
struct TeamResources {
    int team = 0;
    float metal = 0;
    float energy = 0;
    float metal_storage = 0;
    float energy_storage = 0;
    float metal_income = 0;
    float energy_income = 0;
    // Extractor metal per second, separate from metalmake so an unpaid upkeep can skip it.
    float mex_income = 0;
    // Energy per second the active extractors must pay before their metal arrives.
    float energy_upkeep = 0;
    // Energy per second from a negative upkeep, added even when a positive upkeep stalls.
    float solar_income = 0;
    // Energy per second from wind generators, on the same terms as solar income.
    float wind_income = 0;
};

struct ResourceRules {
    float start_metal = 0;
    float start_energy = 0;
    float start_metal_storage = 0;
    float start_energy_storage = 0;
    float min_metal_storage = 0;
    float min_energy_storage = 0;
    // multiplier_energyproduction * multiplier_resourceincome. 1 on a default game.
    float energy_scale = 1;
    // multiplier_metalextraction * multiplier_resourceincome. Applied only when the
    // unit's metal_extractor custom is set, which is the alldefs_post rule.
    float metal_scale = 1;
};

// Reads modoptions.lua defaults, the storage floor in game_team_resources.lua, then the
// start script's modoption overrides. A bonus or proposed-rework flag is an error: those
// paths change the commander def, which this slice does not load.
bool load_resource_rules(const std::string& game_root, const StartScript& script, ResourceRules& rules,
                         std::string& error);

// Storage is the gadget's max, not the commander's storage added on top of it.
bool team_resources_open(TeamResources& team, int team_id, const ResourceRules& rules, const UnitYield& yield,
                         std::string& error);

// One sim frame. Income per second is divided by 30. Solar and wind are income, so they
// arrive even when extractor upkeep cannot be paid. Extractor upkeep is paid from that
// same frame, and the metal from the ground is added only when the energy can cover it.
// The stock is then clamped to storage. A team with no solar and no wind adds nothing
// extra, so the commander tick is unchanged.
void team_resources_step(TeamResources& team);

}  // namespace bar
