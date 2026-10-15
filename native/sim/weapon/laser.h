#pragma once

#include "projectile/laser_bolt.h"

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One direct-laser engagement. The aim script and the weapon piece are not hosted, so the
// muzzle is the top center of the attacker's collision box and the aim point is the target's
// position. Piece collision volumes are not hosted. Spray, burst, and an energy cost are not
// part of this duel. LOS and the wreck are later slices. No gadget adds damage on top.
struct LaserTrace {
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
};

struct LaserDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    LaserBox attacker_box;
    LaserBox target_box;
    float damage = 0;
    float speed = 0;
    float range = 0;
    int reload_frames = 0;
    bool burnblow = true;
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
    std::vector<LaserBolt> bolts;
};

bool laser_begin(LaserDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                 std::string& error);

void laser_step(LaserDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

LaserTrace laser_trace(const LaserDuel& duel);

}  // namespace bar
