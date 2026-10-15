#pragma once

#include "smf.h"
#include "unit_spawn.h"
#include "unitdef.h"

#include <string>
#include <vector>

namespace bar {

// SoA unit store. Hot loops walk these arrays through run_range.
struct UnitSim {
    std::vector<int> id;
    std::vector<int> team;
    std::vector<std::string> def_name;
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;
    std::vector<float> heading;
    std::vector<float> speed;
    // Vertical step from the last airplane update. Ground units leave this at 0.
    std::vector<float> vy;
    std::vector<float> max_speed;
    std::vector<float> max_acc;
    std::vector<float> max_dec;
    std::vector<float> turn_rate;
    std::vector<float> max_slope;
    std::vector<float> max_water;
    std::vector<float> min_water;
    std::vector<float> waterline;
    std::vector<float> cruise;
    std::vector<float> bank;
    std::vector<float> max_rudder;
    std::vector<float> max_aileron;
    std::vector<float> max_bank;
    std::vector<float> max_pitch;
    std::vector<float> turn_radius;
    std::vector<char> floater;
    std::vector<char> hover;
    std::vector<char> gunship;
    std::vector<char> airplane;
    std::vector<char> can_move;
    std::vector<char> has_goal;
    // 0 after DestroyUnit. The slot stays so later ids do not move.
    std::vector<char> alive;
    std::vector<float> health;
    std::vector<float> max_health;
    std::vector<std::vector<float>> path_x;
    std::vector<std::vector<float>> path_z;
    std::vector<int> path_at;
};

// Heading 0 faces +z. A positive heading turns toward +x.
bool move_frame_ok(std::string& error);

void sim_clear(UnitSim& sim);

// ground_y is the terrain under the spawn. Hover and gunship replace the spawned y.
bool sim_add_unit(UnitSim& sim, const UnitSpawn& spawn, const UnitMotion& motion, float max_slope_deg,
                  float max_water_depth, float min_water_depth, float ground_y, bool hover, float cruise,
                  std::string& error);

bool sim_order_move(UnitSim& sim, int index, float goal_x, float goal_z, const TerrainMesh& terrain,
                    std::string& error);

void sim_step(UnitSim& sim, const TerrainMesh& terrain);

// Optional team stock written beside the units. Null leaves the line as units only.
struct TeamTrace {
    int id = 0;
    float metal = 0;
    float energy = 0;
    float metal_storage = 0;
    float energy_storage = 0;
    float metal_income = 0;
    float energy_income = 0;
    float mex_income = 0;
    float energy_upkeep = 0;
    float solar_income = 0;
    float wind_income = 0;
};

struct MexTrace;
struct GenTrace;
struct NanoTrace;
struct WreckTrace;
struct LaserTrace;
struct CannonTrace;
struct BeamTrace;
struct MissileTrace;
struct LightningTrace;
struct FlameTrace;
struct TorpedoTrace;
struct BombTrace;
struct DgunTrace;
struct LosDuel;
struct DeathWrecks;

bool write_sim_trace(const std::string& path, int frame, const UnitSim& sim, const TerrainMesh& terrain, bool append,
                     std::string& error, const std::vector<TeamTrace>* teams = nullptr,
                     const std::vector<MexTrace>* mex = nullptr,
                     const std::vector<GenTrace>* gen = nullptr, const NanoTrace* nano = nullptr,
                     const WreckTrace* wreck = nullptr, const LaserTrace* laser = nullptr,
                     const CannonTrace* cannon = nullptr, const BeamTrace* beam = nullptr,
                     const MissileTrace* missile = nullptr, const LightningTrace* lightning = nullptr,
                     const FlameTrace* flame = nullptr, const TorpedoTrace* torpedo = nullptr,
                     const BombTrace* bomb = nullptr, const DgunTrace* dgun = nullptr,
                     const LosDuel* los = nullptr, const DeathWrecks* deaths = nullptr);

}  // namespace bar