#include "build/reclaim.h"

#include "build/nanoframe.h"
#include "unitdef.h"

#include <algorithm>
#include <cmath>

namespace bar {
namespace {

constexpr float kSimHz = 30.f;
constexpr float kMinBuildDistance = 38.f;

int chunk_number(float left, float method) { return static_cast<int>(std::ceil(left * method)); }

}  // namespace

float wreck_metal_amount(const UnitWreck& corpse, float ratio) {
    float metal = corpse.metal;
    if (!corpse.commander && !corpse.effigy && corpse.has_health && corpse.health != 0.f && corpse.has_metal_cost &&
        corpse.has_energy_cost) {
        const double raw = static_cast<double>(corpse.metal_cost) * static_cast<double>(ratio);
        metal = static_cast<float>(std::floor(raw));
    }
    return metal;
}

bool reclaim_begin(Wreck& wreck, const std::string& game_root, const ConstructionRules& rules, int builder,
                   const std::string& builder_def, float builder_x, float builder_y, float builder_z,
                   const std::string& unit_def, float wreck_x, float wreck_z, const TerrainMesh& terrain,
                   std::string& error) {
    if (wreck.active) {
        error = "a wreck is already being reclaimed";
        return false;
    }
    UnitReclaim maker;
    if (!load_unit_reclaim(game_root, builder_def, maker, error)) {
        return false;
    }
    if (!maker.can_reclaim) {
        error = builder_def + " cannot reclaim";
        return false;
    }
    UnitWreck corpse;
    if (!load_unit_wreck(game_root, unit_def, corpse, error)) {
        return false;
    }
    if (!corpse.present) {
        error = unit_def + " has no wreck";
        return false;
    }
    if (!corpse.reclaimable) {
        error = unit_def + " wreck is not reclaimable";
        return false;
    }
    const float metal = wreck_metal_amount(corpse, rules.wreck_metal_ratio);
    const float energy = corpse.energy;
    float reclaim_time = 0.f;
    if (corpse.has_reclaim_time) {
        reclaim_time = std::max(1.f, corpse.reclaim_time);
    } else {
        reclaim_time = std::max(1.f, (metal + energy) * 6.f);
    }
    const float speed = maker.explicit_speed ? maker.reclaim_speed : maker.reclaim_speed * rules.build_power_scale;
    const float reach = std::max(kMinBuildDistance, maker.build_distance * rules.build_distance_scale);
    const float y = terrain_height_at(terrain, wreck_x, wreck_z);
    const float dx = builder_x - wreck_x;
    const float dy = builder_y - y;
    const float dz = builder_z - wreck_z;
    // Radius 0. A real wreck radius would only make this check looser.
    if (dx * dx + dy * dy + dz * dz >= reach * reach) {
        error = "wreck is out of range";
        return false;
    }
    wreck = Wreck{};
    wreck.active = true;
    wreck.builder = builder;
    wreck.def_name = unit_def;
    wreck.x = wreck_x;
    wreck.y = y;
    wreck.z = wreck_z;
    wreck.left = 1.f;
    wreck.metal = metal;
    wreck.energy = energy;
    wreck.remain_metal = metal;
    wreck.remain_energy = energy;
    wreck.reclaim_time = reclaim_time;
    wreck.reclaim_speed = speed;
    wreck.build_distance = reach;
    wreck.power_per_frame = speed / kSimHz;
    wreck.power_scale = rules.build_power_scale;
    wreck.reclaim_method = rules.reclaim_method;
    wreck.energy_factor = rules.feature_energy_cost_factor;
    wreck.multi_reclaim = rules.multi_reclaim;
    return true;
}

void reclaim_step(Wreck& wreck, TeamResources& team, float builder_x, float builder_y, float builder_z) {
    if (!wreck.active || wreck.gone || !(wreck.left > 0.f)) {
        return;
    }
    const float dx = builder_x - wreck.x;
    const float dy = builder_y - wreck.y;
    const float dz = builder_z - wreck.z;
    if (dx * dx + dy * dy + dz * dz >= wreck.build_distance * wreck.build_distance) {
        return;
    }
    const float step = wreck.power_per_frame / wreck.reclaim_time;
    const float temp = std::max(0.f, wreck.left - step);
    const float fraction = wreck.left - temp;
    const float frac_metal = std::min(wreck.metal * fraction, wreck.remain_metal);
    const float frac_energy = std::min(wreck.energy * fraction, wreck.remain_energy);
    // A float cost assigned to both resources. Zero leaves the stocks unchanged.
    const float cost = frac_metal * wreck.energy_factor;
    if (team.metal < cost || team.energy < cost) {
        return;
    }
    float add_metal = 0.f;
    float add_energy = 0.f;
    if (temp == 0.f) {
        add_metal = wreck.remain_metal;
        add_energy = wreck.remain_energy;
    } else if (wreck.reclaim_method == 0.f) {
        add_metal = frac_metal;
        add_energy = frac_energy;
    } else if (wreck.reclaim_method == 1.f) {
        // The whole refund waits for the last step.
    } else {
        const float chunk = 1.f / wreck.reclaim_method;
        const int crossed = chunk_number(wreck.left, wreck.reclaim_method) - chunk_number(temp, wreck.reclaim_method);
        if (crossed > 0) {
            add_metal = std::min(wreck.metal * (static_cast<float>(crossed) * chunk), wreck.remain_metal);
            add_energy = std::min(wreck.energy * (static_cast<float>(crossed) * chunk), wreck.remain_energy);
        }
    }
    team.metal = std::clamp(team.metal - cost, 0.f, team.metal_storage);
    team.energy = std::clamp(team.energy - cost, 0.f, team.energy_storage);
    team.metal = std::clamp(team.metal + add_metal, 0.f, team.metal_storage);
    team.energy = std::clamp(team.energy + add_energy, 0.f, team.energy_storage);
    wreck.remain_metal -= add_metal;
    wreck.remain_energy -= add_energy;
    wreck.left = temp;
    if (!(wreck.left > 0.f)) {
        wreck.left = 0.f;
        wreck.gone = true;
    }
}

WreckTrace wreck_trace(const Wreck& wreck) {
    WreckTrace trace;
    trace.def_name = wreck.def_name;
    trace.x = wreck.x;
    trace.y = wreck.y;
    trace.z = wreck.z;
    trace.left = wreck.left;
    trace.metal = wreck.metal;
    trace.energy = wreck.energy;
    trace.reclaim_time = wreck.reclaim_time;
    trace.reclaim_speed = wreck.reclaim_speed;
    trace.build_distance = wreck.build_distance;
    trace.power_scale = wreck.power_scale;
    trace.reclaim_method = wreck.reclaim_method;
    trace.energy_factor = wreck.energy_factor;
    trace.multi_reclaim = wreck.multi_reclaim;
    trace.gone = wreck.gone ? 1 : 0;
    return trace;
}

}  // namespace bar
