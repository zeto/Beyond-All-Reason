#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;

// One Flame engagement. The aim script is not hosted, so the muzzle is the top center of the
// attacker's collision box and the aim point is the center of the target's box. Each particle is
// tested along pos -> pos+speed before it moves, then its velocity gains -dir*0.001. It is not
// drawn. Area falloff, ground slide, shields, LOS, and the wreck are later slices.
struct FlameTrace {
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
    int ttl = 0;
    int waterweapon = 0;
};

struct FlameBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct FlameParticle {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float dx = 0;
    float dy = 0;
    float dz = 0;
    int life = 0;
    int ttl = 0;
};

struct FlameDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    FlameBox attacker_box;
    FlameBox target_box;
    float damage = 0;
    float range = 0;
    float speed = 0;
    float duration = 1.2f;
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
    std::vector<FlameParticle> particles;
};

bool flame_begin(FlameDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                 std::string& error);

void flame_step(FlameDuel& duel, const UnitSim& sim, int sim_frame);

FlameTrace flame_trace(const FlameDuel& duel);

}  // namespace bar
