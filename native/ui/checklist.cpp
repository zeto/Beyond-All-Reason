#include "checklist.h"

#include "unitdef.h"
#include "unit_spawn.h"

#include <fstream>
#include <iostream>

namespace bar {
namespace {

ChecklistState g_check;

}  // namespace

void checklist_reset() { g_check = ChecklistState{}; }

ChecklistState& checklist_state() { return g_check; }

void checklist_set_group(int slot, const std::vector<int>& ids) {
    if (slot < 1 || slot > 9) {
        return;
    }
    g_check.groups[static_cast<size_t>(slot)] = ids;
    if (!ids.empty()) {
        g_check.control_groups = true;
    }
}

bool checklist_recall_group(int slot, std::vector<int>& ids) {
    if (slot < 1 || slot > 9) {
        return false;
    }
    ids = g_check.groups[static_cast<size_t>(slot)];
    if (!ids.empty()) {
        g_check.control_groups = true;
    }
    return !ids.empty();
}

void checklist_set_paused(bool paused) {
    g_check.paused = paused;
    g_check.pause_ok = true;
}

void checklist_set_gamespeed(int speed) {
    if (speed < 1) {
        speed = 1;
    }
    if (speed > 4) {
        speed = 4;
    }
    g_check.gamespeed = speed;
    g_check.gamespeed_ok = true;
}

void checklist_note_click_select() { g_check.click_select = true; }
void checklist_note_drag_box() { g_check.drag_box = true; }

void checklist_note_shift_queue(int waypoints) {
    g_check.queued_waypoints = waypoints;
    if (waypoints >= 2) {
        g_check.shift_queue = true;
    }
}

void checklist_note_attack(int target_id) {
    g_check.last_attack_target = target_id;
    g_check.attack = target_id > 0;
}

void checklist_note_facing(int facing) {
    g_check.last_facing = facing & 3;
    g_check.facing = true;
}

void checklist_note_reclaim(int unit_or_feature) {
    g_check.last_reclaim_unit = unit_or_feature;
    g_check.reclaim = unit_or_feature != 0;
}

void checklist_note_repair(int unit_id) {
    g_check.last_repair_unit = unit_id;
    g_check.repair = unit_id > 0;
}


bool checklist_spawn_unit(UnitSim& sim, const TerrainMesh& terrain, const std::string& game_root,
                          const std::string& def_name, float x, float z, int team, int& out_id,
                          std::string& error) {
    UnitMotion motion;
    if (!load_unit_motion(game_root, def_name, motion, error)) {
        return false;
    }
    float slope = 20.f;
    float water = 35.f;
    float min_water = -1.0e6f;
    bool hover = false;
    if (!motion.move_class.empty()) {
        MoveClass move_class;
        if (!load_move_class(game_root, motion.move_class, move_class, error)) {
            return false;
        }
        slope = move_class.max_slope_deg;
        water = move_class.max_water_depth;
        min_water = move_class.min_water_depth;
        hover = move_class.hover;
    }
    int next_id = 1;
    for (int id : sim.id) {
        if (id >= next_id) {
            next_id = id + 1;
        }
    }
    UnitSpawn spawn;
    spawn.id = next_id;
    spawn.team = team;
    spawn.def_name = def_name;
    spawn.x = x;
    spawn.z = z;
    spawn.y = terrain_height_at(terrain, x, z);
    const float cruise = motion.can_fly ? motion.cruise_altitude : 0.f;
    if (!sim_add_unit(sim, spawn, motion, slope, water, min_water, spawn.y, hover, cruise, error)) {
        return false;
    }
    float hp = 0.f;
    std::string hp_error;
    const size_t at = sim.id.size() - 1;
    if (load_unit_health(game_root, def_name, hp, hp_error) && hp > 0.f) {
        sim.health[at] = hp;
        sim.max_health[at] = hp;
    }
    out_id = next_id;
    std::cout << "checklist spawn " << def_name << " id " << next_id << " team " << team << " at " << x << " " << z
              << "\n";
    return true;
}

bool write_checklist_report(const std::string& path, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    const ChecklistState& c = g_check;
    out << "{\"click_select\":" << (c.click_select ? "true" : "false")
        << ",\"drag_box\":" << (c.drag_box ? "true" : "false")
        << ",\"shift_queue\":" << (c.shift_queue ? "true" : "false")
        << ",\"attack\":" << (c.attack ? "true" : "false")
        << ",\"facing\":" << (c.facing ? "true" : "false")
        << ",\"control_groups\":" << (c.control_groups ? "true" : "false")
        << ",\"pause\":" << (c.pause_ok ? "true" : "false")
        << ",\"gamespeed\":" << (c.gamespeed_ok ? "true" : "false")
        << ",\"reclaim\":" << (c.reclaim ? "true" : "false")
        << ",\"repair\":" << (c.repair ? "true" : "false")
        << ",\"queued_waypoints\":" << c.queued_waypoints
        << ",\"last_attack_target\":" << c.last_attack_target
        << ",\"last_facing\":" << c.last_facing
        << ",\"gamespeed_value\":" << c.gamespeed
        << ",\"paused\":" << (c.paused ? "true" : "false") << "}\n";
    return true;
}

}  // namespace bar
