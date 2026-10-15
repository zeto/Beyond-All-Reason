#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One AircraftBomb pass. The dropper inherits the airplane's velocity. A burst, once the
// predicted landing is inside the drop window, finishes without another range test. Map
// gravity is added after the hit test. The first box hit deals the default damage and
// removes that bomb. Misses that reach the ground are counted. The shot is not drawn.
struct BombTrace {
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
    int burst = 0;
    int salvo_delay = 0;
    float speed = 0;
    float accel = 0;
    float cruise = 0;
    float pitch = 0;
    int ttl = 0;
    float gravity = 0;
    int waterweapon = 0;
    float height_z0 = 0;
    float height_step = 0;
    std::vector<float> heights;
};

struct BombBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct BombShot {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    int ttl = 0;
};

struct BombDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    BombBox target_box;
    float damage = 0;
    float range = 0;
    float speed = 0;
    float accel = 0;
    float cruise = 0;
    float pitch = 0;
    float gravity = 0;
    int burst = 1;
    int salvo_delay = 0;
    int reload_frames = 0;
    int ttl = 1000;
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
    float height_z0 = 0;
    float height_step = 0;
    std::vector<float> heights;
    std::vector<BombShot> shots_live;
};

bool bomb_begin(BombDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                const TerrainMesh& terrain, std::string& error);

void bomb_step(BombDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

BombTrace bomb_trace(const BombDuel& duel);

}  // namespace bar
