#pragma once

#include "gl/draw_list.h"
#include "move/unit_sim.h"
#include "smf.h"

#include <string>
#include <vector>

namespace bar {

// Active build / order-cursor state shared with the stock grid menu.
struct BuildCmdState {
    int active_cmd = 0;       // Spring cmd id; build options are negative unitDefIDs
    int active_index = 0;     // 1-based index for GetActiveCommand
    std::string active_name;
    int facing = 0;           // 0..3
    bool placing = false;
    float preview_x = 0;
    float preview_z = 0;
    int footprint_x = 2;
    int footprint_z = 2;
    std::string def_name;
};

const BuildCmdState& build_cmd_state();

void build_cmd_reset();

// SetActiveCommand(cmd). cmd may be a 1-based index into the builder's cmd list, or a raw id.
bool build_set_active_command(int cmd_or_index, const UnitSim* sim, int selected_unit_id, std::string& error);

bool build_set_by_def(const std::string& def_name, int unit_def_id, const UnitSim* sim, std::string& error);

void build_cmd_set_root(const std::string& game_root);

void build_clear_active_command();

// Update ground preview under the pointer. Returns false when not placing.
bool build_update_preview(const TerrainMesh& terrain, float world_x, float world_z);

// Issue GiveOrder-style build at the preview site for the selected builder.
bool build_place(UnitSim& sim, int selected_unit_id, const TerrainMesh& terrain, std::string& error);

// Append a cyan footprint (no unit mesh) into the HUD when placing.
void build_append_preview(HudList& hud, const float* view_proj, int view_w, int view_h);

// Scripted events for the self-check.
enum class BuildEventKind { Pick, Place };

struct BuildEvent {
    int frame = 0;
    BuildEventKind kind = BuildEventKind::Pick;
    std::string def_name;
    float x = 0;
    float z = 0;
};

bool load_build_events(const std::string& path, std::vector<BuildEvent>& events, std::string& error);

bool apply_build_events(UnitSim& sim, const std::vector<BuildEvent>& events, int presented,
                        const TerrainMesh& terrain, int selected_unit_id, std::string& error);

bool write_build_report(const std::string& path, int frame, std::string& error);

// Cmd descs for a builder unit id (Spring unit id). Empty when unknown.
struct CmdDesc {
    int id = 0;
    std::string name;
    std::string type;
    std::string action;
};

std::vector<CmdDesc> build_unit_cmd_descs(const UnitSim& sim, int unit_id);

int build_cmd_desc_index(int cmd_id);

}  // namespace bar
