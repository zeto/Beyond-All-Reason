#pragma once

#include "gl/draw_list.h"
#include "move/unit_sim.h"
#include "smf.h"

#include <string>
#include <vector>

struct lua_State;

namespace bar {

struct WidgetEnv {
    std::string game_root;
    std::string map_name;
    float min_height = 0;
    float max_height = 0;
    float min_wind = 0;
    float max_wind = 0;
    float tidal = 0;
    int view_w = 1280;
    int view_h = 720;
};

bool widgets_load(lua_State* state, const WidgetEnv& env, std::string& error);

bool widgets_frame(int frame, std::string& error);

void widgets_set_view(int width, int height);

void widgets_set_pointer(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift);

bool widgets_mouse_press(float x, float y, int button, std::string& error);

// Notify stock widgets that the selection changed (Spring unit ids).
bool widgets_selection_changed(const std::vector<int>& unit_ids, std::string& error);

// Bind the live sim for GetUnitCmdDescs / build placement.
void widgets_set_sim(UnitSim* sim, const TerrainMesh* terrain);

// Projected view matrix for the footprint preview (16 floats). Null clears it.
void widgets_set_view_proj(const float* view_proj16);

const HudList* hud_list();

}  // namespace bar
