#include "autocolors.h"

#include <set>

namespace bar {
namespace {
float g_ui_color[3] = {0.9f, 0.15f, 0.12f};
}

bool apply_bar_autocolors(StartScript& script, std::string& note) {
    // game_autocolors.lua with SimpleTeamColors 0 (the springsettings default; Jose's cfg
    // does not set it) and no teamcolors_anonymous_mode modoption:
    //  - one ally team holding one team: both the FFA branch (ffaColors[allyTeamID + 1],
    //    no variation for a one-team ally team) and the team-table branch
    //    (teamColors[1][1][1], variation 0) give "#004DFF" (Armada Blue).
    std::set<int> allies;
    for (const TeamStart& team : script.teams) {
        allies.insert(team.ally_team);
    }
    if (script.teams.size() != 1 || allies.size() != 1 || script.teams[0].ally_team != 0) {
        note = "autocolors: layout not ported, keeping startscript RGBColor";
        return false;
    }
    TeamStart& team = script.teams[0];
    team.color_r = 0x00 / 255.f;
    team.color_g = 0x4D / 255.f;
    team.color_b = 0xFF / 255.f;
    team.has_color = true;
    note = "autocolors: team 0 -> #004DFF";
    return true;
}

void set_ui_team_color(float r, float g, float b) {
    g_ui_color[0] = r;
    g_ui_color[1] = g;
    g_ui_color[2] = b;
}

void ui_team_color(float& r, float& g, float& b) {
    r = g_ui_color[0];
    g = g_ui_color[1];
    b = g_ui_color[2];
}

}  // namespace bar
