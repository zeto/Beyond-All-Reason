#pragma once

#include "gl/draw_list.h"

#include <string>

struct lua_State;

namespace bar {

// Installs the gl table. Widgets keep calling gl.*; the host records a command list.
void gl_install(lua_State* state);

void gl_set_root(const std::string& game_root);
void gl_set_view(int width, int height);
int gl_view_width();
int gl_view_height();

// Drops the previous screen list. Display lists and render targets stay.
void gl_begin_draw();

// Copies the screen list into a stable snapshot the renderer can read.
const HudList& gl_publish();

}  // namespace bar
