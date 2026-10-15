#include "selection.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace bar {
namespace {

SelectionSnapshot g_snap;
std::vector<SelectionMark> g_marks;
int g_view_w = 1280;
int g_view_h = 720;
float g_mouse_x = -1;
float g_mouse_y = -1;
bool g_left = false;
bool g_middle = false;
bool g_right = false;
bool g_alt = false;
bool g_ctrl = false;
bool g_meta = false;
bool g_shift = false;
bool g_box_press = false;
float g_press_x = 0;
float g_press_y = 0;

constexpr int kCmdMove = 10;
constexpr float kClickSlop = 4.f;

void push_ring(HudList& hud, float cx, float cy, float radius, float r, float g, float b, float a) {
    const int segments = 24;
    const float inner = radius * 0.72f;
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

bool inside_box(float x, float y, float x1, float y1, float x2, float y2) {
    const float left = std::min(x1, x2);
    const float right = std::max(x1, x2);
    const float bottom = std::min(y1, y2);
    const float top = std::max(y1, y2);
    return x >= left && x <= right && y >= bottom && y <= top;
}

void select_from_box(float x1, float y1, float x2, float y2, bool additive) {
    if (!additive) {
        g_snap.selected.clear();
    }
    const float dx = std::fabs(x2 - x1);
    const float dy = std::fabs(y2 - y1);
    if (dx < kClickSlop && dy < kClickSlop) {
        // Click: nearest mark within its radius.
        int best = -1;
        float best_d2 = 1e12f;
        for (const SelectionMark& mark : g_marks) {
            const float mx = mark.screen_x - x2;
            const float my = mark.screen_y - y2;
            const float d2 = mx * mx + my * my;
            const float limit = mark.radius * mark.radius;
            if (d2 <= limit && d2 < best_d2) {
                best_d2 = d2;
                best = mark.unit_id;
            }
        }
        if (best >= 0) {
            if (std::find(g_snap.selected.begin(), g_snap.selected.end(), best) == g_snap.selected.end()) {
                g_snap.selected.push_back(best);
            }
        }
        return;
    }
    for (const SelectionMark& mark : g_marks) {
        if (!inside_box(mark.screen_x, mark.screen_y, x1, y1, x2, y2)) {
            continue;
        }
        if (std::find(g_snap.selected.begin(), g_snap.selected.end(), mark.unit_id) == g_snap.selected.end()) {
            g_snap.selected.push_back(mark.unit_id);
        }
    }
}

int selected_sim_index(const UnitSim& sim) {
    if (g_snap.selected.empty()) {
        return -1;
    }
    const int want = g_snap.selected.front();
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] == want && (i >= sim.alive.size() || sim.alive[i] != 0)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

void selection_reset() {
    g_snap = SelectionSnapshot{};
    g_marks.clear();
    g_box_press = false;
    g_left = g_middle = g_right = false;
    g_alt = g_ctrl = g_meta = g_shift = false;
    g_mouse_x = g_mouse_y = -1;
}

void selection_set_view(int width, int height) {
    if (width > 0) {
        g_view_w = width;
    }
    if (height > 0) {
        g_view_h = height;
    }
}

void selection_set_mouse(float x, float y, bool left, bool middle, bool right, bool alt, bool ctrl, bool meta,
                         bool shift) {
    g_mouse_x = x;
    g_mouse_y = y;
    g_left = left;
    g_middle = middle;
    g_right = right;
    g_alt = alt;
    g_ctrl = ctrl;
    g_meta = meta;
    g_shift = shift;
}

void selection_set_marks(const std::vector<SelectionMark>& marks) { g_marks = marks; }

void selection_left_down(float x, float y) {
    g_box_press = true;
    g_press_x = x;
    g_press_y = y;
    g_snap.boxing = true;
    g_snap.box_x1 = x;
    g_snap.box_y1 = y;
    g_snap.box_x2 = x;
    g_snap.box_y2 = y;
}

void selection_left_drag(float x, float y) {
    if (!g_snap.boxing) {
        return;
    }
    g_snap.box_x2 = x;
    g_snap.box_y2 = y;
}

void selection_left_up(float x, float y) {
    if (!g_snap.boxing && !g_box_press) {
        return;
    }
    g_snap.box_x2 = x;
    g_snap.box_y2 = y;
    select_from_box(g_press_x, g_press_y, x, y, g_shift || g_ctrl);
    g_snap.boxing = false;
    g_box_press = false;
    g_snap.active_cmd = g_snap.selected.empty() ? 0 : kCmdMove;
    selection_refresh_cursor();
}

bool selection_issue_move(UnitSim& sim, float goal_x, float goal_z, const TerrainMesh& terrain, std::string& error) {
    const int index = selected_sim_index(sim);
    if (index < 0) {
        error = "no selection";
        return false;
    }
    if (!sim_order_move(sim, index, goal_x, goal_z, terrain, error)) {
        return false;
    }
    std::cout << "order move " << goal_x << " " << goal_z << " unit " << sim.id[static_cast<size_t>(index)]
              << " waypoints " << sim.path_x[static_cast<size_t>(index)].size() << "\n";
    return true;
}

void selection_set_cursor(const std::string& name) {
    if (!name.empty()) {
        g_snap.cursor = name;
    }
}

void selection_refresh_cursor() {
    if (g_snap.selected.empty()) {
        g_snap.cursor = "cursornormal";
        g_snap.active_cmd = 0;
        return;
    }
    g_snap.active_cmd = kCmdMove;
    g_snap.cursor = "cursormove";
}

const SelectionSnapshot& selection_snapshot() { return g_snap; }

bool selection_get_box(float& x1, float& y1, float& x2, float& y2) {
    if (!g_snap.boxing) {
        return false;
    }
    x1 = g_snap.box_x1;
    y1 = g_snap.box_y1;
    x2 = g_snap.box_x2;
    y2 = g_snap.box_y2;
    return true;
}

void selection_append_highlights(HudList& hud) {
    for (const SelectionMark& mark : g_marks) {
        if (std::find(g_snap.selected.begin(), g_snap.selected.end(), mark.unit_id) == g_snap.selected.end()) {
            continue;
        }
        push_ring(hud, mark.screen_x, mark.screen_y, mark.radius, 0.2f, 1.f, 0.35f, 0.55f);
    }
}

bool load_select_events(const std::string& path, std::vector<SelectEvent>& events, std::string& error) {
    events.clear();
    std::ifstream in(path);
    if (!in) {
        error = "could not read select input " + path;
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        SelectEvent event;
        std::string kind;
        if (!(row >> event.frame >> kind)) {
            error = "bad select input line " + std::to_string(lineno);
            return false;
        }
        if (kind == "box") {
            event.kind = SelectEventKind::Box;
            if (!(row >> event.x1 >> event.y1 >> event.x2 >> event.y2)) {
                error = "box line needs x1 y1 x2 y2 at " + std::to_string(lineno);
                return false;
            }
        } else if (kind == "move") {
            event.kind = SelectEventKind::MoveClick;
            if (!(row >> event.world_x >> event.world_z)) {
                error = "move line needs x z at " + std::to_string(lineno);
                return false;
            }
        } else if (kind == "pick" || kind == "place") {
            continue;  // build-menu events in a shared --input file
        } else {
            error = "unknown select event " + kind;
            return false;
        }
        events.push_back(event);
    }
    return true;
}

bool apply_select_events(UnitSim& sim, const std::vector<SelectEvent>& events, int presented,
                         const TerrainMesh& terrain, std::string& error) {
    for (const SelectEvent& event : events) {
        if (event.frame != presented) {
            continue;
        }
        if (event.kind == SelectEventKind::Box) {
            selection_left_down(event.x1, event.y1);
            selection_left_drag(event.x2, event.y2);
            selection_left_up(event.x2, event.y2);
            std::cout << "select box " << event.x1 << " " << event.y1 << " " << event.x2 << " " << event.y2
                      << " ids";
            for (int id : g_snap.selected) {
                std::cout << " " << id;
            }
            std::cout << " cursor " << g_snap.cursor << "\n";
        } else if (event.kind == SelectEventKind::MoveClick) {
            if (!selection_issue_move(sim, event.world_x, event.world_z, terrain, error)) {
                return false;
            }
            std::cout << "select cursor " << g_snap.cursor << " cmd " << g_snap.active_cmd << "\n";
        }
    }
    return true;
}

bool write_selection_report(const std::string& path, int frame, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write selection report " + path;
        return false;
    }
    out << "{\"f\":" << frame << ",\"cursor\":\"" << g_snap.cursor << "\",\"cmd\":" << g_snap.active_cmd
        << ",\"sel\":[";
    for (size_t i = 0; i < g_snap.selected.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << g_snap.selected[i];
    }
    out << "]}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar
