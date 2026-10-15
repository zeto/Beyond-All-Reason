#pragma once

#include "gl/draw_list.h"
#include "camera.h"
#include "move/unit_sim.h"
#include "smf.h"

#include <string>
#include <vector>

namespace bar {

struct MinimapGeometry {
    int px = 0;  // from top-left of the view (Spring engine convention)
    int py = 0;
    int sx = 0;
    int sy = 0;
    bool minimized = true;
    bool maximized = false;
    bool slave = false;
};

struct MinimapEvent {
    int frame = 0;
    float screen_x = 0;  // Spring mouse: y up from bottom
    float screen_y = 0;
    bool issue_move = true;
};

const MinimapGeometry& minimap_geometry();

void minimap_reset();

void minimap_set_world(float world_x, float world_z, int view_w, int view_h);

void minimap_set_geometry(int px, int py, int sx, int sy);

void minimap_set_minimized(bool minimized);

void minimap_set_slave(bool slave);

// Parse engine "minimap …" SendCommands lines. Returns true when handled.
bool minimap_send_command(const std::string& command);

// Spring mouse (bottom-left origin) → world. false when outside the minimap.
bool minimap_to_world(float mouse_x, float mouse_y, float view_h, float& world_x, float& world_y, float& world_z,
                      const TerrainMesh& terrain);

// Left-click: SetCameraTarget and optionally issue a move for the selection.
bool minimap_click(float mouse_x, float mouse_y, UnitSim& sim, const TerrainMesh& terrain, Camera& camera,
                   bool issue_move, std::string& error);

bool load_minimap_events(const std::string& path, std::vector<MinimapEvent>& events, std::string& error);

bool apply_minimap_events(UnitSim& sim, const std::vector<MinimapEvent>& events, int presented,
                          const TerrainMesh& terrain, Camera& camera, std::string& error);

// Map texture quad + unit-0 marker into the HUD list / minimap panel.
void minimap_append_draw(HudList& hud, const UnitSim* sim);

bool write_minimap_report(const std::string& path, int frame, std::string& error);

}  // namespace bar
