#include "build_cmd.h"

#include "checklist.h"

#include "unitdef.h"
#include "gadget_host.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace bar {
namespace {

BuildCmdState g_build;
int g_observed_building = 0;
int g_observed_builder = 0;
std::string g_game_root;
std::vector<CmdDesc> g_last_descs;
std::vector<ActiveCmdDesc> g_active_descs;
std::map<int, int> g_fire_state;
std::map<int, int> g_move_state;
std::map<int, int> g_repeat_state;
bool g_commands_changed = false;

void push_quad(HudList& hud, float x1, float y1, float x2, float y2, float r, float g, float b, float a) {
    HudVertex corners[4] = {
        {x1, y1, r, g, b, a},
        {x2, y1, r, g, b, a},
        {x2, y2, r, g, b, a},
        {x1, y2, r, g, b, a},
    };
    for (const HudVertex& vert : corners) {
        hud.vertices.push_back(vert);
    }
}

bool project_point(const float* view_proj, float x, float y, float z, float view_w, float view_h, float& sx,
                   float& sy) {
    const float cx = view_proj[0] * x + view_proj[4] * y + view_proj[8] * z + view_proj[12];
    const float cy = view_proj[1] * x + view_proj[5] * y + view_proj[9] * z + view_proj[13];
    const float cw = view_proj[3] * x + view_proj[7] * y + view_proj[11] * z + view_proj[15];
    if (!(cw > 0.01f)) {
        return false;
    }
    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    sx = (ndc_x * 0.5f + 0.5f) * view_w;
    sy = (ndc_y * 0.5f + 0.5f) * view_h;
    return true;
}

int sim_index_for(const UnitSim& sim, int unit_id) {
    for (size_t i = 0; i < sim.id.size(); ++i) {
        if (sim.id[i] == unit_id && (i >= sim.alive.size() || sim.alive[i] != 0)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

void build_cmd_set_root(const std::string& game_root) { g_game_root = game_root; }

const BuildCmdState& build_cmd_state() { return g_build; }

void build_cmd_reset() {
    g_observed_building = 0;
    g_observed_builder = 0;
    g_build = BuildCmdState{};
    g_last_descs.clear();
    g_active_descs.clear();
    g_fire_state.clear();
    g_move_state.clear();
    g_repeat_state.clear();
}

std::vector<CmdDesc> build_unit_cmd_descs(const UnitSim& sim, int unit_id) {
    g_last_descs.clear();
    const int index = sim_index_for(sim, unit_id);
    if (index < 0 || g_game_root.empty()) {
        return g_last_descs;
    }
    UnitRole role;
    std::string error;
    if (!load_unit_role(g_game_root, sim.def_name[static_cast<size_t>(index)], role, error) || !role.ok) {
        return g_last_descs;
    }
    // Stop / Move first so index 1 is Stop in Spring style; builds follow.
    g_last_descs.push_back({0, "Stop", "CMDTYPE_ICON", "stop"});
    g_last_descs.push_back({10, "Move", "CMDTYPE_ICON_MAP", "move"});
    for (const std::string& option : role.options) {
        UnitRole child;
        if (!load_unit_role(g_game_root, option, child, error)) {
            continue;
        }
        // Negative ids are build commands. Def id lookup is deferred to SetActiveCommand by name.
        CmdDesc desc;
        desc.id = 0;  // filled by caller with UnitDefNames if available
        desc.name = option;
        desc.type = "CMDTYPE_ICON_BUILDING";
        desc.action = "buildunit_" + option;
        g_last_descs.push_back(desc);
    }
    return g_last_descs;
}

int build_cmd_desc_index(int cmd_id) {
    for (size_t i = 0; i < g_last_descs.size(); ++i) {
        if (g_last_descs[i].id == cmd_id) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

bool build_set_active_command(int cmd_or_index, const UnitSim* sim, int selected_unit_id, std::string& error) {
    if (cmd_or_index == 0) {
        build_clear_active_command();
        return true;
    }
    if (sim == nullptr || selected_unit_id <= 0) {
        error = "no builder selected";
        return false;
    }
    std::vector<CmdDesc> descs = build_unit_cmd_descs(*sim, selected_unit_id);
    // Prefer treating the argument as a 1-based index when it is in range.
    int cmd_id = cmd_or_index;
    if (cmd_or_index > 0 && cmd_or_index <= static_cast<int>(descs.size())) {
        cmd_id = descs[static_cast<size_t>(cmd_or_index - 1)].id;
        if (cmd_id == 0 && !descs[static_cast<size_t>(cmd_or_index - 1)].name.empty() &&
            descs[static_cast<size_t>(cmd_or_index - 1)].type == "CMDTYPE_ICON_BUILDING") {
            // Name-only build slot: resolve later via def_name.
            g_build.def_name = descs[static_cast<size_t>(cmd_or_index - 1)].name;
            g_build.active_cmd = -1;  // placeholder until Lua fills unitDefID
            g_build.active_index = cmd_or_index;
            g_build.active_name = g_build.def_name;
            g_build.placing = true;
            UnitBuild build;
            if (load_unit_build(g_game_root, g_build.def_name, build, error)) {
                g_build.footprint_x = std::max(1, build.footprint_x);
                g_build.footprint_z = std::max(1, build.footprint_z);
            }
            error.clear();
            std::cout << "build pick " << g_build.def_name << " index " << cmd_or_index << "\n";
            return true;
        }
    }
    if (cmd_id < 0) {
        // Negative unitDefID build. Def name must be supplied by Lua side via build_set_by_def.
        error = "use build_set_by_def for negative ids";
        return false;
    }
    if (cmd_id == 10) {
        g_build.active_cmd = 10;
        g_build.active_index = 2;
        g_build.active_name = "Move";
        g_build.placing = false;
        g_build.def_name.clear();
        return true;
    }
    error = "unknown active command";
    return false;
}

bool build_set_by_def(const std::string& def_name, int unit_def_id, const UnitSim* /*sim*/, std::string& error) {
    if (def_name.empty() || unit_def_id == 0) {
        error = "missing build def";
        return false;
    }
    g_build.def_name = def_name;
    g_build.active_cmd = -unit_def_id;
    g_build.active_index = 1;
    g_build.active_name = def_name;
    g_build.placing = true;
    UnitBuild build;
    if (!load_unit_build(g_game_root, def_name, build, error)) {
        g_build.footprint_x = 2;
        g_build.footprint_z = 2;
        error.clear();
    } else {
        g_build.footprint_x = std::max(1, build.footprint_x);
        g_build.footprint_z = std::max(1, build.footprint_z);
    }
    std::cout << "build pick " << def_name << " cmd " << g_build.active_cmd << "\n";
    return true;
}

void build_clear_active_command() {
    g_build = BuildCmdState{};
}

bool build_update_preview(const TerrainMesh& terrain, float world_x, float world_z) {
    if (!g_build.placing || g_build.def_name.empty()) {
        return false;
    }
    const float snapped_x = std::floor(world_x / 16.f) * 16.f + 8.f;
    const float snapped_z = std::floor(world_z / 16.f) * 16.f + 8.f;
    g_build.preview_x = snapped_x;
    g_build.preview_z = snapped_z;
    (void)terrain;
    return true;
}

bool build_place(UnitSim& sim, int selected_unit_id, const TerrainMesh& terrain, std::string& error) {
    if (!g_build.placing || g_build.def_name.empty()) {
        error = "no active build";
        return false;
    }
    if (selected_unit_id <= 0) {
        error = "no selection";
        return false;
    }
    (void)sim;
    (void)terrain;
    if (!player_issue_build(selected_unit_id, g_build.def_name, g_build.preview_x, g_build.preview_z, error)) {
        return false;
    }
    return true;
}

void build_append_preview(HudList& hud, const float* view_proj, int view_w, int view_h) {
    if (!g_build.placing || view_proj == nullptr || view_w <= 0 || view_h <= 0) {
        return;
    }
    const float half_x = static_cast<float>(g_build.footprint_x) * 8.f;
    const float half_z = static_cast<float>(g_build.footprint_z) * 8.f;
    const float y = 0.f;
    float corners[4][2];
    const float wx[4] = {g_build.preview_x - half_x, g_build.preview_x + half_x, g_build.preview_x + half_x,
                         g_build.preview_x - half_x};
    const float wz[4] = {g_build.preview_z - half_z, g_build.preview_z - half_z, g_build.preview_z + half_z,
                         g_build.preview_z + half_z};
    for (int i = 0; i < 4; ++i) {
        if (!project_point(view_proj, wx[i], y, wz[i], static_cast<float>(view_w), static_cast<float>(view_h),
                           corners[i][0], corners[i][1])) {
            return;
        }
    }
    // Filled quad from projected footprint corners (no unit mesh).
    HudVertex verts[4];
    for (int i = 0; i < 4; ++i) {
        verts[i] = {corners[i][0], corners[i][1], 0.2f, 0.9f, 1.f, 0.35f};
    }
    for (const HudVertex& vert : verts) {
        hud.vertices.push_back(vert);
    }
    // Outline
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const float x1 = corners[i][0];
        const float y1 = corners[i][1];
        const float x2 = corners[j][0];
        const float y2 = corners[j][1];
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 1.f)) {
            continue;
        }
        const float nx = -dy / len * 1.5f;
        const float ny = dx / len * 1.5f;
        push_quad(hud, x1 + nx, y1 + ny, x2 + nx, y2 + ny, 0.1f, 1.f, 0.8f, 0.9f);
        // Actually need a proper thick line - use a thin quad along the edge
        hud.vertices.pop_back();
        hud.vertices.pop_back();
        hud.vertices.pop_back();
        hud.vertices.pop_back();
        HudVertex edge[4] = {
            {x1 - nx, y1 - ny, 0.1f, 1.f, 0.8f, 0.9f},
            {x1 + nx, y1 + ny, 0.1f, 1.f, 0.8f, 0.9f},
            {x2 + nx, y2 + ny, 0.1f, 1.f, 0.8f, 0.9f},
            {x2 - nx, y2 - ny, 0.1f, 1.f, 0.8f, 0.9f},
        };
        for (const HudVertex& vert : edge) {
            hud.vertices.push_back(vert);
        }
    }
}


void build_cycle_facing(int absolute_or_neg1) {
    if (absolute_or_neg1 >= 0) {
        g_build.facing = absolute_or_neg1 & 3;
    } else {
        g_build.facing = (g_build.facing + 1) & 3;
    }
    checklist_note_facing(g_build.facing);
    std::cout << "build facing " << g_build.facing << "\n";
}

bool load_build_events(const std::string& path, std::vector<BuildEvent>& events, std::string& error) {
    events.clear();
    std::ifstream in(path);
    if (!in) {
        error = "could not read build input " + path;
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
        BuildEvent event;
        std::string kind;
        if (!(row >> event.frame >> kind)) {
            error = "bad build input line " + std::to_string(lineno);
            return false;
        }
        if (kind == "pick") {
            event.kind = BuildEventKind::Pick;
            if (!(row >> event.def_name)) {
                error = "pick needs def name";
                return false;
            }
        } else if (kind == "place") {
            event.kind = BuildEventKind::Place;
            if (!(row >> event.def_name >> event.x >> event.z)) {
                error = "place needs def x z";
                return false;
            }
        } else if (kind == "box" || kind == "move" || kind == "click" || kind == "shiftmove" ||
                   kind == "attack" || kind == "facing" || kind == "groupset" || kind == "group" ||
                   kind == "pause" || kind == "gamespeed" || kind == "reclaim" || kind == "repair" ||
                   kind == "spawn" || kind == "mmap" || kind == "minimap" || kind == "hover" ||
                   kind == "damage") {
            continue;  // selection/checklist/minimap events in a shared --input file
        } else {
            error = "unknown build event " + kind;
            return false;
        }
        events.push_back(event);
    }
    return true;
}

bool apply_build_events(UnitSim& sim, const std::vector<BuildEvent>& events, int presented,
                        const TerrainMesh& terrain, int selected_unit_id, std::string& error) {
    if (selected_unit_id > 0) {
        g_observed_builder = selected_unit_id;
        const int building = unit_is_building(selected_unit_id);
        if (building > 0) {
            g_observed_building = building;
        }
    }
    for (const BuildEvent& event : events) {
        if (event.frame != presented) {
            continue;
        }
        if (event.kind == BuildEventKind::Pick) {
            const int def_id = player_unitdef_id(event.def_name);
            if (!build_set_by_def(event.def_name, def_id != 0 ? def_id : 1, &sim, error)) {
                return false;
            }
            // Also tell the stock grid menu via SetActiveCommand(-defID) path.
            std::cout << "build pick " << event.def_name << " id " << def_id << "\n";
        } else if (event.kind == BuildEventKind::Place) {
            g_build.def_name = event.def_name;
            g_build.placing = true;
            build_update_preview(terrain, event.x, event.z);
            g_build.preview_x = event.x;
            g_build.preview_z = event.z;
            if (!build_place(sim, selected_unit_id, terrain, error)) {
                return false;
            }
            g_observed_builder = selected_unit_id;
            const int building = unit_is_building(selected_unit_id);
            if (building > 0) {
                g_observed_building = building;
            }
        }
    }
    return true;
}

bool write_build_report(const std::string& path, int frame, std::string& error) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "could not write build report " + path;
        return false;
    }
    const int builder = g_observed_builder > 0 ? g_observed_builder : 1;
    const int building_now = unit_is_building(builder);
    if (building_now > 0) {
        g_observed_building = building_now;
    }
    TeamRuleEntry start_unit;
    const bool have_start = team_rules_param(0, "startUnit", start_unit);
    out << "{\"f\":" << frame << ",\"cmd\":" << g_build.active_cmd << ",\"def\":\"" << g_build.def_name
        << "\",\"placing\":" << (g_build.placing ? 1 : 0) << ",\"x\":" << g_build.preview_x
        << ",\"z\":" << g_build.preview_z << ",\"fx\":" << g_build.footprint_x << ",\"fz\":" << g_build.footprint_z
        << ",\"builder\":" << builder << ",\"building\":" << g_observed_building
        << ",\"buildingNow\":" << building_now;
    if (have_start && start_unit.number) {
        out << ",\"startUnit\":" << static_cast<int>(start_unit.numeric);
    } else if (have_start) {
        out << ",\"startUnit\":\"" << start_unit.text << "\"";
    } else {
        out << ",\"startUnit\":null";
    }
    out << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

namespace {

bool role_for(const UnitSim* sim, int unit_id, UnitRole& role) {
    if (sim == nullptr || g_game_root.empty()) {
        return false;
    }
    const int index = sim_index_for(*sim, unit_id);
    if (index < 0) {
        return false;
    }
    std::string error;
    return load_unit_role(g_game_root, sim->def_name[static_cast<size_t>(index)], role, error) && role.ok;
}

}  // namespace

const std::vector<ActiveCmdDesc>& build_active_descs(const UnitSim* sim, int unit_id) {
    g_active_descs.clear();
    UnitRole role;
    if (!role_for(sim, unit_id, role)) {
        return g_active_descs;
    }
    // CMDTYPE from the engine's command type list as BAR's widgets read it (CMDTYPE table).
    constexpr int T_ICON = 0;
    constexpr int T_MODE = 5;
    constexpr int T_MAP = 10;
    constexpr int T_AREA = 11;
    constexpr int T_UNIT = 12;
    constexpr int T_UNIT_OR_MAP = 13;
    constexpr int T_UNIT_FEAT_AREA = 19;
    constexpr int T_BUILDING = 20;
    auto add = [&](int id, int type, const char* name, const char* action, const char* cursor) {
        g_active_descs.push_back({id, type, name, action, cursor});
    };
    add(45, T_MODE, "Firestate", "firestate", "cursorattack");
    add(50, T_MODE, "Movestate", "movestate", "cursormove");
    add(115, T_MODE, "Repeat", "repeat", "cursorattack");
    add(5, T_ICON, "Wait", "wait", "cursorwait");
    add(0, T_ICON, "Stop", "stop", "cursorstop");
    add(20, T_UNIT_OR_MAP, "Attack", "attack", "cursorattack");
    add(105, T_UNIT_OR_MAP, "DGun", "manualfire", "cursordgun");
    add(10, T_MAP, "Move", "move", "cursormove");
    add(15, T_MAP, "Patrol", "patrol", "cursorpatrol");
    add(16, T_UNIT_OR_MAP, "Fight", "fight", "cursorfight");
    add(25, T_UNIT, "Guard", "guard", "cursordefend");
    add(40, T_UNIT_FEAT_AREA, "Repair", "repair", "cursorrepair");
    add(90, T_UNIT_FEAT_AREA, "Reclaim", "reclaim", "cursorreclamate");
    add(110, T_AREA, "Restore", "restore", "cursorrestore");
    add(130, T_UNIT_FEAT_AREA, "Capture", "capture", "cursorcapture");
    add(65, T_ICON, "SelfD", "selfd", "cursorspecial");
    for (const std::string& option : role.options) {
        const int def_id = player_unitdef_id(option);
        if (def_id == 0) {
            continue;
        }
        g_active_descs.push_back({-def_id, T_BUILDING, option, "buildunit_" + option, option});
    }
    return g_active_descs;
}

int build_active_desc_index(const UnitSim* sim, int unit_id, int cmd_id) {
    const std::vector<ActiveCmdDesc>& descs = build_active_descs(sim, unit_id);
    for (size_t i = 0; i < descs.size(); ++i) {
        if (descs[i].id == cmd_id) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

int unit_fire_state(const UnitSim* sim, int unit_id) {
    const auto it = g_fire_state.find(unit_id);
    if (it != g_fire_state.end()) {
        return it->second;
    }
    UnitRole role;
    if (role_for(sim, unit_id, role) && role.fire_state >= 0) {
        return role.fire_state;
    }
    return 2;  // FIRE_AT_WILL when the unit file sets none
}

int unit_move_state(const UnitSim* sim, int unit_id) {
    const auto it = g_move_state.find(unit_id);
    if (it != g_move_state.end()) {
        return it->second;
    }
    UnitRole role;
    if (role_for(sim, unit_id, role) && role.move_state >= 0) {
        return role.move_state;
    }
    return 1;  // MANEUVER when the unit file sets none
}

int unit_repeat_state(int unit_id) {
    const auto it = g_repeat_state.find(unit_id);
    return it != g_repeat_state.end() ? it->second : 0;
}

bool unit_apply_state_command(const UnitSim* sim, int unit_id, int cmd_id, int value) {
    if (sim == nullptr || sim_index_for(*sim, unit_id) < 0) {
        return false;
    }
    if (cmd_id == 45 && value >= 0 && value <= 3) {
        g_fire_state[unit_id] = value;
    } else if (cmd_id == 50 && value >= 0 && value <= 2) {
        g_move_state[unit_id] = value;
    } else if (cmd_id == 115 && (value == 0 || value == 1)) {
        g_repeat_state[unit_id] = value;
    } else {
        return false;
    }
    g_commands_changed = true;
    std::cout << "unit state " << unit_id << " cmd " << cmd_id << " = " << value << "\n";
    return true;
}

bool build_take_commands_changed() {
    const bool changed = g_commands_changed;
    g_commands_changed = false;
    return changed;
}

bool build_activate_index(int index, int button, const UnitSim* sim, const std::vector<int>& selected,
                          std::string& error) {
    if (selected.empty()) {
        error = "no builder selected";
        return false;
    }
    const std::vector<ActiveCmdDesc> descs = build_active_descs(sim, selected.front());
    if (index < 1 || index > static_cast<int>(descs.size())) {
        error = "cmd desc index out of range";
        return false;
    }
    const ActiveCmdDesc& desc = descs[static_cast<size_t>(index - 1)];
    if (desc.id < 0) {
        if (!build_set_by_def(desc.name, -desc.id, sim, error)) {
            return false;
        }
        g_build.active_index = index;
        return true;
    }
    if (desc.id == 45 || desc.id == 50 || desc.id == 115) {
        const int count = desc.id == 45 ? 4 : (desc.id == 50 ? 3 : 2);
        const int lead = selected.front();
        const int current = desc.id == 45 ? unit_fire_state(sim, lead)
                            : desc.id == 50 ? unit_move_state(sim, lead)
                                            : unit_repeat_state(lead);
        // The menu shows 3 fire states; FIRE_AT_ALL (3) is reachable by command only.
        const int shown = desc.id == 45 ? 3 : count;
        int next = button == 1 ? current + 1 : current - 1;
        if (next >= shown) {
            next = 0;
        }
        if (next < 0) {
            next = shown - 1;
        }
        for (int unit : selected) {
            unit_apply_state_command(sim, unit, desc.id, next);
        }
        return true;
    }
    if (desc.id == 0) {
        build_clear_active_command();
        return true;
    }
    g_build = BuildCmdState{};
    g_build.active_cmd = desc.id;
    g_build.active_index = index;
    g_build.active_name = desc.name;
    return true;
}

bool build_live_left_click(UnitSim& sim, const TerrainMesh& terrain, int selected_unit_id, float world_x,
                           float world_z, bool shift, std::string& error) {
    if (!g_build.placing || g_build.def_name.empty()) {
        return false;
    }
    build_update_preview(terrain, world_x, world_z);
    if (!build_place(sim, selected_unit_id, terrain, error)) {
        std::cerr << "build: " << error << "\n";
        error.clear();
        return true;
    }
    g_observed_builder = selected_unit_id;
    const int building = unit_is_building(selected_unit_id);
    if (building > 0) {
        g_observed_building = building;
    }
    std::cout << "build place " << g_build.def_name << " at " << g_build.preview_x << " " << g_build.preview_z
              << "\n";
    if (!shift) {
        build_clear_active_command();
    }
    return true;
}

}  // namespace bar
