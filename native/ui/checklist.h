#pragma once

#include "move/unit_sim.h"
#include "smf.h"

#include <array>
#include <string>
#include <vector>

namespace bar {

// Player checklist state for M7 plan exit (select/queue/groups/pause/speed/orders).
struct ChecklistState {
    std::array<std::vector<int>, 10> groups;  // 1..9 used
    bool paused = false;
    int gamespeed = 1;  // 1..4
    bool click_select = false;
    bool drag_box = false;
    bool shift_queue = false;
    bool attack = false;
    bool facing = false;
    bool control_groups = false;
    bool pause_ok = false;
    bool gamespeed_ok = false;
    bool reclaim = false;
    bool repair = false;
    int last_attack_target = -1;
    int last_facing = 0;
    int last_reclaim_unit = -1;
    int last_repair_unit = -1;
    int queued_waypoints = 0;
};

void checklist_reset();
ChecklistState& checklist_state();

void checklist_set_group(int slot /*1..9*/, const std::vector<int>& ids);
bool checklist_recall_group(int slot, std::vector<int>& ids);
void checklist_set_paused(bool paused);
void checklist_set_gamespeed(int speed);
void checklist_note_click_select();
void checklist_note_drag_box();
void checklist_note_shift_queue(int waypoints);
void checklist_note_attack(int target_id);
void checklist_note_facing(int facing);
void checklist_note_reclaim(int unit_or_feature);
void checklist_note_repair(int unit_id);

bool checklist_spawn_unit(UnitSim& sim, const TerrainMesh& terrain, const std::string& game_root,
                         const std::string& def_name, float x, float z, int team, int& out_id,
                         std::string& error);

bool write_checklist_report(const std::string& path, std::string& error);

}  // namespace bar
