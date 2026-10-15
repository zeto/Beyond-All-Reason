#include "build/death_wreck.h"

#include "build/reclaim.h"
#include "move/unit_sim.h"
#include "unitdef.h"

namespace bar {

void death_note(DeathWrecks& wrecks, const std::string& game_root, float ratio, const UnitSim& sim, bool active,
                int die, int target, int sim_frame) {
    if (!active || die != sim_frame || target < 0 || target >= static_cast<int>(sim.x.size())) {
        return;
    }
    for (const DeathWreck& have : wrecks.items) {
        if (have.unit == target) {
            return;
        }
    }
    const size_t at = static_cast<size_t>(target);
    UnitWreck corpse;
    std::string error;
    if (!load_unit_wreck(game_root, sim.def_name[at], corpse, error) || !corpse.present) {
        return;
    }
    DeathWreck wreck;
    wreck.def_name = sim.def_name[at];
    wreck.unit = target;
    wreck.born = sim_frame;
    wreck.x = sim.x[at];
    wreck.y = sim.y[at];
    wreck.z = sim.z[at];
    wreck.metal = wreck_metal_amount(corpse, ratio);
    wreck.metal_cost = corpse.metal_cost;
    wreck.ratio = ratio;
    wrecks.items.push_back(wreck);
}

}  // namespace bar
