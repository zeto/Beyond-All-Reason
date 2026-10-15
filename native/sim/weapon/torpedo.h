#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One TorpedoLauncher engagement. Tracking stays off, so the heading is the launch direction.
// Each frame tests pos -> pos+speed, then a powered shot adds 0.2 to its speed if it is still
// under weaponvelocity and moves by the new vector. Fuel is the weapon flighttime. Past that,
// the shot drags at 0.98 and takes map gravity. It is not drawn. Homing, splash, and the wreck
// are later slices.
struct TorpedoTrace {
    std::string def_name;
    std::string target_def;
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    float damage = 0;
    float range = 0;
    int reload_frames = 0;
    float speed = 0;
    float max_speed = 0;
    int ttl = 0;
    float gravity = 0;
    int waterweapon = 0;
};

struct TorpedoBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct TorpedoShot {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float dx = 0;
    float dy = 0;
    float dz = 0;
    float speed_w = 0;
    int ttl = 0;
};

struct TorpedoDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    TorpedoBox attacker_box;
    TorpedoBox target_box;
    float damage = 0;
    float range = 0;
    float speed = 0;
    float max_speed = 0;
    float gravity = 0;
    int flight_frames = 0;
    int burst = 1;
    int salvo_delay = 0;
    int reload_frames = 0;
    int ttl = 0;
    bool waterweapon = false;
    float target_hp = 0;
    float target_hp_max = 0;
    int shots = 0;
    int hits = 0;
    int die = 0;
    int ground = 0;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    float muzzle_x = 0;
    float muzzle_y = 0;
    float muzzle_z = 0;
    std::vector<TorpedoShot> shots_live;
};

bool torpedo_begin(TorpedoDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                   const TerrainMesh& terrain, std::string& error);

void torpedo_step(TorpedoDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

TorpedoTrace torpedo_trace(const TorpedoDuel& duel);

}  // namespace bar
