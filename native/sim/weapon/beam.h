#pragma once

#include <string>

namespace bar {

struct UnitSim;

// One BeamLaser engagement. The aim script is not hosted, so the muzzle is the top center of the
// attacker's collision box and the aim point is the center of the target's box. The beam is a
// hitscan. It is not drawn. Splash, shields, water clipping, LOS, and the wreck are later slices.
struct BeamTrace {
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
    float beamtime = 0;
    int salvo_size = 0;
    float intensity = 0;
};

struct BeamBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct BeamDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    BeamBox attacker_box;
    BeamBox target_box;
    float damage = 0;
    float range = 0;
    float beamtime = 0;
    float min_intensity = 0;
    float intensity = 0;
    int salvo_size = 1;
    int reload_frames = 0;
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
};

bool beam_begin(BeamDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                std::string& error);

void beam_step(BeamDuel& duel, const UnitSim& sim, int sim_frame);

BeamTrace beam_trace(const BeamDuel& duel);

}  // namespace bar
