#include "move/unit_sim.h"

#include "build/nanoframe.h"
#include "build/reclaim.h"
#include "weapon/beam.h"
#include "weapon/cannon.h"
#include "weapon/laser.h"
#include "weapon/lightning.h"
#include "weapon/flame.h"
#include "weapon/torpedo.h"
#include "weapon/bomb.h"
#include "weapon/dgun.h"
#include "los/los.h"
#include "build/death_wreck.h"
#include "weapon/missile.h"
#include "economy/energy_yield.h"
#include "economy/metal_extract.h"
#include "job/jobs.h"
#include "path/astar.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>

namespace bar {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSimHz = 30.f;
// Spring's total-annihilation angle: 65536 units per revolution.
constexpr float kTaToRad = kPi / 32768.f;
constexpr float kTurnInPlace = 0.50f;

float wrap_pi(float angle) {
    while (angle > kPi) {
        angle -= 2.f * kPi;
    }
    while (angle < -kPi) {
        angle += 2.f * kPi;
    }
    return angle;
}

void direction(float heading, float& dx, float& dz) {
    dx = std::sin(heading);
    dz = std::cos(heading);
}

struct StepContext {
    UnitSim* sim = nullptr;
    const TerrainMesh* terrain = nullptr;
};

void place_height(UnitSim& sim, size_t i, float ground) {
    const float surface = ground < 0.f ? 0.f : ground;
    if (sim.gunship[i]) {
        // Cruise height is above the ground, or above the sea, whichever is higher.
        sim.y[i] = surface + sim.cruise[i];
        return;
    }
    if (sim.hover[i]) {
        sim.y[i] = surface;
        return;
    }
    // A floater holds the waterline. A ground unit stands on the terrain sample.
    sim.y[i] = sim.floater[i] ? sim.waterline[i] : ground;
}

// An airplane yaws with the rudder while it flies. It does not stop to turn or to arrive.
void step_airplane(UnitSim& sim, size_t i, const TerrainMesh& terrain) {
    constexpr float kArrive = 8.f;
    const bool chasing = sim.has_goal[i] != 0 && sim.path_at[i] >= 0 &&
                         sim.path_at[i] < static_cast<int>(sim.path_x[i].size());
    // A parked airplane stays put. One that already has speed keeps flying after the goal clears.
    if (!chasing && !(sim.speed[i] > 0.f)) {
        sim.vy[i] = 0.f;
        return;
    }
    float yaw = 0.f;
    float cap = 0.f;
    if (chasing) {
        const size_t cursor = static_cast<size_t>(sim.path_at[i]);
        const float dx = sim.path_x[i][cursor] - sim.x[i];
        const float dz = sim.path_z[i][cursor] - sim.z[i];
        const float desired = std::atan2(dx, dz);
        const float delta = wrap_pi(desired - sim.heading[i]);
        cap = sim.max_rudder[i];
        if (sim.speed[i] > 0.f && sim.turn_radius[i] > 1.f) {
            cap = std::min(cap, sim.speed[i] / sim.turn_radius[i]);
        }
        if (!(sim.speed[i] > 0.f)) {
            cap = 0.f;
        }
        yaw = std::clamp(delta, -cap, cap);
        sim.heading[i] = wrap_pi(sim.heading[i] + yaw);
    }
    float target_bank = 0.f;
    if (chasing && cap > 1.0e-8f) {
        target_bank = std::clamp(yaw / cap, -1.f, 1.f) * sim.max_bank[i];
    }
    const float bank_step = std::clamp(target_bank - sim.bank[i], -sim.max_aileron[i], sim.max_aileron[i]);
    sim.bank[i] += bank_step;

    sim.speed[i] = std::min(sim.max_speed[i], sim.speed[i] + sim.max_acc[i]);
    float face_x = 0.f;
    float face_z = 0.f;
    direction(sim.heading[i], face_x, face_z);
    sim.x[i] += face_x * sim.speed[i];
    sim.z[i] += face_z * sim.speed[i];

    const float ground = terrain_height_at(terrain, sim.x[i], sim.z[i]);
    const float surface = ground < 0.f ? 0.f : ground;
    const float target_y = surface + sim.cruise[i];
    float climb = 1.0e6f;
    if (sim.max_pitch[i] > 0.f) {
        climb = std::max(sim.speed[i], 0.01f) * std::tan(sim.max_pitch[i]);
    }
    const float y_before = sim.y[i];
    sim.y[i] += std::clamp(target_y - sim.y[i], -climb, climb);
    sim.vy[i] = sim.y[i] - y_before;

    if (!chasing) {
        return;
    }
    const size_t cursor = static_cast<size_t>(sim.path_at[i]);
    const float left = sim.path_x[i][cursor] - sim.x[i];
    const float forward = sim.path_z[i][cursor] - sim.z[i];
    if (left * left + forward * forward <= kArrive * kArrive) {
        sim.has_goal[i] = 0;
        sim.path_x[i].clear();
        sim.path_z[i].clear();
        sim.path_at[i] = 0;
    }
}

void step_one(int index, void* user) {
    auto* context = static_cast<StepContext*>(user);
    UnitSim& sim = *context->sim;
    const TerrainMesh& terrain = *context->terrain;
    const size_t unit = static_cast<size_t>(index);
    if (unit < sim.alive.size() && sim.alive[unit] == 0) {
        return;
    }
    if (!sim.can_move[unit]) {
        sim.speed[unit] = 0;
        return;
    }
    if (sim.airplane[unit]) {
        step_airplane(sim, unit, terrain);
        return;
    }
    if (!sim.has_goal[unit]) {
        sim.speed[unit] = 0;
        return;
    }

    const size_t i = static_cast<size_t>(index);
    int cursor = sim.path_at[i];
    if (cursor < 0 || cursor >= static_cast<int>(sim.path_x[i].size())) {
        sim.has_goal[i] = 0;
        sim.speed[i] = 0;
        return;
    }
    const float tx = sim.path_x[i][static_cast<size_t>(cursor)];
    const float tz = sim.path_z[i][static_cast<size_t>(cursor)];
    float dx = tx - sim.x[i];
    float dz = tz - sim.z[i];
    float dist = std::sqrt(dx * dx + dz * dz);
    const bool last = cursor + 1 >= static_cast<int>(sim.path_x[i].size());
    const float arrive = last ? 2.f : 12.f;
    if (dist <= arrive) {
        if (last) {
            sim.x[i] = tx;
            sim.z[i] = tz;
            place_height(sim, i, terrain_height_at(terrain, sim.x[i], sim.z[i]));
            sim.speed[i] = 0;
            sim.has_goal[i] = 0;
            sim.path_x[i].clear();
            sim.path_z[i].clear();
            return;
        }
        sim.path_at[i] = ++cursor;
        return;
    }

    const float desired = std::atan2(dx, dz);
    const float delta = wrap_pi(desired - sim.heading[i]);
    const float limit = sim.turn_rate[i] > 0.f ? sim.turn_rate[i] : std::fabs(delta);
    const float turn = std::clamp(delta, -limit, limit);
    sim.heading[i] = wrap_pi(sim.heading[i] + turn);

    if (std::fabs(delta) > kTurnInPlace) {
        sim.speed[i] = std::max(0.f, sim.speed[i] - sim.max_dec[i]);
        place_height(sim, i, terrain_height_at(terrain, sim.x[i], sim.z[i]));
        return;
    }

    const float brake = sim.speed[i] * sim.speed[i] / (2.f * std::max(sim.max_dec[i], 0.01f));
    if (dist <= brake + 1.f) {
        sim.speed[i] = std::max(0.f, sim.speed[i] - sim.max_dec[i]);
    } else {
        sim.speed[i] = std::min(sim.max_speed[i], sim.speed[i] + sim.max_acc[i]);
    }
    const float step = std::min(sim.speed[i], dist);
    float face_x = 0;
    float face_z = 0;
    direction(sim.heading[i], face_x, face_z);
    const float next_x = sim.x[i] + face_x * step;
    const float next_z = sim.z[i] + face_z * step;
    const float h0 = terrain_height_at(terrain, sim.x[i], sim.z[i]);
    const float h1 = terrain_height_at(terrain, next_x, next_z);
    if (!sim.gunship[i]) {
        const float left = (sim.hover[i] && h0 < 0.f) ? 0.f : h0;
        const float right = (sim.hover[i] && h1 < 0.f) ? 0.f : h1;
        const float slope = std::atan2(std::fabs(right - left), std::max(step, 0.01f)) * 180.f / kPi;
        const bool too_deep = !sim.hover[i] && h1 < -sim.max_water[i];
        const bool too_shallow = !sim.hover[i] && h1 > -sim.min_water[i];
        if (too_deep || too_shallow || slope > sim.max_slope[i]) {
            sim.speed[i] = 0;
            place_height(sim, i, h0);
            return;
        }
    }
    sim.x[i] = next_x;
    sim.z[i] = next_z;
    place_height(sim, i, h1);
}

}  // namespace

bool move_frame_ok(std::string& error) {
    float dx = 0;
    float dz = 0;
    direction(0.f, dx, dz);
    if (!(std::fabs(dx) < 1e-4f && dz > 0.99f)) {
        error = "heading 0 should face +z";
        return false;
    }
    direction(kPi * 0.5f, dx, dz);
    if (!(dx > 0.99f && std::fabs(dz) < 1e-4f)) {
        error = "a positive heading should face +x";
        return false;
    }
    return true;
}

void sim_clear(UnitSim& sim) { sim = UnitSim{}; }

bool sim_add_unit(UnitSim& sim, const UnitSpawn& spawn, const UnitMotion& motion, float max_slope_deg,
                  float max_water_depth, float min_water_depth, float ground_y, bool hover, float cruise,
                  std::string& error) {
    if (!(max_slope_deg > 0.f) && !(cruise > 0.f)) {
        error = spawn.def_name + " has no walkable slope";
        return false;
    }
    const float surface = ground_y < 0.f ? 0.f : ground_y;
    const bool gunship = motion.can_fly && motion.hover_attack;
    const bool airplane = motion.can_fly && !motion.hover_attack;
    float y = spawn.y;
    if (gunship || airplane) {
        y = surface + cruise;
    } else if (hover) {
        y = surface;
    }
    sim.id.push_back(spawn.id);
    sim.team.push_back(spawn.team);
    sim.def_name.push_back(spawn.def_name);
    sim.x.push_back(spawn.x);
    sim.y.push_back(y);
    sim.z.push_back(spawn.z);
    sim.heading.push_back(0.f);
    sim.speed.push_back(0.f);
    sim.vy.push_back(0.f);
    sim.max_speed.push_back(std::max(0.f, motion.speed_per_sec) / kSimHz);
    const float acc = motion.max_acc > 0.f ? motion.max_acc : std::max(motion.speed_per_sec / kSimHz, 0.01f);
    sim.max_acc.push_back(acc);
    sim.max_dec.push_back(motion.max_dec > 0.f ? motion.max_dec : acc);
    sim.turn_rate.push_back(motion.turn_rate_raw * kTaToRad);
    sim.max_slope.push_back(max_slope_deg);
    sim.max_water.push_back(max_water_depth);
    sim.min_water.push_back(min_water_depth);
    sim.waterline.push_back(motion.waterline);
    sim.cruise.push_back(cruise);
    sim.bank.push_back(0.f);
    sim.max_rudder.push_back(motion.max_rudder);
    sim.max_aileron.push_back(motion.max_aileron);
    sim.max_bank.push_back(motion.max_bank);
    sim.max_pitch.push_back(motion.max_pitch);
    sim.turn_radius.push_back(motion.turn_radius);
    sim.floater.push_back(motion.floater ? 1 : 0);
    sim.hover.push_back(hover ? 1 : 0);
    sim.gunship.push_back(gunship ? 1 : 0);
    sim.airplane.push_back(airplane ? 1 : 0);
    sim.can_move.push_back(motion.can_move ? 1 : 0);
    sim.has_goal.push_back(0);
    sim.alive.push_back(1);
    sim.health.push_back(1.f);
    sim.max_health.push_back(1.f);
    sim.path_x.emplace_back();
    sim.path_z.emplace_back();
    sim.path_at.push_back(0);
    return true;
}

bool sim_order_move(UnitSim& sim, int index, float goal_x, float goal_z, const TerrainMesh& terrain,
                    std::string& error) {
    if (index < 0 || index >= static_cast<int>(sim.x.size())) {
        error = "move order has no unit";
        return false;
    }
    const size_t i = static_cast<size_t>(index);
    if (!sim.can_move[i]) {
        error = sim.def_name[i] + " cannot move";
        return false;
    }
    std::vector<Waypoint> path;
    if (sim.gunship[i] || sim.airplane[i]) {
        // A gunship or an airplane flies over the terrain. The goal is the only waypoint.
        path.push_back(Waypoint{goal_x, goal_z});
    } else if (!build_ground_path(terrain, sim.max_slope[i], sim.max_water[i], sim.min_water[i], sim.x[i], sim.z[i],
                                  goal_x, goal_z, path, error, sim.hover[i] != 0)) {
        return false;
    }
    sim.path_x[i].clear();
    sim.path_z[i].clear();
    for (const Waypoint& point : path) {
        sim.path_x[i].push_back(point.x);
        sim.path_z[i].push_back(point.z);
    }
    sim.path_at[i] = 0;
    sim.has_goal[i] = 1;
    return true;
}

void sim_step(UnitSim& sim, const TerrainMesh& terrain) {
    StepContext context{&sim, &terrain};
    run_range(0, static_cast<int>(sim.x.size()), step_one, &context);
}

bool write_sim_trace(const std::string& path, int frame, const UnitSim& sim, const TerrainMesh& terrain, bool append,
                     std::string& error, const std::vector<TeamTrace>* teams, const std::vector<MexTrace>* mex,
                     const std::vector<GenTrace>* gen, const NanoTrace* nano, const WreckTrace* wreck,
                     const LaserTrace* laser, const CannonTrace* cannon, const BeamTrace* beam,
                     const MissileTrace* missile, const LightningTrace* lightning, const FlameTrace* flame,
                     const TorpedoTrace* torpedo, const BombTrace* bomb, const DgunTrace* dgun, const LosDuel* los,
                     const DeathWrecks* deaths) {
    std::ofstream out(path, append ? std::ios::app : std::ios::trunc);
    if (!out) {
        error = "could not write sim trace " + path;
        return false;
    }
    out << "{\"f\":" << frame << ",\"units\":[";
    bool wrote_unit = false;
    for (size_t i = 0; i < sim.x.size(); ++i) {
        if (i < sim.alive.size() && sim.alive[i] == 0) {
            continue;
        }
        if (wrote_unit) {
            out << ',';
        }
        wrote_unit = true;
        const float ground = terrain_height_at(terrain, sim.x[i], sim.z[i]);
        out << "{\"id\":" << sim.id[i] << ",\"def\":\"" << sim.def_name[i] << "\",\"t\":" << sim.team[i]
            << ",\"x\":" << sim.x[i] << ",\"y\":" << sim.y[i] << ",\"z\":" << sim.z[i] << ",\"gy\":" << ground
            << ",\"spd\":" << sim.speed[i] << ",\"bank\":" << sim.bank[i] << ",\"cmd\":" << (sim.has_goal[i] ? 1 : 0)
            << "}";
    }
    out << "]";
    if (teams != nullptr && !teams->empty()) {
        out << ",\"teams\":[";
        for (size_t i = 0; i < teams->size(); ++i) {
            const TeamTrace& team = (*teams)[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"id\":" << team.id << ",\"m\":" << team.metal << ",\"e\":" << team.energy
                << ",\"ms\":" << team.metal_storage << ",\"es\":" << team.energy_storage
                << ",\"mi\":" << team.metal_income << ",\"ei\":" << team.energy_income << ",\"mx\":" << team.mex_income
                << ",\"eu\":" << team.energy_upkeep << ",\"so\":" << team.solar_income << ",\"wi\":" << team.wind_income
                << "}";
        }
        out << "]";
    }
    if (mex != nullptr && !mex->empty()) {
        out << ",\"mex\":[";
        for (size_t i = 0; i < mex->size(); ++i) {
            const MexTrace& claim = (*mex)[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"def\":\"" << claim.def_name << "\",\"x\":" << claim.x << ",\"z\":" << claim.z
                << ",\"depth\":" << claim.depth << ",\"scale\":" << claim.scale << ",\"raw\":" << claim.raw
                << ",\"rate\":" << claim.rate << ",\"squares\":" << claim.squares << "}";
        }
        out << "]";
    }
    if (gen != nullptr && !gen->empty()) {
        out << ",\"gen\":[";
        for (size_t i = 0; i < gen->size(); ++i) {
            const GenTrace& one = (*gen)[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"def\":\"" << one.def_name << "\",\"x\":" << one.x << ",\"z\":" << one.z
                << ",\"upkeep\":" << one.upkeep << ",\"make\":" << one.production << ",\"cap\":" << one.wind_cap
                << ",\"wind\":" << one.wind << ",\"rate\":" << one.rate << "}";
        }
        out << "]";
    }
    if (nano != nullptr) {
        out << ",\"nf\":[{\"def\":\"" << nano->def_name << "\",\"x\":" << nano->x << ",\"y\":" << nano->y
            << ",\"z\":" << nano->z << ",\"bp\":" << nano->progress << ",\"hp\":" << nano->health
            << ",\"bt\":" << nano->build_time << ",\"mc\":" << nano->metal_cost << ",\"ec\":" << nano->energy_cost
            << ",\"wk\":" << nano->worker_time << ",\"bd\":" << nano->build_distance << ",\"sc\":" << nano->power_scale
            << ",\"fin\":" << nano->finished << ",\"cd\":" << nano->decay << ",\"ct\":" << nano->decay_time
            << ",\"cs\":" << nano->decay_speed << "}]";
    }
    if (wreck != nullptr) {
        out << ",\"rk\":[{\"def\":\"" << wreck->def_name << "\",\"x\":" << wreck->x << ",\"y\":" << wreck->y
            << ",\"z\":" << wreck->z << ",\"left\":" << wreck->left << ",\"mt\":" << wreck->metal
            << ",\"en\":" << wreck->energy << ",\"rt\":" << wreck->reclaim_time << ",\"rs\":" << wreck->reclaim_speed
            << ",\"bd\":" << wreck->build_distance << ",\"sc\":" << wreck->power_scale
            << ",\"rm\":" << wreck->reclaim_method << ",\"ef\":" << wreck->energy_factor
            << ",\"mr\":" << wreck->multi_reclaim << ",\"gone\":" << wreck->gone << "}]";
    }
    if (laser != nullptr) {
        out << ",\"ls\":[{\"def\":\"" << laser->def_name << "\",\"td\":\"" << laser->target_def << "\",\"hp\":"
            << laser->health << ",\"die\":" << laser->die << ",\"hits\":" << laser->hits << ",\"shots\":" << laser->shots
            << ",\"gnd\":" << laser->ground << ",\"dmg\":" << laser->damage << ",\"spd\":" << laser->speed
            << ",\"rng\":" << laser->range << ",\"rel\":" << laser->reload_frames << "}]";
    }
    if (cannon != nullptr) {
        out << ",\"cn\":[{\"def\":\"" << cannon->def_name << "\",\"td\":\"" << cannon->target_def << "\",\"hp\":"
            << cannon->health << ",\"die\":" << cannon->die << ",\"hits\":" << cannon->hits << ",\"shots\":" << cannon->shots
            << ",\"gnd\":" << cannon->ground << ",\"dmg\":" << cannon->damage << ",\"spd\":" << cannon->speed
            << ",\"rng\":" << cannon->range << ",\"rel\":" << cannon->reload_frames << ",\"grv\":" << cannon->gravity
            << ",\"hmod\":" << cannon->height_mod << "}]";
    }
    if (beam != nullptr) {
        out << ",\"bm\":[{\"def\":\"" << beam->def_name << "\",\"td\":\"" << beam->target_def << "\",\"hp\":"
            << beam->health << ",\"die\":" << beam->die << ",\"hits\":" << beam->hits << ",\"shots\":" << beam->shots
            << ",\"gnd\":" << beam->ground << ",\"dmg\":" << beam->damage << ",\"rng\":" << beam->range
            << ",\"rel\":" << beam->reload_frames << ",\"btm\":" << beam->beamtime << ",\"sal\":" << beam->salvo_size
            << ",\"inten\":" << beam->intensity << "}]";
    }
    if (missile != nullptr) {
        out << ",\"ms\":[{\"def\":\"" << missile->def_name << "\",\"td\":\"" << missile->target_def << "\",\"hp\":"
            << missile->health << ",\"die\":" << missile->die << ",\"hits\":" << missile->hits
            << ",\"shots\":" << missile->shots << ",\"gnd\":" << missile->ground << ",\"dmg\":" << missile->damage
            << ",\"spd\":" << missile->speed << ",\"mxv\":" << missile->max_speed << ",\"acc\":" << missile->accel
            << ",\"rng\":" << missile->range << ",\"rel\":" << missile->reload_frames << ",\"hmod\":"
            << missile->height_mod << "}]";
    }
    if (lightning != nullptr) {
        out << ",\"ln\":[{\"def\":\"" << lightning->def_name << "\",\"td\":\"" << lightning->target_def << "\",\"hp\":"
            << lightning->health << ",\"die\":" << lightning->die << ",\"hits\":" << lightning->hits
            << ",\"shots\":" << lightning->shots << ",\"gnd\":" << lightning->ground << ",\"dmg\":" << lightning->damage
            << ",\"rng\":" << lightning->range << ",\"rel\":" << lightning->reload_frames << ",\"brst\":"
            << lightning->burst << ",\"gap\":" << lightning->salvo_delay << ",\"cost\":" << lightning->energy_cost
            << ",\"nrg\":" << lightning->energy << ",\"wpn\":" << lightning->waterweapon << "}]";
    }
    if (flame != nullptr) {
        out << ",\"fl\":[{\"def\":\"" << flame->def_name << "\",\"td\":\"" << flame->target_def << "\",\"hp\":"
            << flame->health << ",\"die\":" << flame->die << ",\"hits\":" << flame->hits << ",\"shots\":" << flame->shots
            << ",\"gnd\":" << flame->ground << ",\"dmg\":" << flame->damage << ",\"rng\":" << flame->range
            << ",\"rel\":" << flame->reload_frames << ",\"brst\":" << flame->burst << ",\"gap\":" << flame->salvo_delay
            << ",\"spd\":" << flame->speed << ",\"ttl\":" << flame->ttl << ",\"wpn\":" << flame->waterweapon << "}]";
    }
    if (torpedo != nullptr) {
        out << ",\"tp\":[{\"def\":\"" << torpedo->def_name << "\",\"td\":\"" << torpedo->target_def << "\",\"hp\":"
            << torpedo->health << ",\"die\":" << torpedo->die << ",\"hits\":" << torpedo->hits
            << ",\"shots\":" << torpedo->shots << ",\"gnd\":" << torpedo->ground << ",\"dmg\":" << torpedo->damage
            << ",\"rng\":" << torpedo->range << ",\"rel\":" << torpedo->reload_frames << ",\"spd\":" << torpedo->speed
            << ",\"mxv\":" << torpedo->max_speed << ",\"ttl\":" << torpedo->ttl << ",\"grv\":" << torpedo->gravity
            << ",\"wpn\":" << torpedo->waterweapon << "}]";
    }
    if (bomb != nullptr) {
        out << ",\"bb\":[{\"def\":\"" << bomb->def_name << "\",\"td\":\"" << bomb->target_def << "\",\"hp\":"
            << bomb->health << ",\"die\":" << bomb->die << ",\"hits\":" << bomb->hits << ",\"shots\":" << bomb->shots
            << ",\"gnd\":" << bomb->ground << ",\"dmg\":" << bomb->damage << ",\"rng\":" << bomb->range
            << ",\"rel\":" << bomb->reload_frames << ",\"brst\":" << bomb->burst << ",\"gap\":" << bomb->salvo_delay
            << ",\"spd\":" << bomb->speed << ",\"acc\":" << bomb->accel << ",\"cru\":" << bomb->cruise
            << ",\"pit\":" << bomb->pitch << ",\"ttl\":" << bomb->ttl << ",\"grv\":" << bomb->gravity
            << ",\"wpn\":" << bomb->waterweapon << ",\"gz\":" << bomb->height_z0 << ",\"gs\":" << bomb->height_step
            << ",\"gh\":[";
        const std::streamsize saved_precision = out.precision();
        out.precision(std::numeric_limits<float>::max_digits10);
        for (size_t i = 0; i < bomb->heights.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << bomb->heights[i];
        }
        out.precision(saved_precision);
        out << "]}]";
    }
    if (dgun != nullptr) {
        out << ",\"dg\":[{\"def\":\"" << dgun->def_name << "\",\"td\":\"" << dgun->target_def << "\",\"hp\":"
            << dgun->health << ",\"die\":" << dgun->die << ",\"hits\":" << dgun->hits << ",\"shots\":" << dgun->shots
            << ",\"gnd\":" << dgun->ground << ",\"dmg\":" << dgun->damage << ",\"rng\":" << dgun->range
            << ",\"rel\":" << dgun->reload_frames << ",\"spd\":" << dgun->speed << ",\"grv\":" << dgun->gravity
            << "}]";
    }
    if (los != nullptr && !los->checks.empty()) {
        out << ",\"lo\":[";
        for (size_t i = 0; i < los->checks.size(); ++i) {
            const LosCheck& check = los->checks[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"tag\":\"" << check.tag << "\",\"ad\":\"" << check.attacker_def << "\",\"td\":\""
                << check.target_def << "\",\"ai\":" << check.attacker << ",\"ti\":" << check.target
                << ",\"see\":" << check.see << ",\"far\":" << check.far << ",\"wpn\":" << check.weapon
                << ",\"dist\":" << check.dist << ",\"sight\":" << check.sight << ",\"sq\":" << check.square
                << ",\"al\":" << check.attacker_lift << ",\"tl\":" << check.target_lift << ",\"gh\":[";
            const std::streamsize saved_precision = out.precision();
            out.precision(std::numeric_limits<float>::max_digits10);
            for (size_t h = 0; h < check.heights.size(); ++h) {
                if (h != 0) {
                    out << ',';
                }
                out << check.heights[h];
            }
            out.precision(saved_precision);
            out << "]}";
        }
        out << "]";
    }
    if (deaths != nullptr && !deaths->items.empty()) {
        out << ",\"dw\":[";
        for (size_t i = 0; i < deaths->items.size(); ++i) {
            const DeathWreck& wreck = deaths->items[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"def\":\"" << wreck.def_name << "\",\"id\":" << wreck.unit << ",\"born\":" << wreck.born
                << ",\"x\":" << wreck.x << ",\"y\":" << wreck.y << ",\"z\":" << wreck.z << ",\"mt\":" << wreck.metal
                << ",\"cost\":" << wreck.metal_cost << ",\"ratio\":" << wreck.ratio << "}";
        }
        out << "]";
    }
    out << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar