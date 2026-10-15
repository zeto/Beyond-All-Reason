#pragma once

#include "projectile/missile_rocket.h"

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One unguided MissileLauncher engagement. The aim script is not hosted, so the muzzle is the
// top center of the attacker's collision box and the aim point is the center of the target's box.
// Homing, arc, wobble, and splash are not part of this duel. LOS and the wreck are later slices.
struct MissileTrace {
    std::string def_name;
    std::string target_def;
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    float damage = 0;
    float speed = 0;
    float max_speed = 0;
    float accel = 0;
    float range = 0;
    int reload_frames = 0;
    float height_mod = 0;
};

struct MissileDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    MissileBox attacker_box;
    MissileBox target_box;
    float damage = 0;
    float speed = 0;
    float max_speed = 0;
    float accel = 0;
    float range = 0;
    float height_mod = 0.2f;
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
    std::vector<MissileRocket> rockets;
};

bool missile_begin(MissileDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                   std::string& error);

void missile_step(MissileDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

MissileTrace missile_trace(const MissileDuel& duel);

}  // namespace bar
