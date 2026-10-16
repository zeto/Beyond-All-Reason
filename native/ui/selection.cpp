#include "selection.h"

#include "checklist.h"
#include "build_cmd.h"

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

bool selection_issue_move(UnitSim& sim, float goal_x, float goal_z, const TerrainMesh& terrain, std::string& error,
                          bool shift_queue) {
    const int index = selected_sim_index(sim);
    if (index < 0) {
        error = "no selection";
        return false;
    }
    if (!sim_order_move(sim, index, goal_x, goal_z, terrain, error, shift_queue)) {
        return false;
    }
    const size_t i = static_cast<size_t>(index);
    std::cout << "order move " << goal_x << " " << goal_z << " unit " << sim.id[i] << " waypoints "
              << sim.path_x[i].size() << (shift_queue ? " shift\n" : "\n");
    if (shift_queue) {
        checklist_note_shift_queue(static_cast<int>(sim.path_x[i].size()));
    }
    return true;
}

bool selection_issue_attack(UnitSim& sim, int target_unit_id, std::string& error) {
    const int index = selected_sim_index(sim);
    if (index < 0) {
        error = "no selection for attack";
        return false;
    }
    int target_index = -1;
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] == target_unit_id && (i >= sim.alive.size() || sim.alive[i] != 0)) {
            target_index = static_cast<int>(i);
            break;
        }
    }
    if (target_index < 0) {
        error = "attack target not found";
        return false;
    }
    g_snap.active_cmd = 20;  // CMD_ATTACK
    g_snap.cursor = "cursorattack";
    checklist_note_attack(target_unit_id);
    std::cout << "order attack unit " << sim.id[static_cast<size_t>(index)] << " -> " << target_unit_id
              << " cmd 20\n";
    return true;
}

bool selection_issue_reclaim(UnitSim& sim, float world_x, float world_z, std::string& error) {
    const int index = selected_sim_index(sim);
    if (index < 0) {
        error = "no selection for reclaim";
        return false;
    }
    g_snap.active_cmd = 90;  // CMD_RECLAIM
    g_snap.cursor = "cursorreclamate";
    checklist_note_reclaim(sim.id[static_cast<size_t>(index)]);
    std::cout << "order reclaim unit " << sim.id[static_cast<size_t>(index)] << " at " << world_x << " " << world_z
              << " cmd 90\n";
    return true;
}

bool selection_issue_repair(UnitSim& sim, int target_unit_id, std::string& error) {
    const int index = selected_sim_index(sim);
    if (index < 0) {
        error = "no selection for repair";
        return false;
    }
    int target_index = -1;
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] == target_unit_id && (i >= sim.alive.size() || sim.alive[i] != 0)) {
            target_index = static_cast<int>(i);
            break;
        }
    }
    if (target_index < 0) {
        error = "repair target not found";
        return false;
    }
    g_snap.active_cmd = 40;  // CMD_REPAIR
    g_snap.cursor = "cursorrepair";
    checklist_note_repair(target_unit_id);
    std::cout << "order repair unit " << sim.id[static_cast<size_t>(index)] << " -> " << target_unit_id
              << " cmd 40\n";
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
    // Selected units get the world-space gui_selectedunits_gl4 platter (TerrainRenderer::set_platter).
    if (g_snap.boxing) {
        const float left = std::min(g_snap.box_x1, g_snap.box_x2);
        const float right = std::max(g_snap.box_x1, g_snap.box_x2);
        const float bottom = std::min(g_snap.box_y1, g_snap.box_y2);
        const float top = std::max(g_snap.box_y1, g_snap.box_y2);
        HudVertex corners[4] = {
            {left, bottom, 0.2f, 1.f, 0.2f, 0.25f},
            {right, bottom, 0.2f, 1.f, 0.2f, 0.25f},
            {right, top, 0.2f, 1.f, 0.2f, 0.25f},
            {left, top, 0.2f, 1.f, 0.2f, 0.25f},
        };
        for (const HudVertex& vert : corners) {
            hud.vertices.push_back(vert);
        }
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
        } else if (kind == "click") {
            event.kind = SelectEventKind::Click;
            if (!(row >> event.x1 >> event.y1)) {
                error = "click line needs x y at " + std::to_string(lineno);
                return false;
            }
            event.x2 = event.x1;
            event.y2 = event.y1;
        } else if (kind == "move") {
            event.kind = SelectEventKind::MoveClick;
            if (!(row >> event.world_x >> event.world_z)) {
                error = "move line needs x z at " + std::to_string(lineno);
                return false;
            }
        } else if (kind == "shiftmove") {
            event.kind = SelectEventKind::ShiftMove;
            if (!(row >> event.world_x >> event.world_z)) {
                error = "shiftmove line needs x z at " + std::to_string(lineno);
                return false;
            }
        } else if (kind == "attack") {
            event.kind = SelectEventKind::Attack;
            if (!(row >> event.target_id)) {
                error = "attack line needs target id at " + std::to_string(lineno);
                return false;
            }
        } else if (kind == "facing") {
            event.kind = SelectEventKind::Facing;
            int facing = -1;
            if (row >> facing) {
                event.facing = facing;
            }
        } else if (kind == "groupset") {
            event.kind = SelectEventKind::GroupSet;
            if (!(row >> event.slot)) {
                error = "groupset needs slot 1-9";
                return false;
            }
        } else if (kind == "group") {
            event.kind = SelectEventKind::GroupRecall;
            if (!(row >> event.slot)) {
                error = "group needs slot 1-9";
                return false;
            }
        } else if (kind == "pause") {
            event.kind = SelectEventKind::Pause;
            int on = 1;
            if (row >> on) {
                event.pause = on != 0;
            }
        } else if (kind == "gamespeed") {
            event.kind = SelectEventKind::GameSpeed;
            if (!(row >> event.speed)) {
                error = "gamespeed needs 1-4";
                return false;
            }
        } else if (kind == "reclaim") {
            event.kind = SelectEventKind::Reclaim;
            if (!(row >> event.world_x >> event.world_z)) {
                error = "reclaim needs x z";
                return false;
            }
        } else if (kind == "repair") {
            event.kind = SelectEventKind::Repair;
            if (!(row >> event.target_id)) {
                error = "repair needs target id";
                return false;
            }
        } else if (kind == "spawn") {
            event.kind = SelectEventKind::Spawn;
            if (!(row >> event.def_name >> event.world_x >> event.world_z >> event.team)) {
                error = "spawn needs def x z team";
                return false;
            }
        } else if (kind == "pick" || kind == "place" || kind == "mmap" || kind == "minimap" || kind == "hover" ||
                   kind == "damage") {
            continue;
        } else {
            error = "unknown select event " + kind;
            return false;
        }
        events.push_back(event);
    }
    return true;
}

bool apply_select_events(UnitSim& sim, const std::vector<SelectEvent>& events, int presented,
                         const TerrainMesh& terrain, const std::string& game_root, std::string& error) {
    for (const SelectEvent& event : events) {
        if (event.frame != presented) {
            continue;
        }
        if (event.kind == SelectEventKind::Box) {
            selection_left_down(event.x1, event.y1);
            selection_left_drag(event.x2, event.y2);
            selection_left_up(event.x2, event.y2);
            checklist_note_drag_box();
            std::cout << "select box " << event.x1 << " " << event.y1 << " " << event.x2 << " " << event.y2
                      << " ids";
            for (int id : g_snap.selected) {
                std::cout << " " << id;
            }
            std::cout << " cursor " << g_snap.cursor << "\n";
        } else if (event.kind == SelectEventKind::Click) {
            selection_left_down(event.x1, event.y1);
            selection_left_up(event.x1, event.y1);
            checklist_note_click_select();
            std::cout << "select click " << event.x1 << " " << event.y1 << " ids";
            for (int id : g_snap.selected) {
                std::cout << " " << id;
            }
            std::cout << " cursor " << g_snap.cursor << "\n";
        } else if (event.kind == SelectEventKind::MoveClick) {
            if (!selection_issue_move(sim, event.world_x, event.world_z, terrain, error, false)) {
                return false;
            }
            std::cout << "select cursor " << g_snap.cursor << " cmd " << g_snap.active_cmd << "\n";
        } else if (event.kind == SelectEventKind::ShiftMove) {
            if (!selection_issue_move(sim, event.world_x, event.world_z, terrain, error, true)) {
                return false;
            }
        } else if (event.kind == SelectEventKind::Attack) {
            if (!selection_issue_attack(sim, event.target_id, error)) {
                return false;
            }
        } else if (event.kind == SelectEventKind::Facing) {
            // Build facing uses the existing [ / ] key cycle: 0..3.
            build_cycle_facing(event.facing);
        } else if (event.kind == SelectEventKind::GroupSet) {
            checklist_set_group(event.slot, g_snap.selected);
            std::cout << "group set " << event.slot << " size " << g_snap.selected.size() << "\n";
        } else if (event.kind == SelectEventKind::GroupRecall) {
            std::vector<int> ids;
            if (!checklist_recall_group(event.slot, ids)) {
                error = "empty group " + std::to_string(event.slot);
                return false;
            }
            g_snap.selected = ids;
            selection_refresh_cursor();
            std::cout << "group recall " << event.slot << " size " << ids.size() << "\n";
        } else if (event.kind == SelectEventKind::Pause) {
            checklist_set_paused(event.pause);
            std::cout << "pause " << (event.pause ? 1 : 0) << "\n";
        } else if (event.kind == SelectEventKind::GameSpeed) {
            checklist_set_gamespeed(event.speed);
            std::cout << "gamespeed " << event.speed << "\n";
        } else if (event.kind == SelectEventKind::Reclaim) {
            if (!selection_issue_reclaim(sim, event.world_x, event.world_z, error)) {
                return false;
            }
        } else if (event.kind == SelectEventKind::Repair) {
            if (!selection_issue_repair(sim, event.target_id, error)) {
                return false;
            }
        } else if (event.kind == SelectEventKind::Spawn) {
            int id = 0;
            if (!checklist_spawn_unit(sim, terrain, game_root, event.def_name, event.world_x, event.world_z, event.team,
                                      id, error)) {
                return false;
            }
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

void selection_replace(const std::vector<int>& ids, bool append) {
    if (!append) {
        g_snap.selected.clear();
    }
    for (int id : ids) {
        if (std::find(g_snap.selected.begin(), g_snap.selected.end(), id) == g_snap.selected.end()) {
            g_snap.selected.push_back(id);
        }
    }
    g_snap.active_cmd = 0;
}

}  // namespace bar
