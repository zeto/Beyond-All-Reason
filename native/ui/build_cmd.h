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

void build_cycle_facing(int absolute_or_neg1);

// GetActiveCmdDescs entry for the selection. Build options carry id -unitDefID.
struct ActiveCmdDesc {
    int id = 0;
    int type = 0;  // Recoil CMDTYPE
    std::string name;
    std::string action;
    std::string cursor;
};

// One list in one order for GetActiveCmdDescs, GetCmdDescIndex, FindUnitCmdDesc and SetActiveCommand(index).
const std::vector<ActiveCmdDesc>& build_active_descs(const UnitSim* sim, int unit_id);
// 1-based index of cmd id in build_active_descs, 0 when absent.
int build_active_desc_index(const UnitSim* sim, int unit_id, int cmd_id);
// SetActiveCommand(index, button): builds start placement, state commands cycle (button 1 forward,
// otherwise back) on every selected unit, order commands become the active cursor command.
bool build_activate_index(int index, int button, const UnitSim* sim, const std::vector<int>& selected,
                          std::string& error);

// CMD.FIRE_STATE / CMD.MOVE_STATE / CMD.REPEAT as unit state (GetUnitStates, cmd desc params).
int unit_fire_state(const UnitSim* sim, int unit_id);
int unit_move_state(const UnitSim* sim, int unit_id);
int unit_repeat_state(int unit_id);
// Applies a state command (45, 50, 115) to one unit. False for other ids or bad values.
bool unit_apply_state_command(const UnitSim* sim, int unit_id, int cmd_id, int value);

// True once after a unit state or the active command changed (widget:CommandsChanged).
bool build_take_commands_changed();

// Live placement: true when a build is active and the click was consumed.
bool build_live_left_click(UnitSim& sim, const TerrainMesh& terrain, int selected_unit_id, float world_x,
                           float world_z, bool shift, std::string& error);

}  // namespace bar
