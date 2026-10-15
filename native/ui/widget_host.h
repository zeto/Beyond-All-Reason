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

// Loads FlowUI, the font handler, and the top bar into the gadget Lua state.
// A missing call fails the load. The widget bodies are the stock files.
bool widgets_load(lua_State* state, const WidgetEnv& env, std::string& error);

// Runs the top bar for one sim frame and publishes its draw list.
bool widgets_frame(int frame, std::string& error);

void widgets_set_view(int width, int height);

const HudList* hud_list();

}  // namespace bar
