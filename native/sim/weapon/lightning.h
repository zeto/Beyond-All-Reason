#pragma once

#include <string>

namespace bar {

struct UnitSim;
struct TeamResources;

// One LightningCannon engagement. The aim script is not hosted, so the muzzle is the top center
// of the attacker's collision box and the aim point is the center of the target's box. Each bolt
// is a hitscan. It is not drawn. A burst spends its energy once, on the frame the burst starts.
// Chain sparks, shields, LOS, and the wreck are later slices.
struct LightningTrace {
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
    float energy_cost = 0;
    float energy = 0;
    int waterweapon = 0;
};

struct LightningBox {
    float hx = 0;
    float hy = 0;
    float hz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct LightningDuel {
    bool active = false;
    int attacker = -1;
    int target = -1;
    std::string attacker_def;
    std::string target_def;
    LightningBox attacker_box;
    LightningBox target_box;
    float damage = 0;
    float range = 0;
    int burst = 1;
    int salvo_delay = 0;
    int reload_frames = 0;
    float energy_cost = 0;
    float energy = 0;
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
};

bool lightning_begin(LightningDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                     std::string& error);

void lightning_step(LightningDuel& duel, const UnitSim& sim, TeamResources* resources, int sim_frame);

LightningTrace lightning_trace(const LightningDuel& duel);

}  // namespace bar
