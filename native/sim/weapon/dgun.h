#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One DGun engagement. The shot is a fireball: the segment is tested, then the
// shot moves, then map gravity is added. noexplode keeps it alive across
// overlaps until it has traveled the weapon range. It is not drawn.
struct DgunTrace {
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
    float gravity = 0;
};

struct DgunBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct DgunShot {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct DgunDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    DgunBox attacker_box;
    DgunBox target_box;
    float damage = 0;
    float range = 0;
    float speed = 0;
    float gravity = 0;
    int burst = 1;
    int salvo_delay = 0;
    int reload_frames = 0;
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
    std::vector<DgunShot> shots_live;
};

bool dgun_begin(DgunDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                const TerrainMesh& terrain, std::string& error);

void dgun_step(DgunDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame);

DgunTrace dgun_trace(const DgunDuel& duel);

}  // namespace bar
