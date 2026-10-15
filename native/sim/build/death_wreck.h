#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;

// A feature left when a unit dies. It is not a unit, it is not drawn, and it
// does not pay the team. Metal uses the same floor(metalcost * ratio) rule as
// reclaim.
struct DeathWreck {
    std::string def_name;
    int unit = -1;
    int born = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float metal = 0;
    float metal_cost = 0;
    float ratio = 0;
};

struct DeathWrecks {
    std::vector<DeathWreck> items;
};

void death_note(DeathWrecks& wrecks, const std::string& game_root, float ratio, const UnitSim& sim, bool active,
                int die, int target, int sim_frame);

}  // namespace bar
