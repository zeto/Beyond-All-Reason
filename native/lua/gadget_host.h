#pragma once

#include "build/nanoframe.h"
#include "economy/energy_yield.h"
#include "economy/metal_extract.h"
#include "economy/team_resources.h"
#include "move/unit_sim.h"
#include "smf.h"
#include "startscript.h"

#include <string>
#include <vector>

namespace bar {

// What the synced spawn and resource gadgets asked for on this start script.
// Hide, paralyze, and movectrl are recorded and not applied: the move corpus
// starts on frame 1, and the commander warp stays out of this slice.
struct GadgetReport {
    bool loaded = false;
    std::string def_name;
    int team = -1;
    float x = 0;
    float y = 0;
    float z = 0;
    int unit = -1;
    int hide = 0;
    int paralyze = 0;
    int movectrl = 0;
    float metal = 0;
    float energy = 0;
    float metal_store = 0;
    float energy_store = 0;
    int start_pos_type = 0;
};

// Loads game_initial_spawn and game_team_resources, runs Initialize and GameStart.
// CreateUnit binds an already spawned commander instead of adding a second unit.
// Scavenger and raptor files load on every run. A false return leaves them disabled.
// One row per script team. The synced damage gadgets stay open for the rest of the run.
bool load_spawn_gadgets(const std::string& game_root, const StartScript& script, const ResourceRules& rules,
                        std::vector<TeamResources>& teams, UnitSim& sim, const TerrainMesh& terrain,
                        GadgetReport& report, std::string& error);

bool write_gadget_report(const std::string& path, const GadgetReport& report, std::string& error);

// Asks the loaded UnitPreDamaged gadgets what damage they would deal.
// A different return is recorded and not applied, so a gated duel keeps its shot.
float behavior_adjust_damage(const UnitSim& sim, int attacker, int target, float damage);

bool write_behavior_report(const std::string& path, std::string& error);

// Calls SimpleAI's GameFrame when that gadget loaded. A team without LuaAI does no work.
bool simpleai_game_frame(int frame, std::string& error);

// Calls an enabled scavenger or raptor GameFrame. A file that returned false is skipped.
bool mode_game_frame(int frame, std::string& error);

// Steps queued SimpleAI builds after GameFrame. State commands stay out of the queue.
// A finished extractor is added to the sim and to that team's mex income.
struct AiEconomy {
    UnitSim* sim = nullptr;
    std::vector<TeamResources>* teams = nullptr;
    MetalClaims* claims = nullptr;
    std::vector<MexTrace>* mex = nullptr;
    std::vector<GenTrace>* generators = nullptr;
    const ConstructionRules* construction = nullptr;
    const TerrainMesh* terrain = nullptr;
    std::string game_root;
    float energy_scale = 1;
    float metal_scale = 1;
    float wind_strength = 0;
};

bool simpleai_drive(const AiEconomy& eco, int frame, std::string& error);

bool write_ai_report(const std::string& path, std::string& error);

void gadget_session_close();

}  // namespace bar
