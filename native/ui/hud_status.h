#pragma once

#include "gl/draw_list.h"
#include "camera.h"
#include "move/unit_sim.h"
#include "smf.h"

#include <string>
#include <vector>

namespace bar {

struct HudStatusEvent {
    int frame = 0;
    std::string kind;  // hover | damage
    float screen_x = 0;
    float screen_y = 0;
    float health_frac = 1;  // for damage: set health to frac * max
};

void hud_status_reset();

void hud_status_set_root(const std::string& game_root);
// gui_healthbars_gl4 inputs for unit 0: UnitDef height and radius, power and max health.
void hud_status_set_unit(float height, float radius, float power, float max_health);

void hud_status_set_camera(const Camera* camera, const CameraPose* pose);

// Apply hover from screen coords (Spring mouse). Sets tooltip from unit under cursor.
void hud_status_hover(float screen_x, float screen_y, const UnitSim& sim, const TerrainMesh& terrain);

// Reduce unit health to frac * max_health (sim rules: direct health write like SetUnitHealth).
bool hud_status_damage(UnitSim& sim, int spring_unit_id, float health_frac, std::string& error);

void hud_status_append(HudList& hud, const UnitSim& sim, const TerrainMesh& terrain);

bool load_hud_status_events(const std::string& path, std::vector<HudStatusEvent>& events, std::string& error);

bool apply_hud_status_events(UnitSim& sim, const std::vector<HudStatusEvent>& events, int presented,
                             const TerrainMesh& terrain, std::string& error);

bool write_hud_status_report(const std::string& path, int frame, std::string& error);

const std::string& hud_status_tooltip();
float hud_status_health();
float hud_status_max_health();
float hud_status_range();
int hud_status_hover_unit();
float hud_status_hover_x();
float hud_status_hover_y();

}  // namespace bar
