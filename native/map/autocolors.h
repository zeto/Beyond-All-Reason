#pragma once

#include "startscript.h"

namespace bar {

// BAR's luarules/gadgets/game_autocolors.lua replaces every startscript RGBColor with
// Spring.SetTeamColor at load. This applies the same result for the layouts bar-metal
// supports. Returns false (teams left unchanged) for layouts not ported yet.
bool apply_bar_autocolors(StartScript& script, std::string& note);

// Spring.GetTeamColor for the widget host (set from the lead team after autocolors).
void set_ui_team_color(float r, float g, float b);
void ui_team_color(float& r, float& g, float& b);

}  // namespace bar
