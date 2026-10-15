#pragma once

#include "gl/draw_list.h"
#include "move/unit_sim.h"

#include <string>
#include <vector>

namespace bar {

// Screen-space mark for one living unit. y grows up from the view bottom.
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

// One scripted pointer event. Frames match the shell's presented counter.
enum class SelectEventKind { Box, MoveClick };

struct SelectEvent {
    int frame = 0;
    SelectEventKind kind = SelectEventKind::Box;
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float world_x = 0;
    float world_z = 0;
};

void selection_reset();

void selection_set_view(int width, int height);

void selection_set_mouse(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift);

void selection_set_marks(const std::vector<SelectionMark>& marks);

// Left press / drag / release in view points (y up). A release finalizes the box.
void selection_left_down(float x, float y);
void selection_left_drag(float x, float y);
void selection_left_up(float x, float y);

// Right-click move target in world xz. Returns false when nothing is selected.
bool selection_issue_move(UnitSim& sim, float goal_x, float goal_z, const TerrainMesh& terrain, std::string& error);

void selection_set_cursor(const std::string& name);
void selection_refresh_cursor();

const SelectionSnapshot& selection_snapshot();

bool selection_get_box(float& x1, float& y1, float& x2, float& y2);

// Append green selection rings for the current marks into a published HUD list.
void selection_append_highlights(HudList& hud);

bool load_select_events(const std::string& path, std::vector<SelectEvent>& events, std::string& error);

// Apply scripted box / move events for this presented frame.
bool apply_select_events(UnitSim& sim, const std::vector<SelectEvent>& events, int presented,
                         const TerrainMesh& terrain, std::string& error);

bool write_selection_report(const std::string& path, int frame, std::string& error);

}  // namespace bar
