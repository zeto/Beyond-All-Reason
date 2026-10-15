#pragma once

#include "projectile/cannon_shell.h"

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One direct cannon engagement. The aim script and the weapon piece are not hosted, so the
// muzzle is the top center of the attacker's collision box and the aim point is the center
// of the target's unit box. Piece collision volumes are not hosted. Spray, burst, bounce,
// and an energy cost are not part of this duel. Splash, LOS, and the wreck are later slices.
struct CannonTrace {
    std::string def_name;
    std::string target_def;
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    float damage = 0;
    float speed = 0;
    float range = 0;
    int reload_frames = 0;
    float gravity = 0;
    float height_mod = 0;
};

struct CannonDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    CannonBox attacker_box;
    CannonBox target_box;
    float damage = 0;
    float speed = 0;
    float range = 0;
    float gravity = 0;
    float height_mod = 0.8f;
    int reload_frames = 0;
    float target_hp = 0;
    float target_hp_max = 0;
    int shots = 0;
    int hits = 0;
    int die = 0;
    int ground = 0;
    int reload_ready = 0;
    float muzzle_x = 0;
    float muzzle_y = 0;
    float muzzle_z = 0;
    std::vector<CannonShell> shells;
};

bool cannon_begin(CannonDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                  const TerrainMesh& terrain, std::string& error);

void cannon_step(CannonDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

CannonTrace cannon_trace(const CannonDuel& duel);

}  // namespace bar
