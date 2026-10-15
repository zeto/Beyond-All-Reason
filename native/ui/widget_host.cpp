#include "widget_host.h"

#include "gl/gl_api.h"
#include "selection.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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
    return 2;
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
        names.push_back(std::string(relative) + "/" + it->path().filename().string());
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
    lua_newtable(state);
    lua_setglobal(state, "widget");
    if (luaL_loadfile(state, path.c_str()) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "widget load failed";
        lua_pop(state, 1);
        return false;
    }
    if (!pcall_keep(state, 0, 0, error)) {
        return false;
    }
    lua_getglobal(state, "widget");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        error = path + " did not leave a widget table";
        return false;
    }
    lua_setfield(state, LUA_REGISTRYINDEX, key);
    return true;
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
    // Minimap is not hosted yet. Returning minimized keeps the selectionbox off that path.
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushnumber(state, 0);
    lua_pushboolean(state, 1);  // minimized
    lua_pushboolean(state, 0);
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
    }
    return 0;
}

bool install_widget_handler(lua_State* state, std::string& error) {
    lua_newtable(state);
    lua_pushcfunction(state, ui_handler_update_callin);
    lua_setfield(state, -2, "UpdateCallIn");
    lua_pushcfunction(state, ui_handler_remove_callin);
    lua_setfield(state, -2, "RemoveCallIn");
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

    if (!run_chunk(state, R"LUA(
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
WG = WG or {}
LUAUI_DIRNAME = "luaui/"
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
    lua_pop(state, 1);

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
    set_spring(state, "SendCommands", ui_noop);
    set_spring(state, "SetMouseCursor", ui_set_mouse_cursor);
    set_spring(state, "GetMouseCursor", ui_get_mouse_cursor);
    set_spring(state, "GetSelectionBox", ui_get_selection_box);
    set_spring(state, "GetModKeyState", ui_get_mod_keys);
    set_spring(state, "GetMiniMapGeometry", ui_get_minimap_geometry);
    set_spring(state, "GetSelectedUnits", ui_get_selected_units);
    set_spring(state, "GetActiveCommand", ui_get_active_command);
    set_spring(state, "LoadCmdColorsConfig", ui_load_cmd_colors);
    set_spring(state, "ReplaceMouseCursor", ui_replace_mouse_cursor);
    set_spring(state, "GetScreenGeometry", ui_get_screen_geometry);
    set_spring(state, "SetConfigFloat", ui_set_config_float);
    set_spring(state, "SetConfigInt", ui_noop);
    set_spring(state, "SetConfigString", ui_noop);
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

    const std::string font_path = env.game_root + "/luaui/Widgets/gui_fonthandler.lua";
    const std::string flow_path = env.game_root + "/luaui/Widgets/gui_flowui.lua";
    const std::string top_path = env.game_root + "/luaui/Widgets/gui_top_bar.lua";
    const std::string select_path = env.game_root + "/luaui/Widgets/gui_selectionbox.lua";
    if (!load_widget(state, font_path, "bar_ui_font", error) || !load_widget(state, flow_path, "bar_ui_flow", error) ||
        !load_widget(state, top_path, "bar_ui_top", error) ||
        !load_widget(state, select_path, "bar_ui_select", error)) {
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
    // Initialize already calls ViewResize(GetViewGeometry()).
    if (!call_registry(state, "bar_ui_select", "Initialize", nullptr, 0, true, error)) {
        return false;
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
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    lua_State* state = g_state;
    if (g_next_w > 0 && g_next_h > 0 && (g_next_w != g_applied_w || g_next_h != g_applied_h)) {
        gl_set_view(g_next_w, g_next_h);
        selection_set_view(g_next_w, g_next_h);
        if (!call_flow_resize(state, g_next_w, g_next_h, error) ||
            !call_registry(state, "bar_ui_font", "ViewResize", nullptr, 0, false, error) ||
            !call_registry(state, "bar_ui_top", "ViewResize", nullptr, 0, true, error)) {
            return false;
        }
        g_current_widget = "bar_ui_select";
        {
            const double size[2] = {static_cast<double>(g_next_w), static_cast<double>(g_next_h)};
            if (!call_registry(state, "bar_ui_select", "ViewResize", size, 2, false, error)) {
                return false;
            }
        }
        g_current_widget.clear();
        g_applied_w = g_next_w;
        g_applied_h = g_next_h;
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
    g_current_widget.clear();
    g_hud = gl_publish();
    selection_append_highlights(g_hud);
    return true;
}

void widgets_set_view(int width, int height) {
    if (width > 0 && height > 0) {
        g_next_w = width;
        g_next_h = height;
    }
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

bool widgets_mouse_press(float x, float y, int button, std::string& error) {
    if (!g_loaded || g_state == nullptr) {
        return true;
    }
    g_current_widget = "bar_ui_select";
    const double args[3] = {x, y, static_cast<double>(button)};
    const bool ok = call_registry(g_state, "bar_ui_select", "MousePress", args, 3, false, error);
    g_current_widget.clear();
    return ok;
}

const HudList* hud_list() {
    if (!g_loaded || (g_hud.vertices.empty() && g_hud.texts.empty())) {
        return nullptr;
    }
    return &g_hud;
}

}  // namespace bar
