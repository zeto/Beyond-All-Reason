#pragma once

#include "gl/draw_list.h"
#include "move/unit_sim.h"

#include <string>
#include <vector>

namespace bar {

struct SelectionMark {
    int unit_id = 0;
    int sim_index = 0;
    float screen_x = 0;
    float screen_y = 0;
    float radius = 24;
};

struct SelectionSnapshot {
    std::vector<int> selected;
    std::string cursor = "cursornormal";
    int active_cmd = 0;
    bool boxing = false;
    float box_x1 = 0;
    float box_y1 = 0;
    float box_x2 = 0;
    float box_y2 = 0;
};

// Scripted pointer / order events. Frames match the shell presented counter.
enum class SelectEventKind {
    Box,
    Click,       // left-click select (no drag)
    MoveClick,   // right-click move
    ShiftMove,   // shift-queue move waypoint
    Attack,      // right-click attack target unit id
    Facing,      // cycle build facing 0..3 (or set absolute)
    GroupSet,    // ctrl+N store selection
    GroupRecall, // N recall group
    Pause,
    GameSpeed,   // 1..4
    Reclaim,     // reclaim at world xz (wreck def name optional in report)
    Repair,      // repair target unit id
    Spawn,       // spawn def x z team (sim only; mesh still unit 0)
};

struct SelectEvent {
    int frame = 0;
    SelectEventKind kind = SelectEventKind::Box;
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float world_x = 0;
    float world_z = 0;
    int target_id = 0;
    int slot = 0;
    int facing = -1;  // -1 = cycle, else absolute 0..3
    int speed = 1;
    bool pause = true;
    std::string def_name;
    int team = 1;
};

void selection_reset();

void selection_set_view(int width, int height);

void selection_set_mouse(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift);

void selection_set_marks(const std::vector<SelectionMark>& marks);

void selection_left_down(float x, float y);
void selection_left_drag(float x, float y);
void selection_left_up(float x, float y);

bool selection_issue_move(UnitSim& sim, float goal_x, float goal_z, const TerrainMesh& terrain, std::string& error,
                          bool shift_queue = false);

bool selection_issue_attack(UnitSim& sim, int target_unit_id, std::string& error);

bool selection_issue_reclaim(UnitSim& sim, float world_x, float world_z, std::string& error);

bool selection_issue_repair(UnitSim& sim, int target_unit_id, std::string& error);

void selection_set_cursor(const std::string& name);
void selection_refresh_cursor();

const SelectionSnapshot& selection_snapshot();

bool selection_get_box(float& x1, float& y1, float& x2, float& y2);

void selection_append_highlights(HudList& hud);

bool load_select_events(const std::string& path, std::vector<SelectEvent>& events, std::string& error);

bool apply_select_events(UnitSim& sim, const std::vector<SelectEvent>& events, int presented,
                         const TerrainMesh& terrain, const std::string& game_root, std::string& error);

bool write_selection_report(const std::string& path, int frame, std::string& error);

}  // namespace bar
