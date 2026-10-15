#pragma once

#include "gl/draw_list.h"

#include <string>

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

// Loads FlowUI, the font handler, the top bar, and the selection box into the
// gadget Lua state. A missing call fails the load. The widget bodies are the stock files.
bool widgets_load(lua_State* state, const WidgetEnv& env, std::string& error);

// Runs the top bar and the selection box for one sim frame and publishes the draw list.
bool widgets_frame(int frame, std::string& error);

void widgets_set_view(int width, int height);

// Pointer sample in view points with y up from the bottom.
void widgets_set_pointer(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift);

// Forwards a left mouse press to the selectionbox widget (view points, y up).
bool widgets_mouse_press(float x, float y, int button, std::string& error);

const HudList* hud_list();

}  // namespace bar
