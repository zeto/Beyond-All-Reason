#pragma once

#include <string>
#include <vector>

namespace bar {

// One corner of a screen-space quad. y grows up from the bottom of the view.
// The render thread reads these after the widget phase publishes them. It does not call Lua.
struct HudVertex {
    float x = 0;
    float y = 0;
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
};

// A glyph run. align is 0 left, 1 center, 2 right. y is the baseline.
struct HudText {
    std::string text;
    std::string font_path;
    float x = 0;
    float y = 0;
    float size = 16;
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
    int align = 0;
    bool outline = false;
};

// Immutable draw list for one view. Quads are groups of four vertices.
struct HudList {
    std::vector<HudVertex> vertices;
    std::vector<HudText> texts;
    int width = 0;
    int height = 0;
};

}  // namespace bar
