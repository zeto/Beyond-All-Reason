#include "hud_status.h"

#include "gadget_host.h"
#include "unitdef.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

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

float unit_range(const std::string& def_name) {
    float range = 0;
    std::string error;
    if (!g_game_root.empty()) {
        load_unit_max_weapon_range(g_game_root, def_name, range, error);
    }
    if (range <= 0.f) {
        // Fall back to UnitDefs table via gadget session name lookup.
        const int def_id = player_unitdef_id(def_name);
        (void)def_id;
    }
    return range;
}

void push_ring(HudList& hud, float cx, float cy, float radius, float r, float g, float b, float a) {
    const int segments = 48;
    const float inner = radius * 0.92f;
    const float outer = radius;
    for (int i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * 6.2831853f / static_cast<float>(segments);
        const float a1 = static_cast<float>(i + 1) * 6.2831853f / static_cast<float>(segments);
        const float c0 = std::cos(a0);
        const float s0 = std::sin(a0);
        const float c1 = std::cos(a1);
        const float s1 = std::sin(a1);
        HudVertex corners[4] = {
            {cx + c0 * inner, cy + s0 * inner * 0.55f, r, g, b, a},
            {cx + c1 * inner, cy + s1 * inner * 0.55f, r, g, b, a},
            {cx + c1 * outer, cy + s1 * outer * 0.55f, r, g, b, a},
            {cx + c0 * outer, cy + s0 * outer * 0.55f, r, g, b, a},
        };
        for (const HudVertex& vert : corners) {
            hud.vertices.push_back(vert);
        }
    }
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

}  // namespace

void hud_status_reset() {
    g_tooltip.clear();
    g_health = 0;
    g_max_health = 0;
    g_range = 0;
    g_hover_unit = 0;
}

void hud_status_set_camera(const Camera* camera, const CameraPose* pose) {
    g_camera = camera;
    g_pose = pose;
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
    if (g_camera == nullptr || g_pose == nullptr || sim.id.empty()) {
        return;
    }
    // Only unit 0 is drawn; pick it when the pointer is near its screen position.
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
    // Prefer translated name from UnitDefs when available.
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
    (void)terrain;
    if (g_camera == nullptr || g_pose == nullptr || sim.id.empty()) {
        return;
    }
    float sx = 0;
    float sy = 0;
    if (!world_to_screen(*g_camera, *g_pose, sim.x[0], sim.y[0] + 35.f, sim.z[0], sx, sy)) {
        return;
    }
    const float hp = (0 < sim.health.size()) ? sim.health[0] : 0.f;
    const float hp_max = (0 < sim.max_health.size()) ? sim.max_health[0] : 1.f;
    const float frac = hp_max > 0.f ? std::max(0.f, std::min(1.f, hp / hp_max)) : 0.f;
    // Health bar (engine overlay; gui_healthbars_gl4 deferred as GL4-heavy).
    const float bar_w = 42.f;
    const float bar_h = 5.f;
    push_bar(hud, sx - bar_w * 0.5f, sy + 18.f, sx + bar_w * 0.5f, sy + 18.f + bar_h, 0.05f, 0.05f, 0.05f, 0.85f);
    push_bar(hud, sx - bar_w * 0.5f, sy + 18.f, sx - bar_w * 0.5f + bar_w * frac, sy + 18.f + bar_h, 0.15f, 0.9f,
             0.25f, 0.95f);
    // Range ring (engine overlay; gui_attackrange_gl4 deferred). Radius in screen px
    // approximates maxWeaponRange projected from a ground offset sample.
    float range = unit_range(sim.def_name[0]);
    if (range > 0.f) {
        float edge_x = 0;
        float edge_y = 0;
        if (world_to_screen(*g_camera, *g_pose, sim.x[0] + range, sim.y[0], sim.z[0], edge_x, edge_y)) {
            const float dx = edge_x - sx;
            const float dy = edge_y - sy;
            const float screen_r = std::sqrt(dx * dx + dy * dy);
            if (screen_r > 4.f && screen_r < 2000.f) {
                push_ring(hud, sx, sy, screen_r, 1.f, 0.35f, 0.2f, 0.45f);
            }
        }
        g_range = range;
    }
    if (!g_tooltip.empty()) {
        HudText text;
        text.text = g_tooltip;
        text.x = sx;
        text.y = sy + 28.f;
        text.size = 14;
        text.r = 1;
        text.g = 1;
        text.b = 1;
        text.a = 1;
        text.align = 1;
        text.outline = true;
        hud.texts.push_back(std::move(text));
    }
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
        << "\",\"health\":" << g_health << ",\"maxHealth\":" << g_max_health << ",\"range\":" << g_range << "}\n";
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

}  // namespace bar
