#include "minimap.h"

#include "selection.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace bar {
namespace {

MinimapGeometry g_geo;
float g_world_x = 1;
float g_world_z = 1;
int g_view_w = 1280;
int g_view_h = 720;

float g_last_world_x = 0;
float g_last_world_z = 0;
float g_last_screen_x = 0;
float g_last_screen_y = 0;
int g_last_move = 0;

}  // namespace

const MinimapGeometry& minimap_geometry() {
    return g_geo;
}

void minimap_reset() {
    g_geo = MinimapGeometry{};
    g_last_world_x = 0;
    g_last_world_z = 0;
    g_last_screen_x = 0;
    g_last_screen_y = 0;
    g_last_move = 0;
}

void minimap_set_world(float world_x, float world_z, int view_w, int view_h) {
    g_world_x = world_x > 1.f ? world_x : 1.f;
    g_world_z = world_z > 1.f ? world_z : 1.f;
    g_view_w = view_w > 0 ? view_w : 1280;
    g_view_h = view_h > 0 ? view_h : 720;
}

void minimap_set_geometry(int px, int py, int sx, int sy) {
    // BAR's gui_minimap sends top-left engine coords (0,0,w,h) for a top-docked
    // panel. Spring.GetMiniMapGeometry reports bottom-left screen coords.
    g_geo.sx = sx > 0 ? sx : 0;
    g_geo.sy = sy > 0 ? sy : 0;
    if (px == 0 && py == 0 && g_geo.sy > 0) {
        g_geo.px = 0;
        g_geo.py = g_view_h - g_geo.sy;
    } else {
        g_geo.px = px;
        g_geo.py = py;
    }
    if (g_geo.sx > 0 && g_geo.sy > 0) {
        g_geo.minimized = false;
    }
}

void minimap_set_minimized(bool minimized) {
    g_geo.minimized = minimized;
}

void minimap_set_slave(bool slave) {
    g_geo.slave = slave;
}

bool minimap_send_command(const std::string& command) {
    std::istringstream in(command);
    std::string head;
    in >> head;
    if (head != "minimap") {
        return false;
    }
    std::string action;
    in >> action;
    if (action == "geometry") {
        int px = 0;
        int py = 0;
        int sx = 0;
        int sy = 0;
        in >> px >> py >> sx >> sy;
        minimap_set_geometry(px, py, sx, sy);
        return true;
    }
    if (action == "minimize") {
        int value = 1;
        in >> value;
        minimap_set_minimized(value != 0);
        return true;
    }
    if (action == "unitsize") {
        return true;  // icon size: engine draws a single unit-0 marker; scale unused
    }
    return true;  // other minimap commands acknowledged, no-op
}

bool minimap_to_world(float mouse_x, float mouse_y, float view_h, float& world_x, float& world_y, float& world_z,
                      const TerrainMesh& terrain) {
    (void)view_h;
    if (g_geo.minimized || g_geo.sx <= 0 || g_geo.sy <= 0) {
        return false;
    }
    // Match luaui/Include/minimap_utils.lua at rotation 0. Geometry is bottom-left.
    const float px = static_cast<float>(g_geo.px);
    const float py = static_cast<float>(g_geo.py);
    const float sx = static_cast<float>(g_geo.sx);
    const float sy = static_cast<float>(g_geo.sy);
    const float nx = (mouse_x - px) / sx;
    const float nz = 1.f - (mouse_y - py) / sy;
    if (nx < 0.f || nx > 1.f || nz < 0.f || nz > 1.f) {
        return false;
    }
    world_x = nx * g_world_x;
    world_z = nz * g_world_z;
    world_y = terrain_height_at(terrain, world_x, world_z);
    return true;
}

bool minimap_click(float mouse_x, float mouse_y, UnitSim& sim, const TerrainMesh& terrain, Camera& camera,
                   bool issue_move, std::string& error) {
    float wx = 0;
    float wy = 0;
    float wz = 0;
    if (!minimap_to_world(mouse_x, mouse_y, static_cast<float>(g_view_h), wx, wy, wz, terrain)) {
        error = "minimap click outside geometry";
        return false;
    }
    g_last_screen_x = mouse_x;
    g_last_screen_y = mouse_y;
    g_last_world_x = wx;
    g_last_world_z = wz;
    camera.target_x = wx;
    camera.target_y = wy + 30.f;
    camera.target_z = wz;
    std::cout << "minimap click " << mouse_x << " " << mouse_y << " world " << wx << " " << wz << "\n";
    if (issue_move && !selection_snapshot().selected.empty()) {
        if (!selection_issue_move(sim, wx, wz, terrain, error)) {
            return false;
        }
        g_last_move = 1;
    }
    return true;
}

bool load_minimap_events(const std::string& path, std::vector<MinimapEvent>& events, std::string& error) {
    events.clear();
    std::ifstream in(path);
    if (!in) {
        error = "could not read " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        int frame = 0;
        std::string kind;
        row >> frame >> kind;
        if (kind != "mmap" && kind != "minimap") {
            continue;
        }
        MinimapEvent event;
        event.frame = frame;
        row >> event.screen_x >> event.screen_y;
        events.push_back(event);
    }
    return true;
}

bool apply_minimap_events(UnitSim& sim, const std::vector<MinimapEvent>& events, int presented,
                          const TerrainMesh& terrain, Camera& camera, std::string& error) {
    for (const MinimapEvent& event : events) {
        if (event.frame != presented) {
            continue;
        }
        if (!minimap_click(event.screen_x, event.screen_y, sim, terrain, camera, event.issue_move, error)) {
            return false;
        }
    }
    return true;
}

void minimap_append_draw(HudList& hud, const UnitSim* sim) {
    if (g_geo.minimized || !g_geo.slave || g_geo.sx <= 0 || g_geo.sy <= 0) {
        return;
    }
    const float left = static_cast<float>(g_geo.px);
    const float right = left + static_cast<float>(g_geo.sx);
    const float bottom = static_cast<float>(g_geo.py);
    const float top = bottom + static_cast<float>(g_geo.sy);
    // Signal textured panel via special alpha sentinel in first quad? Use HudList fields.
    hud.minimap_active = true;
    hud.minimap_left = left;
    hud.minimap_right = right;
    hud.minimap_bottom = bottom;
    hud.minimap_top = top;
    hud.minimap_marker = false;
    // Unit 0 marker (only unit drawn on the minimap per PIN). Drawn after the map
    // texture in encode_hud so it stays visible at any drawable size.
    if (sim != nullptr && !sim->id.empty()) {
        const float u = sim->x[0] / g_world_x;
        const float v = sim->z[0] / g_world_z;  // 0 at north/top of panel
        const float panel = std::max(right - left, top - bottom);
        const float half = std::max(4.f, panel * 0.018f);
        hud.minimap_marker = true;
        hud.minimap_marker_x = left + u * (right - left);
        hud.minimap_marker_y = top - v * (top - bottom);
        hud.minimap_marker_half = half;
    }
}

bool write_minimap_report(const std::string& path, int frame, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write minimap report " + path;
        return false;
    }
    out << "{\"f\":" << frame << ",\"px\":" << g_geo.px << ",\"py\":" << g_geo.py << ",\"sx\":" << g_geo.sx
        << ",\"sy\":" << g_geo.sy << ",\"minimized\":" << (g_geo.minimized ? 1 : 0)
        << ",\"slave\":" << (g_geo.slave ? 1 : 0) << ",\"clickX\":" << g_last_screen_x
        << ",\"clickY\":" << g_last_screen_y << ",\"worldX\":" << g_last_world_x << ",\"worldZ\":" << g_last_world_z
        << ",\"moved\":" << g_last_move << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar
