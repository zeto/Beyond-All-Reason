#include "widget_host.h"
#include "autocolors.h"

#include "gl/gl_api.h"
#include "selection.h"
#include "build_cmd.h"
#include "minimap.h"
#include "hud_status.h"
#include "camera.h"
#include "gadget_host.h"
#include "unitdef.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>

namespace bar {
namespace {

lua_State* g_state = nullptr;
bool g_loaded = false;
WidgetEnv g_env;
int g_applied_w = 0;
int g_applied_h = 0;
int g_next_w = 0;
int g_next_h = 0;
HudList g_hud;
std::string g_current_widget;
bool g_select_draw = false;
bool g_select_update = false;
std::string g_cmd_colors;
float g_mouse_x = -1;
float g_mouse_y = -1;
bool g_mouse_left = false;
bool g_mouse_middle = false;
bool g_mouse_right = false;
bool g_mod_alt = false;
bool g_mod_ctrl = false;
bool g_mod_meta = false;
bool g_mod_shift = false;
int g_frame = 0;
UnitSim* g_sim = nullptr;
const TerrainMesh* g_terrain = nullptr;
Camera* g_camera = nullptr;
bool g_minimap_draw = true;
bool g_minimap_update = true;
float g_view_proj[16] = {};
bool g_have_view_proj = false;
bool g_grid_draw = true;
bool g_grid_update = true;
bool g_volume_draw = true;
bool g_cursor_update = false;
bool g_screenmode_draw = true;
bool g_order_draw = true;
bool g_order_update = true;
bool g_info_draw = false;
bool g_info_update = false;
// Stock widgets hosted through one generic path: load, Initialize, ViewResize,
// Update, DrawScreen, with UpdateCallIn/RemoveCallIn honoured per widget.
struct ExtraWidget {
    const char* key;
    const char* file;
    bool loaded = false;
    bool draw = true;
    bool update = true;
};
ExtraWidget g_extra[] = {
    {"bar_ui_topbuttons", "gui_top_bar_buttons.lua"},
    {"bar_ui_idle", "gui_idle_builders.lua"},
    {"bar_ui_chat", "gui_chat.lua"},
    {"bar_ui_messages", "gui_messages.lua"},
    {"bar_ui_notifications", "snd_notifications.lua"},
};
// Recoil sends every Spring.Echo line to the console, and widgets see it through
// widget:AddConsoleLine(line, priority). Lines queue here and are delivered each frame.
std::vector<std::string> g_console_lines;
ExtraWidget* extra_current();
bool g_buildmenu_draw = false;
bool g_buildmenu_update = false;


int lua_error_handler(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    lua_getglobal(state, "debug");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_pushstring(state, message != nullptr ? message : "lua error");
        return 1;
    }
    lua_getfield(state, -1, "traceback");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        lua_pushstring(state, message != nullptr ? message : "lua error");
        return 1;
    }
    lua_pushstring(state, message != nullptr ? message : "lua error");
    lua_pushinteger(state, 2);
    lua_call(state, 2, 1);
    return 1;
}

// errfunc stays on the stack. Success leaves nresults above it; the caller drops the handler.
bool pcall_keep(lua_State* state, int nargs, int nresults, std::string& error) {
    const int function_index = lua_gettop(state) - nargs;
    lua_pushcfunction(state, lua_error_handler);
    lua_insert(state, function_index);
    if (lua_pcall(state, nargs, nresults, function_index) != 0) {
        const char* message = lua_tostring(state, -1);
        error = message != nullptr ? message : "lua call failed";
        lua_pop(state, 2);
        return false;
    }
    lua_remove(state, function_index);
    return true;
}

void set_spring(lua_State* state, const char* name, lua_CFunction function) {
    lua_getglobal(state, "Spring");
    lua_pushcfunction(state, function);
    lua_setfield(state, -2, name);
    lua_pop(state, 1);
}

void set_vfs(lua_State* state, const char* name, lua_CFunction function) {
    lua_getglobal(state, "VFS");
    lua_pushcfunction(state, function);
    lua_setfield(state, -2, name);
    lua_pop(state, 1);
}

bool safe_relative(const std::string& path) {
    return !path.empty() && path.front() != '/' && path.find("..") == std::string::npos;
}

std::filesystem::path rooted(const char* relative) {
    return std::filesystem::path(g_env.game_root) / relative;
}

int ui_config_number(lua_State* state) {
    if (lua_isnumber(state, 2) != 0) {
        lua_pushvalue(state, 2);
    } else {
        lua_pushnumber(state, 0);
    }
    return 1;
}

int ui_config_string(lua_State* state) {
    if (lua_isstring(state, 2) != 0) {
        lua_pushvalue(state, 2);
    } else {
        lua_pushstring(state, "");
    }
    return 1;
}

int ui_view_geometry(lua_State* state) {
    lua_pushinteger(state, gl_view_width());
    lua_pushinteger(state, gl_view_height());
    lua_pushinteger(state, 0);  // viewPosX
    lua_pushinteger(state, 0);  // viewPosY
    return 4;
}

int ui_ground_extremes(lua_State* state) {
    lua_pushnumber(state, g_env.min_height);
    lua_pushnumber(state, g_env.max_height);
    lua_pushnumber(state, g_env.min_height);
    lua_pushnumber(state, g_env.max_height);
    return 4;
}

int ui_tidal(lua_State* state) {
    lua_pushnumber(state, g_env.tidal);
    return 1;
}

int ui_local_team(lua_State* state) {
    (void)state;
    lua_pushnumber(state, 0);
    return 1;
}

int ui_local_ally(lua_State* state) {
    (void)state;
    lua_pushnumber(state, 0);
    return 1;
}

int ui_spectating(lua_State* state) {
    lua_pushboolean(state, 0);
    lua_pushboolean(state, 0);
    return 2;
}

int ui_mouse_state(lua_State* state) {
    lua_pushnumber(state, g_mouse_x);
    lua_pushnumber(state, g_mouse_y);
    lua_pushboolean(state, g_mouse_left ? 1 : 0);
    lua_pushboolean(state, g_mouse_middle ? 1 : 0);
    lua_pushboolean(state, g_mouse_right ? 1 : 0);
    return 5;
}

int ui_game_speed(lua_State* state) {
    lua_pushnumber(state, 1);
    lua_pushnumber(state, 1);
    lua_pushboolean(state, 0);
    return 3;
}

int ui_wind(lua_State* state) {
    (void)state;
    const double speed = 0.5 * (static_cast<double>(g_env.min_wind) + static_cast<double>(g_env.max_wind));
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, speed);
    return 4;
}

int ui_gui_hidden(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}

int ui_noop(lua_State* state) {
    (void)state;
    return 0;
}



int ui_get_unit_health(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] != unit && static_cast<int>(i) != unit) {
            continue;
        }
        const float hp = i < g_sim->health.size() ? g_sim->health[i] : 0.f;
        const float hp_max = i < g_sim->max_health.size() ? g_sim->max_health[i] : 0.f;
        lua_pushnumber(state, hp);
        lua_pushnumber(state, hp_max);
        lua_pushnumber(state, 0);
        lua_pushnumber(state, 0);
        lua_pushnumber(state, hp_max > 0.f ? static_cast<double>(hp / hp_max) : 0.0);
        return 5;
    }
    return 0;
}

int ui_trace_screen_ray(lua_State* state) {
    // Only unit 0 is drawn. If the cursor is near its screen mark, return "unit", id.
    if (g_sim == nullptr || g_sim->id.empty() || g_camera == nullptr) {
        return 0;
    }
    // Without a live pose here, return ground nil — hover is driven by hud_status events.
    (void)state;
    return 0;
}

int gl_slave_minimap(lua_State* state) {
    const bool slave = lua_toboolean(state, 1) != 0;
    minimap_set_slave(slave);
    return 0;
}

int gl_draw_minimap(lua_State* state) {
    (void)state;
    // Content is appended in C++ after DrawScreen via minimap_append_draw.
    return 0;
}

int ui_send_commands(lua_State* state) {
    const int top = lua_gettop(state);
    for (int i = 1; i <= top; ++i) {
        if (lua_isstring(state, i)) {
            minimap_send_command(lua_tostring(state, i));
        } else if (lua_istable(state, i)) {
            const int n = static_cast<int>(lua_objlen(state, i));
            for (int k = 1; k <= n; ++k) {
                lua_rawgeti(state, i, k);
                if (lua_isstring(state, -1)) {
                    minimap_send_command(lua_tostring(state, -1));
                }
                lua_pop(state, 1);
            }
        }
    }
    return 0;
}

int ui_subdirs(lua_State* state) {
    const char* relative = luaL_checkstring(state, 1);
    lua_newtable(state);
    if (!safe_relative(relative)) {
        return 1;
    }
    std::error_code failure;
    const std::filesystem::path dir = rooted(relative);
    std::filesystem::directory_iterator it(dir, failure);
    if (failure) {
        return 1;
    }
    std::vector<std::string> names;
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(failure)) {
        if (failure) {
            break;
        }
        std::error_code kind;
        if (!it->is_directory(kind) || kind) {
            continue;
        }
        names.push_back(std::string(relative) + "/" + it->path().filename().string() + "/");
    }
    int count = 0;
    for (const std::string& name : names) {
        lua_pushlstring(state, name.data(), name.size());
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int ui_dir_list(lua_State* state) {
    const char* relative = luaL_checkstring(state, 1);
    const char* pattern = lua_tostring(state, 2);
    std::string suffix;
    if (pattern != nullptr && pattern[0] == '*' && pattern[1] == '.') {
        suffix = pattern + 1;
    }
    lua_newtable(state);
    if (!safe_relative(relative)) {
        return 1;
    }
    std::error_code failure;
    std::filesystem::directory_iterator it(rooted(relative), failure);
    if (failure) {
        return 1;
    }
    std::vector<std::string> names;
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(failure)) {
        if (failure) {
            break;
        }
        std::error_code kind;
        if (!it->is_regular_file(kind) || kind) {
            continue;
        }
        const std::string filename = it->path().filename().string();
        if (!suffix.empty() && (filename.size() < suffix.size() ||
                                 filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)) {
            continue;
        }
        names.push_back(std::string(relative) + "/" + filename);
    }
    int count = 0;
    for (const std::string& name : names) {
        lua_pushlstring(state, name.data(), name.size());
        lua_rawseti(state, -2, ++count);
    }
    return 1;
}

int ui_load_file(lua_State* state) {
    const char* relative = luaL_checkstring(state, 1);
    if (!safe_relative(relative)) {
        lua_pushnil(state);
        return 1;
    }
    std::ifstream in(rooted(relative), std::ios::binary);
    if (!in) {
        lua_pushnil(state);
        return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();
    lua_pushlstring(state, bytes.data(), bytes.size());
    return 1;
}

int ui_file_exists(lua_State* state) {
    const char* relative = luaL_checkstring(state, 1);
    std::error_code failure;
    const bool exists = safe_relative(relative) && std::filesystem::is_regular_file(rooted(relative), failure);
    lua_pushboolean(state, exists ? 1 : 0);
    return 1;
}

bool run_chunk(lua_State* state, const char* source, std::string& error) {
    if (luaL_loadstring(state, source) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "widget prelude failed";
        lua_pop(state, 1);
        return false;
    }
    return pcall_keep(state, 0, 0, error);
}

bool include_one(lua_State* state, const char* path, std::string& error) {
    lua_getglobal(state, "VFS");
    lua_getfield(state, -1, "Include");
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        error = "VFS.Include is missing";
        return false;
    }
    lua_pushstring(state, path);
    return pcall_keep(state, 1, 1, error);
}

bool assign_include(lua_State* state, const char* global_name, const char* field, const char* path, std::string& error) {
    if (!include_one(state, path, error)) {
        return false;
    }
    lua_getglobal(state, global_name);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setglobal(state, global_name);
    }
    lua_pushvalue(state, -2);
    lua_setfield(state, -2, field);
    lua_pop(state, 2);
    return true;
}

bool load_widget(lua_State* state, const std::string& path, const char* key, std::string& error) {
    // Recoil's widget handler runs each widget chunk in its own environment whose
    // reads fall through to the shared globals; undeclared globals a widget assigns
    // (gui_top_bar's and gui_idle_builders' uiTex, for one) stay private to it.
    if (luaL_loadfile(state, path.c_str()) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "widget load failed";
        lua_pop(state, 1);
        return false;
    }
    lua_newtable(state);  // env
    lua_newtable(state);  // widget
    lua_pushvalue(state, -1);
    lua_setfield(state, -3, "widget");
    lua_setglobal(state, "widget");  // hosts that read the global widget still see it
    lua_newtable(state);             // env metatable
    lua_pushvalue(state, LUA_GLOBALSINDEX);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    lua_setfield(state, LUA_REGISTRYINDEX, (std::string(key) + "_env").c_str());
    lua_setfenv(state, -2);
    if (!pcall_keep(state, 0, 0, error)) {
        return false;
    }
    lua_getfield(state, LUA_REGISTRYINDEX, (std::string(key) + "_env").c_str());
    lua_getfield(state, -1, "widget");
    lua_remove(state, -2);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = path + " did not leave a widget table";
        return false;
    }
    lua_setfield(state, LUA_REGISTRYINDEX, key);
    return true;
}

ExtraWidget* extra_current() {
    for (ExtraWidget& w : g_extra) {
        if (g_current_widget == w.key) {
            return &w;
        }
    }
    return nullptr;
}

bool call_registry(lua_State* state, const char* key, const char* method, const double* args, int nargs, bool required,
                   std::string& error) {
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = std::string(key) + " is missing";
        return false;
    }
    lua_getfield(state, -1, method);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        if (!required) {
            return true;
        }
        error = std::string(key) + " has no " + method;
        return false;
    }
    lua_pushvalue(state, -2);
    for (int i = 0; i < nargs; ++i) {
        lua_pushnumber(state, args[i]);
    }
    lua_remove(state, -(nargs + 3));
    return pcall_keep(state, nargs + 1, 0, error);
}

bool call_flow_resize(lua_State* state, int width, int height, std::string& error) {
    const int base = lua_gettop(state);
    lua_getglobal(state, "WG");
    if (!lua_istable(state, -1)) {
        lua_settop(state, base);
        error = "WG is missing";
        return false;
    }
    lua_getfield(state, -1, "FlowUI");
    lua_getfield(state, -1, "Callin");
    lua_getfield(state, -1, "ViewResize1");
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        error = "WG.FlowUI.Callin.ViewResize1 is missing";
        return false;
    }
    lua_pushnumber(state, width);
    lua_pushnumber(state, height);
    // Drop WG, FlowUI, and Callin. The function and the two sizes stay.
    lua_remove(state, base + 3);
    lua_remove(state, base + 2);
    lua_remove(state, base + 1);
    return pcall_keep(state, 2, 0, error);
}


int ui_get_selection_box(lua_State* state) {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    if (!selection_get_box(x1, y1, x2, y2)) {
        return 0;
    }
    lua_pushnumber(state, x1);
    lua_pushnumber(state, y1);
    lua_pushnumber(state, x2);
    lua_pushnumber(state, y2);
    return 4;
}

int ui_get_mod_keys(lua_State* state) {
    lua_pushboolean(state, g_mod_alt ? 1 : 0);
    lua_pushboolean(state, g_mod_ctrl ? 1 : 0);
    lua_pushboolean(state, g_mod_meta ? 1 : 0);
    lua_pushboolean(state, g_mod_shift ? 1 : 0);
    return 4;
}

int ui_get_minimap_geometry(lua_State* state) {
    const MinimapGeometry& geo = minimap_geometry();
    lua_pushnumber(state, geo.px);
    lua_pushnumber(state, geo.py);
    lua_pushnumber(state, geo.sx);
    lua_pushnumber(state, geo.sy);
    lua_pushboolean(state, geo.minimized ? 1 : 0);
    lua_pushboolean(state, geo.maximized ? 1 : 0);
    return 6;
}

int ui_get_selected_units(lua_State* state) {
    const SelectionSnapshot& snap = selection_snapshot();
    lua_newtable(state);
    for (size_t i = 0; i < snap.selected.size(); ++i) {
        lua_pushinteger(state, snap.selected[i]);
        lua_rawseti(state, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int ui_get_active_command(lua_State* state) {
    const SelectionSnapshot& snap = selection_snapshot();
    if (snap.active_cmd == 0) {
        return 0;
    }
    lua_pushinteger(state, 1);
    lua_pushinteger(state, snap.active_cmd);
    lua_pushinteger(state, 0);
    lua_pushstring(state, snap.active_cmd == 10 ? "Move" : "Command");
    return 4;
}

int ui_set_mouse_cursor(lua_State* state) {
    const char* name = lua_tostring(state, 1);
    if (name != nullptr) {
        selection_set_cursor(name);
    }
    return 0;
}

int ui_get_mouse_cursor(lua_State* state) {
    lua_pushstring(state, selection_snapshot().cursor.c_str());
    lua_pushnumber(state, 1);
    return 2;
}

int ui_load_cmd_colors(lua_State* state) {
    const char* text = lua_tostring(state, 1);
    if (text != nullptr) {
        g_cmd_colors = text;
    }
    return 0;
}

int ui_replace_mouse_cursor(lua_State* state) {
    (void)state;
    return 0;
}

int ui_get_screen_geometry(lua_State* state) {
    return ui_view_geometry(state);
}

int ui_set_config_float(lua_State* state) {
    (void)state;
    return 0;
}

int ui_handler_update_callin(lua_State* state) {
    const char* name = lua_tostring(state, 2);
    if (name == nullptr) {
        return 0;
    }
    const std::string callin = name;
    if (g_current_widget == "bar_ui_select") {
        if (callin == "DrawScreen") {
            g_select_draw = true;
        } else if (callin == "Update") {
            g_select_update = true;
        }
    } else if (g_current_widget == "bar_ui_grid") {
        if (callin == "DrawScreen") {
            g_grid_draw = true;
        } else if (callin == "Update") {
            g_grid_update = true;
        }
    } else if (g_current_widget == "bar_ui_minimap") {
        if (callin == "DrawScreen") {
            g_minimap_draw = true;
        } else if (callin == "Update") {
            g_minimap_update = true;
        }
    } else if (g_current_widget == "bar_ui_cursor") {
        if (callin == "Update") {
            g_cursor_update = true;
        }
    } else if (g_current_widget == "bar_ui_order") {
        if (callin == "DrawScreen") {
            g_order_draw = true;
        } else if (callin == "Update") {
            g_order_update = true;
        }
    } else if (g_current_widget == "bar_ui_info") {
        if (callin == "DrawScreen") {
            g_info_draw = true;
        } else if (callin == "Update") {
            g_info_update = true;
        }
    } else if (ExtraWidget* w = extra_current()) {
        if (callin == "DrawScreen") {
            w->draw = true;
        } else if (callin == "Update") {
            w->update = true;
        }
    }
    return 0;
}

int ui_handler_remove_callin(lua_State* state) {
    const char* name = lua_tostring(state, 2);
    if (name == nullptr) {
        return 0;
    }
    const std::string callin = name;
    if (g_current_widget == "bar_ui_select") {
        if (callin == "DrawScreen") {
            g_select_draw = false;
        } else if (callin == "Update") {
            g_select_update = false;
        }
    } else if (g_current_widget == "bar_ui_grid") {
        if (callin == "DrawScreen") {
            g_grid_draw = false;
        } else if (callin == "Update") {
            g_grid_update = false;
        }
    } else if (g_current_widget == "bar_ui_minimap") {
        if (callin == "DrawScreen") {
            g_minimap_draw = false;
        } else if (callin == "Update") {
            g_minimap_update = false;
        }
    } else if (g_current_widget == "bar_ui_cursor") {
        if (callin == "Update") {
            g_cursor_update = false;
        }
    } else if (g_current_widget == "bar_ui_order") {
        if (callin == "DrawScreen") {
            g_order_draw = false;
        } else if (callin == "Update") {
            g_order_update = false;
        }
    } else if (g_current_widget == "bar_ui_info") {
        if (callin == "DrawScreen") {
            g_info_draw = false;
        } else if (callin == "Update") {
            g_info_update = false;
        }
    } else if (ExtraWidget* w = extra_current()) {
        if (callin == "DrawScreen") {
            w->draw = false;
        } else if (callin == "Update") {
            w->update = false;
        }
    }
    return 0;
}


int ui_get_game_frame(lua_State* state) {
    lua_pushinteger(state, g_frame);
    return 1;
}

// The boot startscript has one human player, [player0], so the local player is 0.
// bar-metal boots a live game from a startscript; it has no demo playback.
int ui_is_replay(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}

int ui_local_player(lua_State* state) {
    lua_pushinteger(state, 0);
    return 1;
}

// Spring.GetTimer returns an opaque timer; Recoil packs microseconds into a light
// userdata. DiffTimers(a, b[, inMilliseconds]) returns seconds (or ms) between them.
int ui_get_timer(lua_State* state) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    lua_pushlightuserdata(state, reinterpret_cast<void*>(static_cast<uintptr_t>(us)));
    return 1;
}

int ui_diff_timers(lua_State* state) {
    const auto a = reinterpret_cast<uintptr_t>(lua_touserdata(state, 1));
    const auto b = reinterpret_cast<uintptr_t>(lua_touserdata(state, 2));
    const double us = static_cast<double>(static_cast<int64_t>(a - b));
    lua_pushnumber(state, lua_toboolean(state, 3) ? us / 1000.0 : us / 1e6);
    return 1;
}

int ui_get_game_seconds(lua_State* state) {
    lua_pushnumber(state, static_cast<double>(g_frame) / 30.0);
    return 1;
}

int sim_def_id_of(int unit_id) {
    if (g_sim == nullptr) return 0;
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] == unit_id) return player_unitdef_id(g_sim->def_name[i]);
    }
    return 0;
}

int ui_get_selected_units_counts(lua_State* state) {
    const SelectionSnapshot& snap = selection_snapshot();
    lua_newtable(state);
    int n = 0;
    for (int unit_id : snap.selected) {
        const int def_id = sim_def_id_of(unit_id);
        if (def_id == 0) continue;
        lua_rawgeti(state, -1, def_id);
        const int have = lua_isnumber(state, -1) ? static_cast<int>(lua_tointeger(state, -1)) : 0;
        lua_pop(state, 1);
        if (have == 0) ++n;
        lua_pushinteger(state, have + 1);
        lua_rawseti(state, -2, def_id);
    }
    lua_pushinteger(state, n);
    lua_setfield(state, -2, "n");
    return 1;
}

int ui_get_units_in_sphere(lua_State* state) {
    const double x = luaL_checknumber(state, 1);
    const double y = luaL_checknumber(state, 2);
    const double z = luaL_checknumber(state, 3);
    const double r = luaL_checknumber(state, 4);
    lua_newtable(state);
    if (g_sim == nullptr) return 1;
    int n = 0;
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        const double dx = g_sim->x[i] - x, dy = g_sim->y[i] - y, dz = g_sim->z[i] - z;
        if (dx * dx + dy * dy + dz * dz <= r * r) {
            lua_pushinteger(state, g_sim->id[i]);
            lua_rawseti(state, -2, ++n);
        }
    }
    return 1;
}

int ui_select_unit_array(lua_State* state) {
    std::vector<int> ids;
    if (lua_istable(state, 1)) {
        const int n = static_cast<int>(lua_objlen(state, 1));
        for (int k = 1; k <= n; ++k) {
            lua_rawgeti(state, 1, k);
            if (lua_isnumber(state, -1)) ids.push_back(static_cast<int>(lua_tointeger(state, -1)));
            lua_pop(state, 1);
        }
    }
    selection_replace(ids, lua_toboolean(state, 2) != 0);
    return 0;
}

int ui_select_unit_map(lua_State* state) {
    std::vector<int> ids;
    if (lua_istable(state, 1)) {
        lua_pushnil(state);
        while (lua_next(state, 1) != 0) {
            if (lua_isnumber(state, -2)) ids.push_back(static_cast<int>(lua_tointeger(state, -2)));
            lua_pop(state, 1);
        }
    }
    selection_replace(ids, lua_toboolean(state, 2) != 0);
    return 0;
}

int ui_get_selected_units_sorted(lua_State* state) {
    const SelectionSnapshot& snap = selection_snapshot();
    lua_newtable(state);
    if (g_sim == nullptr || snap.selected.empty()) {
        return 1;
    }
    // Map defID -> array of unit ids. Look up def from sim.
    for (int unit_id : snap.selected) {
        int def_id = 0;
        for (size_t i = 0; i < g_sim->id.size(); ++i) {
            if (g_sim->id[i] != unit_id) {
                continue;
            }
            def_id = player_unitdef_id(g_sim->def_name[i]);
            break;
        }
        if (def_id == 0) {
            def_id = 1;
        }
        lua_rawgeti(state, -1, def_id);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushvalue(state, -1);
            lua_rawseti(state, -3, def_id);
        }
        const int n = static_cast<int>(lua_objlen(state, -1));
        lua_pushinteger(state, unit_id);
        lua_rawseti(state, -2, n + 1);
        lua_pop(state, 1);
    }
    // n = count of distinct def keys
    int n = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        ++n;
        lua_pop(state, 1);
    }
    lua_pushinteger(state, n);
    lua_setfield(state, -2, "n");
    return 1;
}


void push_cmd_desc(lua_State* state, int id, int type, const char* name, const char* action, const char* cursor,
                   bool disabled, const float* params, int nparams) {
    lua_newtable(state);
    lua_pushinteger(state, id);
    lua_setfield(state, -2, "id");
    lua_pushinteger(state, type);
    lua_setfield(state, -2, "type");
    lua_pushstring(state, name);
    lua_setfield(state, -2, "name");
    lua_pushstring(state, action);
    lua_setfield(state, -2, "action");
    lua_pushstring(state, cursor != nullptr ? cursor : "");
    lua_setfield(state, -2, "cursor");
    lua_pushboolean(state, disabled ? 1 : 0);
    lua_setfield(state, -2, "disabled");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "hidden");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "queueing");
    lua_newtable(state);
    for (int i = 0; i < nparams; ++i) {
        lua_pushnumber(state, params[i]);
        lua_rawseti(state, -2, i + 1);
    }
    lua_setfield(state, -2, "params");
}

// Active cmd list for the current selection. Wired to real selection + unit role;
// state params are hold/fire/repeat defaults for a living commander (not invented ids).
int ui_get_active_cmd_descs(lua_State* state) {
    lua_newtable(state);
    const SelectionSnapshot& snap = selection_snapshot();
    if (g_sim == nullptr || snap.selected.empty()) {
        return 1;
    }
    const int unit = snap.selected.front();
    int n = 0;
    auto add = [&](int id, int type, const char* name, const char* action, const char* cursor, const float* params,
                   int nparams) {
        push_cmd_desc(state, id, type, name, action, cursor, false, params, nparams);
        lua_rawseti(state, -2, ++n);
    };
    // CMDTYPE from Recoil Command.h
    constexpr int T_MODE = 5;
    auto add_mode = [&](int id, const char* name, const char* action, const char* cursor, int mode,
                        std::initializer_list<const char*> labels) {
        lua_newtable(state);
        lua_pushinteger(state, id);
        lua_setfield(state, -2, "id");
        lua_pushinteger(state, T_MODE);
        lua_setfield(state, -2, "type");
        lua_pushstring(state, name);
        lua_setfield(state, -2, "name");
        lua_pushstring(state, action);
        lua_setfield(state, -2, "action");
        lua_pushstring(state, cursor);
        lua_setfield(state, -2, "cursor");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "disabled");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "hidden");
        lua_newtable(state);
        lua_pushnumber(state, mode);
        lua_rawseti(state, -2, 1);
        int pi = 2;
        for (const char* lab : labels) {
            lua_pushstring(state, lab);
            lua_rawseti(state, -2, pi++);
        }
        lua_setfield(state, -2, "params");
        lua_rawseti(state, -2, ++n);
    };
    // Spring CmdDesc params: [1]=current state, [2..]=labels (see CustomFirestateDefs.stateLabel).
    // Same list and order as GetCmdDescIndex / SetActiveCommand(index) (build_active_descs).
    for (const ActiveCmdDesc& desc : build_active_descs(g_sim, unit)) {
        if (desc.id == 45) {
            add_mode(45, "Firestate", "firestate", "cursorattack", unit_fire_state(g_sim, unit),
                     {"Hold fire", "Return fire", "Fire at will"});
        } else if (desc.id == 50) {
            add_mode(50, "Movestate", "movestate", "cursormove", unit_move_state(g_sim, unit),
                     {"Hold pos", "Maneuver", "Roam"});
        } else if (desc.id == 115) {
            add_mode(115, "Repeat", "repeat", "cursorattack", unit_repeat_state(unit), {"Repeat off", "Repeat on"});
        } else {
            add(desc.id, desc.type, desc.name.c_str(), desc.action.c_str(), desc.cursor.c_str(), nullptr, 0);
        }
    }
    (void)unit;
    return 1;
}

int ui_find_unit_cmd_desc(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    const int cmd = static_cast<int>(luaL_checknumber(state, 2));
    const int index = build_active_desc_index(g_sim, unit, cmd);
    if (index > 0) {
        lua_pushinteger(state, index);
        return 1;
    }
    return 0;
}

int ui_valid_unit_id(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) {
        lua_pushboolean(state, 0);
        return 1;
    }
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] == unit && (g_sim->alive.empty() || g_sim->alive[i] != 0)) {
            lua_pushboolean(state, 1);
            return 1;
        }
    }
    lua_pushboolean(state, 0);
    return 1;
}

int ui_get_unit_commands(lua_State* state) {
    // Empty queue — selection has no engine command queue hosted yet.
    (void)state;
    lua_newtable(state);
    return 1;
}

int ui_get_factory_commands(lua_State* state) {
    (void)state;
    lua_newtable(state);
    return 1;
}

int ui_get_unit_rules_param(lua_State* state) {
    (void)state;
    // No unit rules params hosted; return nil (real absence).
    return 0;
}

int ui_get_selected_units_count(lua_State* state) {
    lua_pushinteger(state, static_cast<int>(selection_snapshot().selected.size()));
    return 1;
}

int ui_get_unit_cmd_descs(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) {
        return 0;
    }
    // Prefer nil so gridmenu uses UnitDefs.buildOptions (already annotated).
    (void)unit;
    return 0;
}

int ui_get_unit_def_id(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] == unit) {
            const int id = player_unitdef_id(g_sim->def_name[i]);
            if (id != 0) {
                lua_pushinteger(state, id);
                return 1;
            }
        }
    }
    return 0;
}

int ui_get_unit_is_being_built(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    float progress = 0;
    if (!unit_is_being_built(unit, &progress)) {
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_pushboolean(state, 1);
    lua_pushnumber(state, progress);
    return 2;
}

int ui_set_active_command(lua_State* state) {
    // Forms: SetActiveCommand(0) | SetActiveCommand(cmd) | SetActiveCommand(idx, button, left, right, ...)
    if (lua_isnoneornil(state, 1) || (lua_isnumber(state, 1) && lua_tonumber(state, 1) == 0)) {
        build_clear_active_command();
        lua_pushboolean(state, 1);
        return 1;
    }
    const int cmd = static_cast<int>(lua_tonumber(state, 1));
    const SelectionSnapshot& snap = selection_snapshot();
    const int selected = snap.selected.empty() ? 0 : snap.selected.front();
    std::string error;
    if (cmd < 0) {
        // Negative unitDefID. Resolve name from UnitDefs.
        lua_getglobal(state, "UnitDefs");
        lua_rawgeti(state, -1, -cmd);
        std::string name;
        if (lua_istable(state, -1)) {
            lua_getfield(state, -1, "name");
            if (lua_isstring(state, -1)) {
                name = lua_tostring(state, -1);
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 2);
        if (name.empty() || !build_set_by_def(name, -cmd, g_sim, error)) {
            lua_pushboolean(state, 0);
            return 1;
        }
        lua_pushboolean(state, 1);
        return 1;
    }
    (void)selected;
    // SetActiveCommand(index [, button, leftClick, rightClick, alt, ctrl, meta, shift]).
    const int button = lua_isnumber(state, 2) ? static_cast<int>(lua_tonumber(state, 2)) : 1;
    if (!build_activate_index(cmd, button, g_sim, snap.selected, error)) {
        std::cerr << "SetActiveCommand(" << cmd << "): " << error << "\n";
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_pushboolean(state, 1);
    return 1;
}

int ui_get_active_command_build(lua_State* state) {
    const BuildCmdState& build = build_cmd_state();
    if (build.placing && build.active_cmd != 0) {
        lua_pushinteger(state, build.active_index > 0 ? build.active_index : 1);
        lua_pushinteger(state, build.active_cmd);
        lua_pushinteger(state, 0);
        lua_pushstring(state, build.active_name.c_str());
        return 4;
    }
    // Fall back to selection move command.
    return ui_get_active_command(state);
}

// GetUnitStates(unitID): the unit's fire / move / repeat state.
int ui_get_unit_states(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    bool valid = false;
    if (g_sim != nullptr) {
        for (size_t i = 0; i < g_sim->id.size(); ++i) {
            valid = valid || (g_sim->id[i] == unit && (g_sim->alive.empty() || g_sim->alive[i] != 0));
        }
    }
    if (!valid) {
        return 0;
    }
    // GetUnitStates(unitID, false): firestate, movestate, repeat, cloak, active, trajectory.
    if (lua_isboolean(state, 2) && lua_toboolean(state, 2) == 0) {
        lua_pushinteger(state, unit_fire_state(g_sim, unit));
        lua_pushinteger(state, unit_move_state(g_sim, unit));
        lua_pushboolean(state, unit_repeat_state(unit));
        lua_pushboolean(state, 0);
        lua_pushboolean(state, 1);
        lua_pushboolean(state, 0);
        return 6;
    }
    lua_newtable(state);
    lua_pushinteger(state, unit_fire_state(g_sim, unit));
    lua_setfield(state, -2, "firestate");
    lua_pushinteger(state, unit_move_state(g_sim, unit));
    lua_setfield(state, -2, "movestate");
    lua_pushboolean(state, unit_repeat_state(unit));
    lua_setfield(state, -2, "repeat");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "cloak");
    lua_pushboolean(state, 1);
    lua_setfield(state, -2, "active");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "trajectory");
    return 1;
}

int ui_get_cmd_desc_index(lua_State* state) {
    const int cmd = static_cast<int>(luaL_checknumber(state, 1));
    const SelectionSnapshot& snap = selection_snapshot();
    const int unit = snap.selected.empty() ? 0 : snap.selected.front();
    const int index = build_active_desc_index(g_sim, unit, cmd);
    if (index == 0) {
        return 0;
    }
    lua_pushinteger(state, index);
    return 1;
}



int ui_get_mod_options(lua_State* state) {
    // Same defaults as gadget_host spring_get_mod_options (modoptions.lua file defs).
    // gui_top_bar checkTidalRelevant requires debugcommands to be a string.
    lua_newtable(state);
    lua_pushstring(state, "");
    lua_setfield(state, -2, "debugcommands");
    lua_pushnumber(state, 0);
    lua_setfield(state, -2, "map_waterlevel");
    lua_pushnumber(state, 1000);
    lua_setfield(state, -2, "startmetal");
    lua_pushnumber(state, 1000);
    lua_setfield(state, -2, "startenergy");
    lua_pushnumber(state, 1000);
    lua_setfield(state, -2, "startmetalstorage");
    lua_pushnumber(state, 1000);
    lua_setfield(state, -2, "startenergystorage");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "proposed_unit_reworks");
    lua_pushstring(state, "normal");
    lua_setfield(state, -2, "scav_difficulty");
    lua_pushstring(state, "normal");
    lua_setfield(state, -2, "raptor_difficulty");
    return 1;
}

int ui_get_team_color(lua_State* state) {
    // Team 0's colour after BAR autocolors (map/autocolors.cpp).
    float r = 0, g = 0, b = 0;
    ui_team_color(r, g, b);
    lua_pushnumber(state, r);
    lua_pushnumber(state, g);
    lua_pushnumber(state, b);
    return 3;
}

int ui_get_team_info(lua_State* state) {
    lua_pushstring(state, "Player");
    lua_pushinteger(state, 0);  // leader
    lua_pushboolean(state, 0);  // dead
    lua_pushboolean(state, 0);  // isAI
    lua_pushstring(state, "Armada");
    lua_pushinteger(state, 0);  // allyTeam
    return 6;
}

int ui_get_current_tooltip(lua_State* state) {
    lua_pushstring(state, hud_status_tooltip().c_str());
    return 1;
}

int ui_get_unit_position(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) return 0;
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] == unit) {
            lua_pushnumber(state, g_sim->x[i]);
            lua_pushnumber(state, g_sim->y[i]);
            lua_pushnumber(state, g_sim->z[i]);
            return 3;
        }
    }
    return 0;
}

int ui_get_unit_radius(lua_State* state) {
    (void)state;
    lua_pushnumber(state, 20);
    return 1;
}

int ui_get_unit_height(lua_State* state) {
    (void)state;
    lua_pushnumber(state, 40);
    return 1;
}

int ui_get_unit_experience(lua_State* state) {
    lua_pushnumber(state, 0);
    return 1;
}

int ui_get_unit_resources(lua_State* state) {
    // metalMake, metalUse, energyMake, energyUse — commander income from top bar state is elsewhere;
    // report zeros when unknown.
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    return 4;
}

int ui_get_unit_team(lua_State* state) {
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    if (g_sim == nullptr) return 0;
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        if (g_sim->id[i] == unit) {
            lua_pushinteger(state, g_sim->team[i]);
            return 1;
        }
    }
    return 0;
}

int ui_get_player_info(lua_State* state) {
    lua_pushstring(state, "Player");
    lua_pushboolean(state, 1);  // active
    lua_pushinteger(state, 0);  // spectator
    lua_pushinteger(state, 0);  // team
    lua_pushinteger(state, 0);  // allyTeam
    lua_pushnumber(state, 0);   // ping
    lua_pushnumber(state, 0);   // cpu
    lua_pushstring(state, "");  // country
    lua_pushinteger(state, 0);  // rank
    return 9;
}

int ui_get_ai_info(lua_State* state) {
    return 0;
}

int ui_get_all_features(lua_State* state) {
    lua_newtable(state);
    return 1;
}

int ui_get_features_in_sphere(lua_State* state) {
    lua_newtable(state);
    return 1;
}

int ui_get_feature_def_id(lua_State* state) {
    return 0;
}

int ui_get_feature_resources(lua_State* state) {
    return 0;
}

int ui_get_ground_info(lua_State* state) {
    lua_pushstring(state, "Grass");
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    return 6;
}

int ui_get_game_rules_param(lua_State* state) {
    return 0;
}

int ui_get_unit_mass(lua_State* state) {
    lua_pushnumber(state, 5000);
    return 1;
}

int ui_get_unit_is_transporting(lua_State* state) {
    lua_newtable(state);
    return 1;
}

int ui_get_team_units_sorted(lua_State* state) {
    lua_newtable(state);
    return 1;
}

int ui_get_key_bindings(lua_State* state) {
    // No keybindings archive hosted yet — empty list is the real state.
    lua_newtable(state);
    return 1;
}

int ui_get_action_hotkeys(lua_State* state) {
    lua_newtable(state);
    return 1;
}

int ui_play_sound(lua_State* state) {
    (void)state;
    return 0;
}

// bar-metal has no audio backend yet (PlaySoundFile above plays nothing), so no
// sound items get registered; Recoil returns false when a sound def did not load.
int ui_load_sound_def(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}

bool give_unit_order(lua_State* state, int unit, int cmd, int params_index);

int ui_give_order(lua_State* state) {
    // GiveOrderToUnit(unitID, cmdID, params, options).
    const int unit = static_cast<int>(luaL_checknumber(state, 1));
    const int cmd = static_cast<int>(luaL_checknumber(state, 2));
    lua_pushboolean(state, give_unit_order(state, unit, cmd, 3) ? 1 : 0);
    return 1;
}

// GiveOrder(cmdID, params, options) goes to every selected unit.
int ui_give_order_selected(lua_State* state) {
    const int cmd = static_cast<int>(luaL_checknumber(state, 1));
    bool any = false;
    const std::vector<int> selected = selection_snapshot().selected;
    for (int unit : selected) {
        any = give_unit_order(state, unit, cmd, 2) || any;
    }
    lua_pushboolean(state, any ? 1 : 0);
    return 1;
}

// GiveOrderToUnitArray(unitIDs, cmdID, params, options).
int ui_give_order_array(lua_State* state) {
    const int cmd = static_cast<int>(luaL_checknumber(state, 2));
    bool any = false;
    if (lua_istable(state, 1)) {
        const int n = static_cast<int>(lua_objlen(state, 1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(state, 1, i);
            const int unit = static_cast<int>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            any = give_unit_order(state, unit, cmd, 3) || any;
        }
    }
    lua_pushboolean(state, any ? 1 : 0);
    return 1;
}

bool give_unit_order(lua_State* state, int unit, int cmd, int params_index) {
    if (cmd == 45 || cmd == 50 || cmd == 115) {
        int value = -1;
        if (lua_istable(state, params_index)) {
            lua_rawgeti(state, params_index, 1);
            value = lua_isnumber(state, -1) ? static_cast<int>(lua_tonumber(state, -1)) : -1;
            lua_pop(state, 1);
        } else if (lua_isnumber(state, params_index)) {
            value = static_cast<int>(lua_tonumber(state, params_index));
        }
        return unit_apply_state_command(g_sim, unit, cmd, value);
    }
    if (cmd < 0) {
        float x = 0;
        float z = 0;
        if (lua_istable(state, params_index)) {
            lua_rawgeti(state, params_index, 1);
            x = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            lua_rawgeti(state, params_index, 3);
            z = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
        }
        lua_getglobal(state, "UnitDefs");
        lua_rawgeti(state, -1, -cmd);
        std::string name;
        if (lua_istable(state, -1)) {
            lua_getfield(state, -1, "name");
            if (lua_isstring(state, -1)) {
                name = lua_tostring(state, -1);
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 2);
        std::string error;
        if (!name.empty() && player_issue_build(unit, name, x, z, error)) {
            return true;
        }
    }
    return false;
}

// Spring.Echo stringifies each argument with tostring and joins them with ", ".
std::string echo_arg(lua_State* state, int index) {
    lua_getglobal(state, "tostring");
    lua_pushvalue(state, index);
    lua_call(state, 1, 1);
    size_t len = 0;
    const char* s = lua_tolstring(state, -1, &len);
    std::string out = s != nullptr ? std::string(s, len) : "nil";
    lua_pop(state, 1);
    return out;
}

int ui_echo(lua_State* state) {
    const int top = lua_gettop(state);
    std::string line;
    for (int i = 1; i <= top; ++i) {
        line += echo_arg(state, i);
        if (i != top) {
            line += ", ";
        }
    }
    std::cout << line << "\n";
    g_console_lines.push_back(std::move(line));
    return 0;
}





int ui_slave_minimap(lua_State* state) {
    // Actually on gl table - see gl_install hook below
    (void)state;
    return 0;
}

int ui_get_camera_state(lua_State* state) {
    lua_newtable(state);
    lua_pushstring(state, "ta");
    lua_setfield(state, -2, "name");
    if (g_camera != nullptr) {
        lua_pushnumber(state, g_camera->target_x);
        lua_setfield(state, -2, "px");
        lua_pushnumber(state, g_camera->target_y);
        lua_setfield(state, -2, "py");
        lua_pushnumber(state, g_camera->target_z);
        lua_setfield(state, -2, "pz");
        lua_pushnumber(state, g_camera->distance);
        lua_setfield(state, -2, "height");
        lua_pushnumber(state, g_camera->tilt);
        lua_setfield(state, -2, "angle");
        lua_pushnumber(state, g_camera->yaw);
        lua_setfield(state, -2, "ry");
    }
    return 1;
}

int ui_set_camera_target(lua_State* state) {
    if (g_camera == nullptr) {
        return 0;
    }
    g_camera->target_x = static_cast<float>(luaL_checknumber(state, 1));
    g_camera->target_y = static_cast<float>(luaL_checknumber(state, 2));
    g_camera->target_z = static_cast<float>(luaL_checknumber(state, 3));
    return 0;
}

int ui_get_ground_height(lua_State* state) {
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float z = static_cast<float>(luaL_checknumber(state, 2));
    if (g_terrain == nullptr) {
        lua_pushnumber(state, 0);
        return 1;
    }
    lua_pushnumber(state, terrain_height_at(*g_terrain, x, z));
    return 1;
}

int ui_get_all_units(lua_State* state) {
    lua_newtable(state);
    if (g_sim == nullptr) {
        return 1;
    }
    for (size_t i = 0; i < g_sim->id.size(); ++i) {
        lua_pushinteger(state, g_sim->id[i]);
        lua_rawseti(state, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int ui_get_minimap_rotation(lua_State* state) {
    lua_pushnumber(state, 0);
    return 1;
}

// Single-player bar-metal has no god-mode toggle; always reports false.
int ui_is_god_mode(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}

// Cheating is not enabled in this client.
int ui_is_cheating(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}

int ui_unit_is_building(lua_State* state) {
    const int builder = static_cast<int>(luaL_checknumber(state, 1));
    const int nano_id = unit_is_building(builder);
    if (nano_id <= 0) {
        return 0;
    }
    lua_pushinteger(state, nano_id);
    return 1;
}

int ui_team_rules_params(lua_State* state) {
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    std::vector<TeamRuleEntry> rows;
    team_rules_all(team, rows);
    lua_newtable(state);
    for (const TeamRuleEntry& row : rows) {
        if (row.number) {
            lua_pushnumber(state, row.numeric);
        } else {
            lua_pushlstring(state, row.text.data(), row.text.size());
        }
        lua_setfield(state, -2, row.key.c_str());
    }
    return 1;
}

int ui_team_rules_param(lua_State* state) {
    const int team = static_cast<int>(luaL_checknumber(state, 1));
    const char* key = luaL_checkstring(state, 2);
    TeamRuleEntry row;
    if (!team_rules_param(team, key, row)) {
        return 0;
    }
    if (row.number) {
        lua_pushnumber(state, row.numeric);
    } else {
        lua_pushlstring(state, row.text.data(), row.text.size());
    }
    return 1;
}

int ui_handler_known(lua_State* state) {
    (void)state;
    lua_pushboolean(state, 0);
    return 1;
}

int ui_handler_disable(lua_State* state) {
    (void)state;
    return 0;
}

int ui_handler_register_cmd(lua_State* state) {
    (void)state;
    return 0;
}

int ui_handler_action_add(lua_State* state) {
    (void)state;
    return 0;
}

int ui_handler_is_modal_active(lua_State* state) {
    lua_pushboolean(state, 0);
    return 1;
}


int ui_get_menu_name(lua_State* state) {
    (void)state;
    lua_pushstring(state, "");
    return 1;
}

int ui_send_lua_menu_msg(lua_State* state) {
    (void)state;
    return 0;
}

int ui_assign_mouse_cursor(lua_State* state) {
    (void)state;
    return 0;
}

int ui_get_map_draw_mode(lua_State* state) {
    (void)state;
    lua_pushstring(state, "normal");
    return 1;
}

int ui_register_modal_window(lua_State* state) {
    (void)state;
    return 0;
}

int ui_handler_get_view_sizes(lua_State* state) {
    lua_pushnumber(state, gl_view_width());
    lua_pushnumber(state, gl_view_height());
    return 2;
}

int gl_config_minimap(lua_State* state) {
    const int px = lua_isnumber(state, 1) != 0 ? static_cast<int>(lua_tonumber(state, 1)) : 0;
    const int py_top = lua_isnumber(state, 2) != 0 ? static_cast<int>(lua_tonumber(state, 2)) : 0;
    const int sx = lua_isnumber(state, 3) != 0 ? static_cast<int>(lua_tonumber(state, 3)) : 0;
    const int sy = lua_isnumber(state, 4) != 0 ? static_cast<int>(lua_tonumber(state, 4)) : 0;
    const int py_bottom = gl_view_height() - py_top - sy;
    minimap_set_geometry(px, py_bottom, sx, sy);
    return 0;
}

// Fill UnitDefNames[name] with unit-file values for every def the live sim carries.
// Same loaders the sim uses; nothing is invented. Fields Spring exposes on UnitDef.
void fill_live_def_stats(lua_State* state, const std::string& game_root) {
    if (g_sim == nullptr) return;
    std::vector<std::string> seen;
    lua_getglobal(state, "Spring");
    lua_newtable(state);
    {
        int n = 0;
        std::vector<std::string> names;
        for (const std::string& nm : g_sim->def_name) {
            if (!nm.empty() && std::find(names.begin(), names.end(), nm) == names.end()) {
                names.push_back(nm);
                lua_pushstring(state, nm.c_str());
                lua_rawseti(state, -2, ++n);
            }
        }
    }
    lua_setfield(state, -2, "_liveDefNames");
    lua_pop(state, 1);
    for (const std::string& name : g_sim->def_name) {
        if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        lua_getglobal(state, "UnitDefNames");
        lua_getfield(state, -1, name.c_str());
        if (!lua_istable(state, -1)) { lua_pop(state, 2); continue; }
        std::string err;
        UnitSensors sensors;
        if (load_unit_sensors(game_root, name, sensors, err)) {
            lua_pushnumber(state, sensors.sight); lua_setfield(state, -2, "sightDistance");
            lua_pushnumber(state, sensors.air); lua_setfield(state, -2, "airSightDistance");
            lua_pushnumber(state, sensors.radar); lua_setfield(state, -2, "radarDistance");
            lua_pushnumber(state, sensors.sonar); lua_setfield(state, -2, "sonarDistance");
        }
        UnitMotion motion;
        if (load_unit_motion(game_root, name, motion, err)) {
            lua_pushnumber(state, motion.speed_per_sec); lua_setfield(state, -2, "speed");
        }
        float range = 0.f;
        if (load_unit_max_weapon_range(game_root, name, range, err)) {
            lua_pushnumber(state, range); lua_setfield(state, -2, "maxWeaponRange");
        }
        float health = 0.f;
        if (load_unit_health(game_root, name, health, err)) {
            lua_pushnumber(state, health); lua_setfield(state, -2, "health");
        }
        UnitBuild build;
        if (load_unit_build(game_root, name, build, err)) {
            lua_pushnumber(state, build.build_time); lua_setfield(state, -2, "buildTime");
            lua_pushnumber(state, build.metal_cost); lua_setfield(state, -2, "metalCost");
            lua_pushnumber(state, build.energy_cost); lua_setfield(state, -2, "energyCost");
        }
        lua_pop(state, 2);
    }
}

// Host plumbing (not a Spring API): the raw unit-file def table for name, or nil.
int ui_unit_file_table(lua_State* state) {
    const char* name = luaL_checkstring(state, 1);
    const std::string path = find_unit_file(g_env.game_root, name);
    if (path.empty()) return 0;
    if (luaL_loadfile(state, path.c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0 || !lua_istable(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    lua_getfield(state, -1, name);
    lua_remove(state, -2);
    return lua_istable(state, -1) ? 1 : (lua_pop(state, 1), 0);
}

bool install_widget_handler(lua_State* state, std::string& error) {
    lua_newtable(state);
    lua_pushcfunction(state, ui_handler_update_callin);
    lua_setfield(state, -2, "UpdateCallIn");
    lua_pushcfunction(state, ui_handler_remove_callin);
    lua_setfield(state, -2, "RemoveCallIn");
    lua_pushcfunction(state, ui_handler_known);
    lua_setfield(state, -2, "IsWidgetKnown");
    lua_pushcfunction(state, ui_handler_disable);
    lua_setfield(state, -2, "DisableWidgetRaw");
    lua_pushcfunction(state, ui_handler_register_cmd);
    lua_setfield(state, -2, "RegisterUnitCommand");
    lua_pushcfunction(state, ui_handler_is_modal_active);
    lua_setfield(state, -2, "IsModalActive");
    lua_pushcfunction(state, ui_handler_action_add);
    lua_setfield(state, -2, "AddAction");
    lua_pushcfunction(state, ui_register_modal_window);
    lua_setfield(state, -2, "RegisterModalWindow");
    lua_pushcfunction(state, ui_handler_get_view_sizes);
    lua_setfield(state, -2, "GetViewSizes");
    lua_newtable(state);
    lua_pushcfunction(state, ui_handler_action_add);
    lua_setfield(state, -2, "AddAction");
    lua_setfield(state, -2, "actionHandler");
    lua_setglobal(state, "widgetHandler");
    (void)error;
    return true;
}

}  // namespace

bool widgets_load(lua_State* state, const WidgetEnv& env, std::string& error) {
    g_loaded = false;
    g_state = nullptr;
    g_hud = HudList{};
    selection_reset();
    g_select_draw = false;
    g_select_update = false;
    g_env = env;
    gl_set_root(env.game_root);
    gl_set_view(env.view_w, env.view_h);
    gl_install(state);
    lua_getglobal(state, "gl");
    if (lua_istable(state, -1)) {
        lua_pushcfunction(state, gl_slave_minimap);
        lua_setfield(state, -2, "SlaveMiniMap");
        lua_pushcfunction(state, gl_draw_minimap);
        lua_setfield(state, -2, "DrawMiniMap");
        lua_pushcfunction(state, gl_config_minimap);
        lua_setfield(state, -2, "ConfigMiniMap");
    }
    lua_pop(state, 1);

    if (!run_chunk(state, R"LUA(
-- Recoil LuaMathExtra::round: floor(x + 0.5), or round to n (0..7) decimals.
function math.round(value, decimals)
  if decimals == nil then return math.floor(value + 0.5) end
  local n = math.max(0, math.min(7, math.floor(decimals)))
  local p = 10 ^ n
  local i = math.floor(value)
  return i + math.floor((value - i) * p + 0.5) / p
end
tracy = tracy or {
  ZoneBeginN = function() end,
  ZoneBegin = function() end,
  ZoneEnd = function() end,
  Message = function() end,
  ZoneName = function() end,
  ZoneText = function() end,
}
function pairsByKeys(tbl, keySortFunction)
  local keys = {}
  for k in pairs(tbl) do keys[#keys + 1] = k end
  table.sort(keys, keySortFunction)
  local i = 0
  return function()
    i = i + 1
    local k = keys[i]
    if k ~= nil then return k, tbl[k] end
  end
end
function math.clamp(value, low, high)
  if value < low then return low end
  if value > high then return high end
  return value
end
function math.isInRect(x, y, x1, y1, x2, y2)
  return x >= x1 and y >= y1 and x <= x2 and y <= y2
end
function string.split(val, delimiter)
  delimiter = delimiter or "%s"
  local results = {}
  local n = 0
  for part in string.gmatch(val, "[^" .. delimiter .. "]+") do
    n = n + 1
    results[n] = part
  end
  return results
end
function string.lines(s)
  if s == nil then return {} end
  s = tostring(s)
  local results = {}
  local n = 0
  for line in string.gmatch(s .. "\n", "(.-)\n") do
    n = n + 1
    results[n] = line
  end
  if n == 0 then results[1] = s; n = 1 end
  return results
end
WG = WG or {}
LUAUI_DIRNAME = "luaui/"
CMD.OPT_ALT = 4
CMD.OPT_CTRL = 64
CMD.OPT_META = 128
CMD.OPT_SHIFT = 32
CMD.OPT_RIGHT = 16
CMD.BUILD = -1
CMD.INSERT = 1
GameCMD = GameCMD or { STOP_PRODUCTION = 37215 }

if not Spring.I18N and BAR and BAR.I18N then Spring.I18N = BAR.I18N end
CMDTYPE = CMDTYPE or {
  ICON = 0, ICON_MODE = 5, ICON_MAP = 10, ICON_AREA = 11, ICON_UNIT = 12,
  ICON_UNIT_OR_MAP = 13, ICON_FRONT = 14, ICON_UNIT_OR_AREA = 16,
  ICON_UNIT_FEATURE_OR_AREA = 19, ICON_BUILDING = 20, CUSTOM = 21,
  ICON_UNIT_OR_RECTANGLE = 22, NUMBER = 23,
}
CMD.WAIT = CMD.WAIT or 5
CMD.REPEAT = CMD.REPEAT or 115
CMD.RESTORE = CMD.RESTORE or 110
CMD.CAPTURE = CMD.CAPTURE or 130
CMD.SELFD = CMD.SELFD or 65
CMD.MANUALFIRE = CMD.MANUALFIRE or CMD.DGUN or 105

local _include = include or VFS.Include
function include(path)
  local tries = { "luaui/Headers/" .. path, "luaui/" .. path, path, "common/" .. path }
  for _, p in ipairs(tries) do
    local ok, result = pcall(_include, p)
    if ok then return result end
  end
  error("include failed: " .. tostring(path))
end
os = os or {}
os.clock = os.clock or function() return 0 end
)LUA",
                            error)) {
        return false;
    }

    lua_getglobal(state, "Game");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = "Game is missing";
        return false;
    }
    lua_pushnumber(state, env.min_wind);
    lua_setfield(state, -2, "windMin");
    lua_pushlstring(state, env.map_name.data(), env.map_name.size());
    lua_setfield(state, -2, "mapName");
    lua_getfield(state, -1, "mapSizeX");
    const double map_size_x = lua_tonumber(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, -1, "mapSizeZ");
    const double map_size_z = lua_tonumber(state, -1);
    lua_pop(state, 1);
    lua_pushnumber(state, map_size_x / 512.0);
    lua_setfield(state, -2, "mapX");
    lua_pushnumber(state, map_size_z / 512.0);
    lua_setfield(state, -2, "mapY");
    lua_pop(state, 1);
    minimap_reset();
    minimap_set_world(static_cast<float>(map_size_x), static_cast<float>(map_size_z), env.view_w, env.view_h);

    set_spring(state, "GetConfigInt", ui_config_number);
    set_spring(state, "GetConfigFloat", ui_config_number);
    set_spring(state, "GetConfigString", ui_config_string);
    set_spring(state, "GetViewGeometry", ui_view_geometry);
    set_spring(state, "GetGroundExtremes", ui_ground_extremes);
    set_spring(state, "GetTidal", ui_tidal);
    set_spring(state, "GetLocalTeamID", ui_local_team);
    set_spring(state, "GetLocalAllyTeamID", ui_local_ally);
    set_spring(state, "GetSpectatingState", ui_spectating);
    set_spring(state, "GetMouseState", ui_mouse_state);
    set_spring(state, "GetGameSpeed", ui_game_speed);
    set_spring(state, "GetWind", ui_wind);
    set_spring(state, "IsGUIHidden", ui_gui_hidden);
    set_spring(state, "SendCommands", ui_send_commands);
    set_spring(state, "SetMouseCursor", ui_set_mouse_cursor);
    set_spring(state, "GetMouseCursor", ui_get_mouse_cursor);
    set_spring(state, "GetSelectionBox", ui_get_selection_box);
    set_spring(state, "GetModKeyState", ui_get_mod_keys);
    set_spring(state, "GetMiniMapGeometry", ui_get_minimap_geometry);
    set_spring(state, "GetSelectedUnits", ui_get_selected_units);
    set_spring(state, "GetActiveCommand", ui_get_active_command_build);
    set_spring(state, "LoadCmdColorsConfig", ui_load_cmd_colors);
    set_spring(state, "ReplaceMouseCursor", ui_replace_mouse_cursor);
    set_spring(state, "GetScreenGeometry", ui_get_screen_geometry);
    set_spring(state, "SetConfigFloat", ui_set_config_float);
    set_spring(state, "SetConfigInt", ui_noop);
    set_spring(state, "SetConfigString", ui_noop);
    set_spring(state, "GetGameFrame", ui_get_game_frame);
    set_spring(state, "GetGameSeconds", ui_get_game_seconds);
    set_spring(state, "GetSelectedUnitsSorted", ui_get_selected_units_sorted);
    set_spring(state, "GetUnitCmdDescs", ui_get_unit_cmd_descs);
    set_spring(state, "GetActiveCmdDescs", ui_get_active_cmd_descs);
    set_spring(state, "GetSelectedUnitsCounts", ui_get_selected_units_counts);
    set_spring(state, "GetUnitsInSphere", ui_get_units_in_sphere);
    set_spring(state, "SelectUnitArray", ui_select_unit_array);
    set_spring(state, "SelectUnitMap", ui_select_unit_map);
    // bar-metal draws no engine selection-info text; the toggle has nothing to switch.
    set_spring(state, "SetDrawSelectionInfo", ui_noop);
    set_spring(state, "_unitFileTable", ui_unit_file_table);
    set_spring(state, "FindUnitCmdDesc", ui_find_unit_cmd_desc);
    set_spring(state, "ValidUnitID", ui_valid_unit_id);
    set_spring(state, "GetUnitCommands", ui_get_unit_commands);
    set_spring(state, "GetFactoryCommands", ui_get_factory_commands);
    set_spring(state, "GetUnitRulesParam", ui_get_unit_rules_param);
    set_spring(state, "GetSelectedUnitsCount", ui_get_selected_units_count);
    set_spring(state, "GetUnitDefID", ui_get_unit_def_id);
    set_spring(state, "GetUnitHealth", ui_get_unit_health);
    set_spring(state, "TraceScreenRay", ui_trace_screen_ray);
    set_spring(state, "GetUnitIsBeingBuilt", ui_get_unit_is_being_built);
    set_spring(state, "SetActiveCommand", ui_set_active_command);
    set_spring(state, "GetCmdDescIndex", ui_get_cmd_desc_index);
    set_spring(state, "GetActionHotKeys", ui_get_action_hotkeys);
    set_spring(state, "GetKeyBindings", ui_get_key_bindings);
    set_spring(state, "GetModOptions", ui_get_mod_options);
    set_spring(state, "GetTeamColor", ui_get_team_color);
    set_spring(state, "GetTeamInfo", ui_get_team_info);
    set_spring(state, "GetCurrentTooltip", ui_get_current_tooltip);
    set_spring(state, "GetUnitPosition", ui_get_unit_position);
    set_spring(state, "GetUnitRadius", ui_get_unit_radius);
    set_spring(state, "GetUnitHeight", ui_get_unit_height);
    set_spring(state, "GetUnitExperience", ui_get_unit_experience);
    set_spring(state, "GetUnitResources", ui_get_unit_resources);
    set_spring(state, "GetUnitTeam", ui_get_unit_team);
    set_spring(state, "GetPlayerInfo", ui_get_player_info);
    set_spring(state, "GetLocalPlayerID", ui_local_player);
    set_spring(state, "IsReplay", ui_is_replay);
    set_spring(state, "LoadSoundDef", ui_load_sound_def);
    set_spring(state, "GetMyPlayerID", ui_local_player);
    set_spring(state, "GetTimer", ui_get_timer);
    // Cocoa delivers key text to the view at all times; there is no SDL text-input
    // mode to switch, so start/stop have nothing to toggle.
    set_spring(state, "SDLStartTextInput", ui_noop);
    set_spring(state, "SDLStopTextInput", ui_noop);
    set_spring(state, "DiffTimers", ui_diff_timers);
    set_spring(state, "GetAIInfo", ui_get_ai_info);
    set_spring(state, "GetAllFeatures", ui_get_all_features);
    set_spring(state, "GetFeaturesInSphere", ui_get_features_in_sphere);
    set_spring(state, "GetFeatureDefID", ui_get_feature_def_id);
    set_spring(state, "GetFeatureResources", ui_get_feature_resources);
    set_spring(state, "GetGroundInfo", ui_get_ground_info);
    set_spring(state, "GetGameRulesParam", ui_get_game_rules_param);
    set_spring(state, "GetUnitMass", ui_get_unit_mass);
    set_spring(state, "GetUnitIsTransporting", ui_get_unit_is_transporting);
    set_spring(state, "GetTeamUnitsSorted", ui_get_team_units_sorted);
    set_spring(state, "PlaySoundFile", ui_play_sound);
    set_spring(state, "GiveOrderToUnit", ui_give_order);
    set_spring(state, "GiveOrder", ui_give_order_selected);
    set_spring(state, "GiveOrderToUnitArray", ui_give_order_array);
    set_spring(state, "GetUnitStates", ui_get_unit_states);
    set_spring(state, "Echo", ui_echo);
    set_spring(state, "GetTeamRulesParams", ui_team_rules_params);
    set_spring(state, "GetUnitIsBuilding", ui_unit_is_building);
    set_spring(state, "IsGodModeEnabled", ui_is_god_mode);
    set_spring(state, "IsCheatingEnabled", ui_is_cheating);
    set_spring(state, "GetTeamRulesParam", ui_team_rules_param);
    set_spring(state, "GetCameraState", ui_get_camera_state);
    set_spring(state, "SetCameraTarget", ui_set_camera_target);
    set_spring(state, "GetGroundHeight", ui_get_ground_height);
    set_spring(state, "GetAllUnits", ui_get_all_units);
    set_spring(state, "GetMiniMapRotation", ui_get_minimap_rotation);
    set_spring(state, "GetActiveCommand", ui_get_active_command_build);
    set_spring(state, "GetMenuName", ui_get_menu_name);
    set_spring(state, "SendLuaMenuMsg", ui_send_lua_menu_msg);
    set_spring(state, "AssignMouseCursor", ui_assign_mouse_cursor);
    set_spring(state, "GetMapDrawMode", ui_get_map_draw_mode);
    set_vfs(state, "SubDirs", ui_subdirs);
    set_vfs(state, "DirList", ui_dir_list);
    set_vfs(state, "LoadFile", ui_load_file);
    set_vfs(state, "FileExists", ui_file_exists);

    if (!assign_include(state, "gl", "R2tHelper", "modules/graphics/r2thelper.lua", error) ||
        !assign_include(state, "BAR", "I18N", "modules/i18n/i18n.lua", error) ||
        !assign_include(state, "BAR", "Lava", "modules/lava.lua", error)) {
        return false;
    }

    if (!install_widget_handler(state, error)) {
        return false;
    }

    if (!run_chunk(state, R"LUA(
Game = Game or {}
Game.textColorCodes = Game.textColorCodes or {
  Color = string.char(255),
  ColorAndOutline = string.char(254),
  Reset = string.char(8),
}
-- Recoil DamageArrayHandler: "default" at 0, then ArmorDefs keys sorted (GetKeys).
-- Bidirectional map name<->index. Categories from gamedata/armordefs.lua (pre-customparams).
-- bar-metal sim spawns no features (GetAllFeatures is empty), so no FeatureDefs are loaded.
FeatureDefs = FeatureDefs or {}
FeatureDefNames = FeatureDefNames or {}
if Game.armorTypes == nil then
  local names = {
    "default",
    "commanders", "crawlingbombs", "hvyboats", "indestructable",
    "lboats", "mines", "nanos", "raptor", "raptorqueen",
    "scavboss", "shields", "standard", "subs", "vtol",
  }
  local t = {}
  for i, name in ipairs(names) do
    local idx = i - 1
    t[idx] = name
    t[name] = idx
  end
  Game.armorTypes = t
end
os = os or {}
os.date = os.date or function() return "19700101_000000" end
local sf = VFS.Include("common/springFunctions.lua")
BAR = BAR or {}
BAR.Utilities = sf.Utilities
BAR.Debug = sf.Debug
)LUA",
                            error)) {
        return false;
    }

    if (!run_chunk(state, R"LUA(
VFS.Include("common/tablefunctions.lua")
)LUA",
                            error)) {
        return false;
    }

    if (!run_chunk(state, R"LUA(
for id, ud in pairs(UnitDefs) do
  if type(ud) == "table" then
    -- Spring defaults for missing UnitDef fields (gui_info compares with > / ==).
    local num = {
      "maxWeaponRange", "metalCost", "energyCost", "buildTime", "buildSpeed",
      "minWaterDepth", "maxThisUnit", "speed", "rSpeed", "health",
      "sightDistance", "airSightDistance", "radarDistance", "sonarDistance",
      "radarDistanceJam", "sonarDistanceJam", "seismicDistance",
      "energyStorage", "metalStorage", "extractsMetal", "armorType",
      "cloakCost", "cloakCostMoving", "transportMass", "transportSize",
      "transportCapacity", "buildDistance",
    }
    for _, k in ipairs(num) do
      if ud[k] == nil then ud[k] = 0 end
    end
    local bools = {
      "isBuilder", "isFactory", "isAirUnit", "isTransport", "stealth",
      "canCloak", "canStockpile", "needGeo", "canAssist", "canReclaim",
      "canRepair", "canRestore", "canResurrect", "canCapture",
    }
    for _, k in ipairs(bools) do
      if ud[k] == nil then ud[k] = false end
    end
    if ud.buildOptions == nil then ud.buildOptions = {} end
    if ud.weapons == nil then ud.weapons = {} end
    if ud.modCategories == nil then ud.modCategories = {} end
    if ud.customParams == nil then ud.customParams = {} end
    -- Recoil UnitDef buildpicname/buildPic: the unit file's buildpic, "" when unset.
    if ud.buildpicname == nil then ud.buildpicname = "" end
    if ud.buildPic == nil then ud.buildPic = ud.buildpicname end
    if ud.translatedHumanName == nil then ud.translatedHumanName = ud.name or tostring(id) end
    if ud.translatedTooltip == nil then ud.translatedTooltip = ud.translatedHumanName end
    if ud.iconType == nil then ud.iconType = "default" end
  end
end
)LUA",
                            error)) {
        return false;
    }

    const std::string font_path = env.game_root + "/luaui/Widgets/gui_fonthandler.lua";
    const std::string flow_path = env.game_root + "/luaui/Widgets/gui_flowui.lua";
    const std::string top_path = env.game_root + "/luaui/Widgets/gui_top_bar.lua";
    const std::string select_path = env.game_root + "/luaui/Widgets/gui_selectionbox.lua";
    const std::string grid_path = env.game_root + "/luaui/Widgets/gui_gridmenu.lua";
    const std::string minimap_path = env.game_root + "/luaui/Widgets/gui_minimap.lua";
    const std::string relative_path = env.game_root + "/luaui/Widgets/minimap_relative.lua";
    const std::string tip_path = env.game_root + "/luaui/Widgets/gui_tooltip.lua";
    const std::string volume_path = env.game_root + "/luaui/Widgets/snd_volume_osd.lua";
    const std::string cursor_path = env.game_root + "/luaui/Widgets/gui_cursor.lua";
    const std::string screen_path = env.game_root + "/luaui/Widgets/gui_screen_mode_info.lua";
    const std::string order_path = env.game_root + "/luaui/Widgets/gui_ordermenu.lua";
    const std::string info_path = env.game_root + "/luaui/Widgets/gui_info.lua";
    build_cmd_set_root(env.game_root);
    hud_status_set_root(env.game_root);
    hud_status_reset();
    build_cmd_reset();
    if (!load_widget(state, font_path, "bar_ui_font", error) || !load_widget(state, flow_path, "bar_ui_flow", error) ||
        !load_widget(state, top_path, "bar_ui_top", error) ||
        !load_widget(state, select_path, "bar_ui_select", error) ||
        !load_widget(state, grid_path, "bar_ui_grid", error) ||
        !load_widget(state, tip_path, "bar_ui_tooltip", error) ||
        !load_widget(state, relative_path, "bar_ui_relative", error) ||
        !load_widget(state, minimap_path, "bar_ui_minimap", error) ||
        !load_widget(state, volume_path, "bar_ui_volume", error) ||
        !load_widget(state, cursor_path, "bar_ui_cursor", error) ||
        !load_widget(state, screen_path, "bar_ui_screenmode", error) ||
        !load_widget(state, order_path, "bar_ui_order", error) ||
        !load_widget(state, info_path, "bar_ui_info", error)) {
        return false;
    }
    if (!call_registry(state, "bar_ui_font", "Initialize", nullptr, 0, true, error) ||
        !call_registry(state, "bar_ui_top", "Initialize", nullptr, 0, true, error) ||
        !call_registry(state, "bar_ui_top", "ViewResize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_select";
    g_select_draw = false;
    g_select_update = false;
    if (!call_registry(state, "bar_ui_select", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_grid";
    g_grid_draw = true;
    g_grid_update = true;
    if (!call_registry(state, "bar_ui_grid", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_tooltip";
    if (!call_registry(state, "bar_ui_tooltip", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_relative";
    if (!call_registry(state, "bar_ui_relative", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_minimap";
    g_minimap_draw = true;
    g_minimap_update = true;
    if (!call_registry(state, "bar_ui_minimap", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_volume";
    g_volume_draw = true;
    if (!call_registry(state, "bar_ui_volume", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_cursor";
    g_cursor_update = false;
    if (!call_registry(state, "bar_ui_cursor", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_screenmode";
    g_screenmode_draw = true;
    if (!call_registry(state, "bar_ui_screenmode", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_order";
    g_order_draw = true;
    g_order_update = true;
    if (!call_registry(state, "bar_ui_order", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_info";
    g_info_draw = true;
    g_info_update = true;
    // Re-pad UnitDefs: earlier widgets may have created stubs via UnitDefNames __index.
    // The pad is kept as a function so later widget loads can re-run it.
    if (!run_chunk(state, "function _bar_pad_unitdefs()\n" R"LUA(
for id, ud in pairs(UnitDefs) do
  if type(ud) == "table" then
    local num = {
      "maxWeaponRange","metalCost","energyCost","buildTime","buildSpeed",
      "minWaterDepth","maxThisUnit","speed","rSpeed","health",
      "sightDistance","airSightDistance","radarDistance","sonarDistance",
      "radarDistanceJam","sonarDistanceJam","seismicDistance",
      "energyStorage","metalStorage","extractsMetal","armorType",
      "cloakCost","cloakCostMoving","transportMass","transportSize",
      "transportCapacity","buildDistance",
      -- Recoil UnitDef defaults for units with no such key; live defs carry
      -- their file values from annotate_roles before this pad runs.
      "energyMake","windGenerator","tidalGenerator",
    }
    for _, k in ipairs(num) do if ud[k] == nil then ud[k] = 0 end end
    local bools = {
      "isBuilder","isFactory","isAirUnit","isTransport","stealth",
      "canCloak","canStockpile","needGeo","canAssist","canReclaim",
      "canRepair","canRestore","canResurrect","canCapture","isBuilding",
    }
    for _, k in ipairs(bools) do if ud[k] == nil then ud[k] = false end end
    if ud.buildOptions == nil then ud.buildOptions = {} end
    if ud.weapons == nil then ud.weapons = {} end
    if ud.modCategories == nil then ud.modCategories = {} end
    if ud.customParams == nil then ud.customParams = {} end
    -- Recoil UnitDef buildpicname/buildPic: the unit file's buildpic, "" when unset.
    if ud.buildpicname == nil then ud.buildpicname = "" end
    if ud.buildPic == nil then ud.buildPic = ud.buildpicname end
  end
end
)LUA" "\nend\n", error)) {
        return false;
    }
    if (!run_chunk(state, R"LUA(
_bar_pad_unitdefs()
-- luaui/setupdefs.lua runs this before any widget loads.
VFS.Include("luaui/i18nhelpers.lua").RefreshDefs()
)LUA",
                            error)) {
        return false;
    }
    if (!call_registry(state, "bar_ui_info", "Initialize", nullptr, 0, true, error)) {
        return false;
    }
    for (ExtraWidget& w : g_extra) {
        std::string werr;
        if (!run_chunk(state, "_bar_pad_unitdefs()", werr)) {
            std::cout << "unitdef pad: " << werr << "\n";
        }
        g_current_widget = w.key;
        const std::string path = env.game_root + "/luaui/Widgets/" + w.file;
        if (!load_widget(state, path, w.key, werr) ||
            !call_registry(state, w.key, "Initialize", nullptr, 0, false, werr)) {
            std::cout << "widget " << w.file << " failed: " << werr << "\n";
            continue;
        }
        w.loaded = true;
        call_registry(state, w.key, "ViewResize", nullptr, 0, false, werr);
    }
    g_current_widget.clear();

    g_applied_w = env.view_w;
    g_applied_h = env.view_h;
    g_next_w = env.view_w;
    g_next_h = env.view_h;
    g_state = state;
    g_loaded = true;
    return true;
}

bool widgets_frame(int frame, std::string& error) {
    g_frame = frame;
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    lua_State* state = g_state;
    // widget:GameStart once the sim has left frame 0, as the widget handler sends it when the game
    // starts. gui_gridmenu leaves its pregame blueprint mode only on this callin.
    if (build_take_commands_changed()) {
        for (const char* key : {"bar_ui_order", "bar_ui_grid"}) {
            g_current_widget = key;
            const bool ok = call_registry(state, key, "CommandsChanged", nullptr, 0, false, error);
            g_current_widget.clear();
            if (!ok) {
                error = std::string(key) + " CommandsChanged: " + error;
                return false;
            }
        }
    }
    static bool s_game_started = false;
    if (!s_game_started && frame > 0) {
        s_game_started = true;
        for (const char* key : {"bar_ui_top", "bar_ui_select", "bar_ui_grid", "bar_ui_tooltip", "bar_ui_minimap",
                                "bar_ui_order", "bar_ui_info"}) {
            g_current_widget = key;
            const bool ok = call_registry(state, key, "GameStart", nullptr, 0, false, error);
            g_current_widget.clear();
            if (!ok) {
                error = std::string(key) + " GameStart: " + error;
                return false;
            }
        }
    }
    if (g_next_w > 0 && g_next_h > 0 && (g_next_w != g_applied_w || g_next_h != g_applied_h)) {
        gl_set_view(g_next_w, g_next_h);
        selection_set_view(g_next_w, g_next_h);
        // Spring ViewResize(vsx, vsy). gui_fonthandler reads the second arg as vsy and
        // crashes if we call it with no args (interactive/fullscreen resize path).
        const double size[2] = {static_cast<double>(g_next_w), static_cast<double>(g_next_h)};
        // Update minimap's view size before widgets reassert geometry (special-case uses g_view_h).
        if (g_terrain != nullptr) {
            minimap_set_world(g_terrain->world_x, g_terrain->world_z, g_next_w, g_next_h);
        }
        if (!call_flow_resize(state, g_next_w, g_next_h, error) ||
            !call_registry(state, "bar_ui_font", "ViewResize", size, 2, false, error) ||
            !call_registry(state, "bar_ui_top", "ViewResize", nullptr, 0, true, error)) {
            return false;
        }
        g_current_widget = "bar_ui_select";
        if (!call_registry(state, "bar_ui_select", "ViewResize", size, 2, false, error)) {
            return false;
        }
        g_current_widget = "bar_ui_grid";
        call_registry(state, "bar_ui_grid", "ViewResize", nullptr, 0, false, error);
        error.clear();
        g_current_widget = "bar_ui_tooltip";
        call_registry(state, "bar_ui_tooltip", "ViewResize", size, 2, false, error);
        error.clear();
        g_current_widget = "bar_ui_relative";
        call_registry(state, "bar_ui_relative", "ViewResize", size, 2, false, error);
        error.clear();
        g_current_widget = "bar_ui_minimap";
        if (!call_registry(state, "bar_ui_minimap", "ViewResize", nullptr, 0, false, error)) {
            return false;
        }
        g_current_widget = "bar_ui_volume";
        call_registry(state, "bar_ui_volume", "ViewResize", size, 2, false, error);
        error.clear();
        g_current_widget = "bar_ui_cursor";
        call_registry(state, "bar_ui_cursor", "ViewResize", nullptr, 0, false, error);
        error.clear();
        g_current_widget = "bar_ui_screenmode";
        call_registry(state, "bar_ui_screenmode", "ViewResize", nullptr, 0, false, error);
        error.clear();
        g_current_widget = "bar_ui_order";
        call_registry(state, "bar_ui_order", "ViewResize", nullptr, 0, false, error);
        error.clear();
        g_current_widget = "bar_ui_info";
        call_registry(state, "bar_ui_info", "ViewResize", nullptr, 0, false, error);
        error.clear();
        for (ExtraWidget& w : g_extra) {
            if (w.loaded) {
                g_current_widget = w.key;
                call_registry(state, w.key, "ViewResize", nullptr, 0, false, error);
                error.clear();
            }
        }
        g_current_widget.clear();
        g_applied_w = g_next_w;
        g_applied_h = g_next_h;
        std::cout << "widgets view " << g_applied_w << "x" << g_applied_h << "\n";
    }
    gl_begin_draw();
    const double game_frame = frame;
    const double step = 1.0 / 30.0;
    if (!call_registry(state, "bar_ui_top", "GameFrame", &game_frame, 1, false, error) ||
        !call_registry(state, "bar_ui_top", "Update", &step, 1, true, error) ||
        !call_registry(state, "bar_ui_top", "DrawScreen", nullptr, 0, true, error)) {
        return false;
    }
    g_current_widget = "bar_ui_select";
    if (g_select_update) {
        if (!call_registry(state, "bar_ui_select", "Update", &step, 1, true, error)) {
            return false;
        }
    }
    if (g_select_draw) {
        if (!call_registry(state, "bar_ui_select", "DrawScreen", nullptr, 0, true, error)) {
            return false;
        }
    }
    g_current_widget = "bar_ui_grid";
    if (g_grid_update) {
        if (!call_registry(state, "bar_ui_grid", "Update", &step, 1, false, error)) {
            return false;
        }
    }
    // Recoil draws gridmenu whenever a builder is selected (it hides itself otherwise),
    // and while placing.
    const bool draw_grid = g_grid_draw && (build_cmd_state().placing || !selection_snapshot().selected.empty());
    if (draw_grid) {
        if (!call_registry(state, "bar_ui_grid", "DrawScreen", nullptr, 0, false, error)) {
            return false;
        }
    }
    g_current_widget = "bar_ui_minimap";
    if (g_minimap_update) {
        if (!call_registry(state, "bar_ui_minimap", "Update", &step, 1, false, error)) {
            return false;
        }
    }
    if (g_minimap_draw) {
        if (!call_registry(state, "bar_ui_minimap", "DrawScreen", nullptr, 0, false, error)) {
            return false;
        }
    }
    g_current_widget = "bar_ui_volume";
    if (g_volume_draw) {
        call_registry(state, "bar_ui_volume", "DrawScreen", nullptr, 0, false, error);
        error.clear();
    }
    g_current_widget = "bar_ui_cursor";
    if (g_cursor_update) {
        call_registry(state, "bar_ui_cursor", "Update", &step, 1, false, error);
        error.clear();
    }
    g_current_widget = "bar_ui_screenmode";
    if (g_screenmode_draw) {
        call_registry(state, "bar_ui_screenmode", "DrawScreen", nullptr, 0, false, error);
        error.clear();
    }
    g_current_widget = "bar_ui_order";
    if (g_order_update) {
        call_registry(state, "bar_ui_order", "Update", &step, 1, false, error);
        error.clear();
    }
    if (g_order_draw) {
        // Only draw when something is selected (matches Recoil bottom order panel).
        if (!selection_snapshot().selected.empty()) {
            if (!call_registry(state, "bar_ui_order", "DrawScreen", nullptr, 0, false, error)) {
                std::cout << "ordermenu DrawScreen: " << error << "\n";
                error.clear();
            }
        }
    }
    g_current_widget = "bar_ui_info";
    if (g_info_update) {
        call_registry(state, "bar_ui_info", "Update", &step, 1, false, error);
        error.clear();
    }
    if (g_info_draw && !selection_snapshot().selected.empty()) {
        if (!call_registry(state, "bar_ui_info", "DrawScreen", nullptr, 0, false, error)) {
            std::cout << "info DrawScreen: " << error << "\n";
            error.clear();
        }
    }
    if (!g_console_lines.empty()) {
        std::vector<std::string> lines;
        lines.swap(g_console_lines);
        for (const ExtraWidget& w : g_extra) {
            if (!w.loaded) {
                continue;
            }
            g_current_widget = w.key;
            for (const std::string& line : lines) {
                lua_getfield(state, LUA_REGISTRYINDEX, w.key);
                lua_getfield(state, -1, "AddConsoleLine");
                if (!lua_isfunction(state, -1)) {
                    lua_pop(state, 2);
                    break;
                }
                lua_pushvalue(state, -2);
                lua_pushlstring(state, line.data(), line.size());
                lua_pushinteger(state, 0);  // LOG.INFO priority, as Spring.Echo uses
                lua_remove(state, -5);
                if (!pcall_keep(state, 3, 0, error)) {
                    std::cout << w.file << " AddConsoleLine: " << error << "\n";
                    error.clear();
                }
            }
        }
    }
    for (ExtraWidget& w : g_extra) {
        if (!w.loaded) {
            continue;
        }
        g_current_widget = w.key;
        if (w.update && !call_registry(state, w.key, "Update", &step, 1, false, error)) {
            std::cout << w.file << " Update: " << error << "\n";
        }
        error.clear();
        if (w.draw && !call_registry(state, w.key, "DrawScreen", nullptr, 0, false, error)) {
            std::cout << w.file << " DrawScreen: " << error << "\n";
        }
        error.clear();
    }
    // Drive stock WG.tooltip BEFORE DrawScreen. ShowTooltip must run every frame
    // (one-shot disabled=true after draw) and twice on first create (stock sets value
    // only on the update path). Point the mouse at the hover so the tip sits near the cursor.
    if (!hud_status_tooltip().empty()) {
        widgets_set_pointer(hud_status_hover_x(), hud_status_hover_y(), false, false, false, false, false, false,
                            false);
        auto show_tip = [&]() {
            lua_getglobal(state, "WG");
            if (!lua_istable(state, -1)) {
                lua_pop(state, 1);
                return;
            }
            lua_getfield(state, -1, "tooltip");
            if (!lua_istable(state, -1)) {
                lua_pop(state, 2);
                return;
            }
            lua_getfield(state, -1, "ShowTooltip");
            if (!lua_isfunction(state, -1)) {
                lua_pop(state, 3);
                return;
            }
            lua_pushstring(state, "bar_hud_unit");
            lua_pushstring(state, hud_status_tooltip().c_str());
            lua_pushnumber(state, static_cast<double>(hud_status_hover_x()));
            lua_pushnumber(state, static_cast<double>(hud_status_hover_y()));
            if (lua_pcall(state, 4, 0, 0) != 0) {
                lua_pop(state, 1);
            }
            lua_pop(state, 2);  // tooltip, WG
        };
        show_tip();
        show_tip();
    }
    g_current_widget = "bar_ui_tooltip";
    {
        std::string tip_error;
        if (!call_registry(state, "bar_ui_tooltip", "DrawScreen", nullptr, 0, true, tip_error)) {
            std::cout << "tooltip DrawScreen: " << tip_error << "\n";
        }
    }
    error.clear();
    g_current_widget.clear();
    g_hud = gl_publish();
    // Drop FlowUI chrome quads that sit on the minimap (drawn after the map texture)
    // and large empty grey panels mid-left (unhosted/empty build chrome).
    {
        const MinimapGeometry& geo = minimap_geometry();
        const float view_h = static_cast<float>(gl_view_height());
        const float view_w = static_cast<float>(gl_view_width());
        const float mmap_l = 0.f;
        const float mmap_r = static_cast<float>(geo.sx) + 24.f;
        const float mmap_b = view_h - static_cast<float>(geo.sy) - 24.f;
        const float mmap_t = view_h;
        std::vector<HudVertex> kept;
        kept.reserve(g_hud.vertices.size());
        for (size_t i = 0; i + 3 < g_hud.vertices.size(); i += 4) {
            const HudVertex& a = g_hud.vertices[i];
            const HudVertex& b = g_hud.vertices[i + 1];
            const HudVertex& c = g_hud.vertices[i + 2];
            const HudVertex& d = g_hud.vertices[i + 3];
            const float cx = 0.25f * (a.x + b.x + c.x + d.x);
            const float cy = 0.25f * (a.y + b.y + c.y + d.y);
            const float cr = 0.25f * (a.r + b.r + c.r + d.r);
            const float cg = 0.25f * (a.g + b.g + c.g + d.g);
            const float cb = 0.25f * (a.b + b.b + c.b + d.b);
            const float aw = std::fabs(a.x - c.x);
            const float ah = std::fabs(a.y - c.y);
            const bool grey = cr < 0.4f && cg < 0.4f && cb < 0.4f && cr + cg + cb < 0.9f;
            const bool in_mmap = geo.sx > 0 && cx >= mmap_l && cx <= mmap_r && cy >= mmap_b && cy <= mmap_t;
            // Empty mid-left panel: large grey rect on the left third, not topbar/minimap.
            const bool empty_panel = grey && aw > 120.f && ah > 120.f && cx < view_w * 0.35f &&
                                     cy < view_h * 0.75f && cy > view_h * 0.15f;
            if ((in_mmap && grey) || empty_panel) {
                continue;
            }
            kept.push_back(a);
            kept.push_back(b);
            kept.push_back(c);
            kept.push_back(d);
        }
        g_hud.vertices.swap(kept);
    }
    selection_append_highlights(g_hud);
    if (g_have_view_proj) {
        build_append_preview(g_hud, g_view_proj, gl_view_width(), gl_view_height());
    }
    minimap_append_draw(g_hud, g_sim);
    if (g_sim != nullptr && g_terrain != nullptr) {
        hud_status_append(g_hud, *g_sim, *g_terrain);
    }
    return true;
}

void widgets_set_view(int width, int height) {
    if (width > 0 && height > 0) {
        g_next_w = width;
        g_next_h = height;
    }
}


bool widgets_selection_changed(const std::vector<int>& unit_ids, std::string& error) {
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    lua_State* state = g_state;
    g_current_widget = "bar_ui_grid";
    lua_getfield(state, LUA_REGISTRYINDEX, "bar_ui_grid");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        g_current_widget.clear();
        return true;
    }
    lua_getfield(state, -1, "SelectionChanged");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        g_current_widget.clear();
        return true;
    }
    lua_pushvalue(state, -2);
    lua_newtable(state);
    for (size_t i = 0; i < unit_ids.size(); ++i) {
        lua_pushinteger(state, unit_ids[i]);
        lua_rawseti(state, -2, static_cast<int>(i) + 1);
    }
    // Drop the registry widget under the function: stack is widget, fn, self, sel.
    lua_remove(state, -4);
    const bool ok = pcall_keep(state, 2, 0, error);
    g_current_widget.clear();
    return ok;
}

void widgets_set_sim(UnitSim* sim, const TerrainMesh* terrain) {
    g_sim = sim;
    g_terrain = terrain;
    static std::string s_live_key;
    std::string live_key;
    if (sim != nullptr) {
        for (const std::string& nm : sim->def_name) {
            if (live_key.find("|" + nm + "|") == std::string::npos) live_key += "|" + nm + "|";
        }
    }
    if (g_loaded && g_state != nullptr && sim != nullptr && live_key != s_live_key) {
        s_live_key = live_key;
        // Unit-file stats for the live defs, then the stock def-refresh callin
        // (gui_info LanguageChanged reruns refreshUnitInfo).
        fill_live_def_stats(g_state, g_env.game_root);
        {
            std::string werr;
            if (!run_chunk(g_state, R"LUA(
-- Recoil WeaponDef conversion of the unit file's weapondefs / weapons for live defs.
WeaponDefs = WeaponDefs or {}
WeaponDefNames = WeaponDefNames or {}
local function nextWeaponID()
  local n = -1
  for id in pairs(WeaponDefs) do if type(id) == "number" and id > n then n = id end end
  return n + 1
end
local function lowerKeys(t)
  local out = {}
  if type(t) == "table" then for k, v in pairs(t) do out[type(k) == "string" and k:lower() or k] = v end end
  return out
end
-- Recoil customParams are strings.
local function stringParams(t)
  local out = {}
  for k, v in pairs(lowerKeys(t)) do out[k] = tostring(v) end
  return out
end
for _, unitName in ipairs(Spring._liveDefNames or {}) do
  local ud = UnitDefNames[unitName]
  local file = Spring._unitFileTable(unitName)
  if ud and file and type(file.weapondefs) == "table" then
    local byKey = {}
    -- gamedata/alldefs_post: weapons_group = file value (default 0) if a weapon slot uses it, else -1.
    local used = {}
    for _, w in ipairs(file.weapons or {}) do if w.def then used[w.def:lower()] = true end end
    for key, wd in pairs(file.weapondefs) do
      wd.customparams = wd.customparams or {}
      wd.customparams.weapons_group = used[key:lower()] and (tonumber(wd.customparams.weapons_group or 0) or 0) or -1
    end
    for key, wd in pairs(file.weapondefs) do
      local name = (unitName .. "_" .. key):lower()
      local def = WeaponDefNames[name]
      if not def then
        local id = nextWeaponID()
        local dmg = lowerKeys(wd.damage)
        local base = dmg.default or 0
        local damages = { paralyzeDamageTime = wd.paralyzetime or 0 }
        for i = 0, #Game.armorTypes do damages[i] = dmg[Game.armorTypes[i]] or base end
        local shield = lowerKeys(wd.shield)
        def = {
          id = id, name = name, type = wd.weapontype or "Cannon",
          range = wd.range or 0, reload = wd.reloadtime or 1,
          salvoSize = wd.burst or 1, salvoDelay = wd.burstrate or 0.1,
          projectiles = wd.projectiles or 1, damages = damages,
          energyCost = wd.energypershot or 0, metalCost = wd.metalpershot or 0,
          beamtime = wd.beamtime or 1, beamburst = wd.beamburst or false,
          stockpile = wd.stockpile or false, stockpileTime = (wd.stockpiletime or 0) * Game.gameSpeed,
          interceptor = wd.interceptor or 0, coverageRange = wd.coverage or 0,
          shieldRadius = shield.radius or 0, shieldPower = shield.power or 0,
          shieldPowerRegen = shield.powerregen or 0, shieldPowerRegenEnergy = shield.powerregenenergy or 0,
          paralyzer = wd.paralyzer or false, minIntensity = wd.minintensity or 0, customParams = stringParams(wd.customparams),
        }
        WeaponDefs[id] = def
        WeaponDefNames[name] = def
      end
      byKey[key:lower()] = def
    end
    local weapons = {}
    for i, w in ipairs(file.weapons or {}) do
      local def = w.def and byKey[w.def:lower()]
      if def then
        local only = {}
        for cat in tostring(w.onlytargetcategory or ""):gmatch("%S+") do only[cat:lower()] = true end
        weapons[#weapons + 1] = { weaponDef = def.id, onlyTargets = only }
      end
    end
    ud.weapons = weapons
  end
end
-- Weapon state for a live unit: the def values (bar-metal has no per-unit weapon modifiers).
local stateKey = { range = "range", reloadTime = "reload", projectiles = "projectiles", burst = "salvoSize" }
Spring.GetUnitWeaponState = function(unitID, weaponNum, key)
  local defID = Spring.GetUnitDefID(unitID)
  local ud = defID and UnitDefs[defID]
  local w = ud and ud.weapons and ud.weapons[weaponNum]
  local wd = w and WeaponDefs[w.weaponDef]
  if not wd then return nil end
  local field = stateKey[key]
  if field then return wd[field] end
  return nil
end
)LUA",
                           werr)) {
                std::cout << "weapondefs: " << werr << "\n";
            }
        }
        std::string error;
        g_current_widget = "bar_ui_info";
        if (!call_registry(g_state, "bar_ui_info", "LanguageChanged", nullptr, 0, false, error)) {
            std::cout << "info LanguageChanged: " << error << "\n";
        }
        g_current_widget.clear();
    }
}

void widgets_set_view_proj(const float* view_proj16) {
    if (view_proj16 == nullptr) {
        g_have_view_proj = false;
        return;
    }
    for (int i = 0; i < 16; ++i) {
        g_view_proj[i] = view_proj16[i];
    }
    g_have_view_proj = true;
}

void widgets_set_pointer(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift) {
    g_mouse_x = x;
    g_mouse_y = y;
    g_mouse_left = left;
    g_mouse_middle = middle;
    g_mouse_right = right;
    g_mod_alt = alt;
    g_mod_ctrl = ctrl;
    g_mod_meta = meta;
    g_mod_shift = shift;
    selection_set_mouse(x, y, left, middle, right, alt, ctrl, meta, shift);
}

namespace {

// Widget that took the last MousePress (Recoil widgetHandler: it gets the matching MouseRelease).
std::string g_mouse_owner;

// Calls key:method(x, y, button) and reads a true first result as "consumed".
bool call_mouse(lua_State* state, const char* key, const char* method, float x, float y, int button,
                bool& consumed, std::string& error) {
    consumed = false;
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    lua_getfield(state, -1, method);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        return true;
    }
    lua_insert(state, -2);  // fn, self
    lua_pushnumber(state, x);
    lua_pushnumber(state, y);
    lua_pushnumber(state, button);
    g_current_widget = key;
    const bool ok = pcall_keep(state, 4, 1, error);
    g_current_widget.clear();
    if (!ok) {
        return false;
    }
    consumed = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return true;
}

}  // namespace

bool widgets_mouse_press(float x, float y, int button, bool& consumed, std::string& error) {
    consumed = false;
    g_mouse_owner.clear();
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    // Front-most layers first, as the widget handler walks them: order menu, grid menu, info,
    // minimap, then the selection box (which never takes the press).
    static const char* kOrder[] = {"bar_ui_order", "bar_ui_grid", "bar_ui_info", "bar_ui_minimap", "bar_ui_select"};
    for (const char* key : kOrder) {
        bool took = false;
        if (!call_mouse(g_state, key, "MousePress", x, y, button, took, error)) {
            error = std::string(key) + " MousePress: " + error;
            return false;
        }
        if (took) {
            consumed = true;
            g_mouse_owner = key;
            std::cout << "ui press " << key << " at " << x << " " << y << " button " << button << "\n";
            return true;
        }
    }
    return true;
}

bool widgets_mouse_release(float x, float y, int button, std::string& error) {
    if (!g_loaded || g_state == nullptr || g_mouse_owner.empty()) {
        return true;
    }
    const std::string owner = g_mouse_owner;
    g_mouse_owner.clear();
    bool ignored = false;
    return call_mouse(g_state, owner.c_str(), "MouseRelease", x, y, button, ignored, error);
}

bool widgets_selection_changed_live(const std::vector<int>& unit_ids, std::string& error) {
    static std::vector<int> s_last;
    static bool s_have = false;
    if (s_have && s_last == unit_ids) {
        return true;
    }
    s_have = true;
    s_last = unit_ids;
    if (!widgets_selection_changed(unit_ids, error)) {
        return false;
    }
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    for (const char* key : {"bar_ui_order", "bar_ui_info"}) {
        lua_getfield(g_state, LUA_REGISTRYINDEX, key);
        if (!lua_istable(g_state, -1)) {
            lua_pop(g_state, 1);
            continue;
        }
        lua_getfield(g_state, -1, "SelectionChanged");
        if (!lua_isfunction(g_state, -1)) {
            lua_pop(g_state, 2);
            continue;
        }
        lua_insert(g_state, -2);
        lua_newtable(g_state);
        for (size_t i = 0; i < unit_ids.size(); ++i) {
            lua_pushinteger(g_state, unit_ids[i]);
            lua_rawseti(g_state, -2, static_cast<int>(i) + 1);
        }
        g_current_widget = key;
        const bool ok = pcall_keep(g_state, 2, 0, error);
        g_current_widget.clear();
        if (!ok) {
            error = std::string(key) + " SelectionChanged: " + error;
            return false;
        }
    }
    return true;
}

const HudList* hud_list() {
    if (!g_loaded || (g_hud.vertices.empty() && g_hud.texts.empty() && !g_hud.minimap_active && !g_hud.minimap_marker)) {
        return nullptr;
    }
    return &g_hud;
}

void widgets_set_camera(Camera* camera) {
    g_camera = camera;
}

}  // namespace bar
