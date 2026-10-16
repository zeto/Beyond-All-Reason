#include "hud_status.h"

#include "gadget_host.h"
#include "unitdef.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace bar {
namespace {

const Camera* g_camera = nullptr;
const CameraPose* g_pose = nullptr;
std::string g_tooltip;
float g_health = 0;
float g_max_health = 0;
float g_range = 0;
int g_hover_unit = 0;
std::string g_game_root;
float g_hover_sx = 640.f;
float g_hover_sy = 360.f;
float g_bar_left = 0;
float g_bar_right = 0;
float g_bar_fill_right = 0;
float g_bar_bottom = 0;
float g_bar_top = 0;
float g_bar_frac = 0;
float g_unit_height = 0;
float g_unit_size = 1;
std::vector<std::pair<float, float>> g_ring_samples;

float unit_range(const std::string& def_name) {
    float range = 0;
    std::string error;
    if (!g_game_root.empty()) {
        load_unit_max_weapon_range(g_game_root, def_name, range, error);
    }
    if (range <= 0.f) {
        const int def_id = player_unitdef_id(def_name);
        (void)def_id;
    }
    return range;
}

void push_bar(HudList& hud, float left, float bottom, float right, float top, float r, float g, float b, float a) {
    HudVertex corners[4] = {
        {left, bottom, r, g, b, a},
        {right, bottom, r, g, b, a},
        {right, top, r, g, b, a},
        {left, top, r, g, b, a},
    };
    for (const HudVertex& vert : corners) {
        hud.vertices.push_back(vert);
    }
}

// Thin world-space circle on the terrain at radius, projected with the real camera.
void push_world_range_ring(HudList& hud, const UnitSim& sim, const TerrainMesh& terrain, float range) {
    g_ring_samples.clear();
    if (g_camera == nullptr || g_pose == nullptr || !(range > 0.f)) {
        return;
    }
    const int segments = 96;
    const float line_half = 1.15f;  // ~2.3 px thick band
    const float lift = 1.5f;
    const float cx = sim.x[0];
    const float cz = sim.z[0];
    float prev_sx = 0;
    float prev_sy = 0;
    bool have_prev = false;
    for (int i = 0; i <= segments; ++i) {
        const float ang = static_cast<float>(i) * 6.2831853f / static_cast<float>(segments);
        const float wx = cx + std::cos(ang) * range;
        const float wz = cz + std::sin(ang) * range;
        const float wy = terrain_height_at(terrain, wx, wz) + lift;
        float sx = 0;
        float sy = 0;
        if (!world_to_screen(*g_camera, *g_pose, wx, wy, wz, sx, sy)) {
            have_prev = false;
            continue;
        }
        if (i % 8 == 0) {
            g_ring_samples.emplace_back(sx, sy);
        }
        if (have_prev) {
            const float dx = sx - prev_sx;
            const float dy = sy - prev_sy;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.4f) {
                const float nx = (-dy / len) * line_half;
                const float ny = (dx / len) * line_half;
                HudVertex corners[4] = {
                    {prev_sx + nx, prev_sy + ny, 1.f, 0.4f, 0.15f, 0.85f},
                    {sx + nx, sy + ny, 1.f, 0.4f, 0.15f, 0.85f},
                    {sx - nx, sy - ny, 1.f, 0.4f, 0.15f, 0.85f},
                    {prev_sx - nx, prev_sy - ny, 1.f, 0.4f, 0.15f, 0.85f},
                };
                for (const HudVertex& vert : corners) {
                    hud.vertices.push_back(vert);
                }
            }
        }
        prev_sx = sx;
        prev_sy = sy;
        have_prev = true;
    }
}

}  // namespace

void hud_status_reset() {
    g_tooltip.clear();
    g_health = 0;
    g_max_health = 0;
    g_range = 0;
    g_hover_unit = 0;
    g_bar_left = g_bar_right = g_bar_fill_right = g_bar_bottom = g_bar_top = g_bar_frac = 0;
    g_ring_samples.clear();
}

void hud_status_set_camera(const Camera* camera, const CameraPose* pose) {
    g_camera = camera;
    g_pose = pose;
}

void hud_status_set_unit(float height, float radius, float power, float max_health) {
    g_unit_height = height;
    // unitDefSizeMultipliers: min(1.45, max(0.85, radius / 150 + min(0.6, power / 4000))) + min(0.6, health / 22000).
    g_unit_size = std::min(1.45f, std::max(0.85f, radius / 150.f + std::min(0.6f, power / 4000.f))) +
                  std::min(0.6f, max_health / 22000.f);
}

void hud_status_set_root(const std::string& game_root) {
    g_game_root = game_root;
}

void hud_status_hover(float screen_x, float screen_y, const UnitSim& sim, const TerrainMesh& terrain) {
    (void)terrain;
    g_hover_unit = 0;
    g_tooltip.clear();
    g_health = 0;
    g_max_health = 0;
    g_range = 0;
    g_hover_sx = screen_x;
    g_hover_sy = screen_y;
    if (g_camera == nullptr || g_pose == nullptr || sim.id.empty()) {
        return;
    }
    float sx = 0;
    float sy = 0;
    if (!world_to_screen(*g_camera, *g_pose, sim.x[0], sim.y[0] + 20.f, sim.z[0], sx, sy)) {
        return;
    }
    const float dx = screen_x - sx;
    const float dy = screen_y - sy;
    if (dx * dx + dy * dy > 48.f * 48.f) {
        return;
    }
    g_hover_unit = sim.id[0];
    g_health = (0 < sim.health.size()) ? sim.health[0] : 0.f;
    g_max_health = (0 < sim.max_health.size()) ? sim.max_health[0] : 0.f;
    g_range = unit_range(sim.def_name[0]);
    g_tooltip = sim.def_name[0];
    const int def_id = player_unitdef_id(sim.def_name[0]);
    (void)def_id;
    std::cout << "hud hover unit " << g_hover_unit << " tooltip " << g_tooltip << " hp " << g_health << "/"
              << g_max_health << " range " << g_range << "\n";
}

bool hud_status_damage(UnitSim& sim, int spring_unit_id, float health_frac, std::string& error) {
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] != spring_unit_id) {
            continue;
        }
        if (i >= sim.max_health.size() || !(sim.max_health[i] > 0.f)) {
            error = "unit has no max health";
            return false;
        }
        if (health_frac < 0.f) {
            health_frac = 0.f;
        }
        if (health_frac > 1.f) {
            health_frac = 1.f;
        }
        sim.health[i] = sim.max_health[i] * health_frac;
        g_health = sim.health[i];
        g_max_health = sim.max_health[i];
        std::cout << "hud damage unit " << spring_unit_id << " hp " << sim.health[i] << "/" << sim.max_health[i]
                  << "\n";
        return true;
    }
    error = "unit not found for damage";
    return false;
}

void hud_status_append(HudList& hud, const UnitSim& sim, const TerrainMesh& terrain) {
    if (g_camera == nullptr || g_pose == nullptr || sim.id.empty()) {
        return;
    }
    // gui_healthbars_gl4: a camera-facing bar at drawPos + HEIGHTOFFSET 3 + height + 24 * size,
    // 2 * BARWIDTH 2.56 by BARHEIGHT 0.9 times BARSCALE 4 * size elmos; percentage bars hide above 0.999.
    const float size = g_unit_size;
    const float anchor_y = sim.y[0] + 3.f + g_unit_height + 24.f * size;
    float sx = 0;
    float sy = 0;
    if (!world_to_screen(*g_camera, *g_pose, sim.x[0], anchor_y, sim.z[0], sx, sy)) {
        return;
    }
    // A camera-right vector is horizontal: its pixel length per elmo is the x reach of world x and z.
    float ax = 0, ay = 0, bx = 0, by = 0;
    float px_per_elmo = 0.f;
    if (world_to_screen(*g_camera, *g_pose, sim.x[0] + 1.f, anchor_y, sim.z[0], ax, ay) &&
        world_to_screen(*g_camera, *g_pose, sim.x[0], anchor_y, sim.z[0] + 1.f, bx, by)) {
        px_per_elmo = std::sqrt((ax - sx) * (ax - sx) + (bx - sx) * (bx - sx));
    }
    const float hp = (0 < sim.health.size()) ? sim.health[0] : 0.f;
    const float hp_max = (0 < sim.max_health.size()) ? sim.max_health[0] : 1.f;
    const float frac = hp_max > 0.f ? std::max(0.f, std::min(1.f, hp / hp_max)) : 0.f;
    // Health bar: readable size above the unit, dark track + green fill at health/maxHealth.
    const float bar_w = 2.f * 2.56f * 4.f * size * px_per_elmo;
    const float bar_h = 0.9f * 4.f * size * px_per_elmo;
    const float bar_y = sy - bar_h * 0.5f;
    if (frac > 0.999f || !(bar_w > 1.f)) {
        float range = unit_range(sim.def_name[0]);
        if (range > 0.f) {
            push_world_range_ring(hud, sim, terrain, range);
            g_range = range;
        }
        g_health = hp;
        g_max_health = hp_max;
        g_bar_frac = frac;
        return;
    }
    const float left = sx - bar_w * 0.5f;
    const float right = sx + bar_w * 0.5f;
    const float fill_right = left + bar_w * frac;
    // Border + track + fill.
    push_bar(hud, left - 1.f, bar_y - 1.f, right + 1.f, bar_y + bar_h + 1.f, 0.f, 0.f, 0.f, 0.9f);
    push_bar(hud, left, bar_y, right, bar_y + bar_h, 0.12f, 0.12f, 0.12f, 0.92f);
    if (frac > 0.01f) {
        // health bar type: mix(mincolor red, maxcolor green, health), BITCOLORCORRECT divides by max(r, g).
        const float cr = 1.f - frac;
        const float cg = frac;
        const float cm = std::max(cr, cg);
        push_bar(hud, left, bar_y, fill_right, bar_y + bar_h, cr / cm, cg / cm, 0.f, 1.f);
    }
    g_bar_left = left;
    g_bar_right = right;
    g_bar_fill_right = fill_right;
    g_bar_bottom = bar_y;
    g_bar_top = bar_y + bar_h;
    g_bar_frac = frac;
    g_health = hp;
    g_max_health = hp_max;

    float range = unit_range(sim.def_name[0]);
    if (range > 0.f) {
        push_world_range_ring(hud, sim, terrain, range);
        g_range = range;
    }
    // Stock gui_tooltip renders the unit name near the cursor; no engine HudText label.
}

bool load_hud_status_events(const std::string& path, std::vector<HudStatusEvent>& events, std::string& error) {
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
        if (kind != "hover" && kind != "damage") {
            continue;
        }
        HudStatusEvent event;
        event.frame = frame;
        event.kind = kind;
        if (kind == "hover") {
            row >> event.screen_x >> event.screen_y;
        } else {
            row >> event.health_frac;
        }
        events.push_back(event);
    }
    return true;
}

bool apply_hud_status_events(UnitSim& sim, const std::vector<HudStatusEvent>& events, int presented,
                             const TerrainMesh& terrain, std::string& error) {
    for (const HudStatusEvent& event : events) {
        if (event.frame != presented) {
            continue;
        }
        if (event.kind == "hover") {
            hud_status_hover(event.screen_x, event.screen_y, sim, terrain);
        } else if (event.kind == "damage") {
            const int id = sim.id.empty() ? 0 : sim.id[0];
            if (!hud_status_damage(sim, id, event.health_frac, error)) {
                return false;
            }
        }
    }
    return true;
}

bool write_hud_status_report(const std::string& path, int frame, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write hud status report " + path;
        return false;
    }
    out << "{\"f\":" << frame << ",\"hover\":" << g_hover_unit << ",\"tooltip\":\"" << g_tooltip
        << "\",\"health\":" << g_health << ",\"maxHealth\":" << g_max_health << ",\"range\":" << g_range
        << ",\"hoverX\":" << g_hover_sx << ",\"hoverY\":" << g_hover_sy << ",\"barLeft\":" << g_bar_left
        << ",\"barRight\":" << g_bar_right << ",\"barFillRight\":" << g_bar_fill_right
        << ",\"barBottom\":" << g_bar_bottom << ",\"barTop\":" << g_bar_top << ",\"barFrac\":" << g_bar_frac
        << ",\"ringSamples\":[";
    for (size_t i = 0; i < g_ring_samples.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "[" << g_ring_samples[i].first << "," << g_ring_samples[i].second << "]";
    }
    out << "]}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

const std::string& hud_status_tooltip() {
    return g_tooltip;
}
float hud_status_health() {
    return g_health;
}
float hud_status_max_health() {
    return g_max_health;
}
float hud_status_range() {
    return g_range;
}
int hud_status_hover_unit() {
    return g_hover_unit;
}
float hud_status_hover_x() {
    return g_hover_sx;
}
float hud_status_hover_y() {
    return g_hover_sy;
}

}  // namespace bar
