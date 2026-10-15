#pragma once

#include "smf.h"

#include <string>
#include <vector>

namespace bar {

// One placed extractor. raw is the sum of metal-map bytes whose square centers
// fall inside the map extractor radius. rate is metal per second.
struct MexTrace {
    std::string def_name;
    float x = 0;
    float z = 0;
    float depth = 0;
    float scale = 0;
    float rate = 0;
    int raw = 0;
    int squares = 0;
};

// Depth already claimed on each metal square, so a second mex on the same square
// only receives the depth the first one left.
struct MetalClaims {
    std::vector<float> taken;
    int size_x = 0;
    int size_z = 0;
    float square = 0;
    float scale = 0;
    float radius = 0;
};

bool metal_claims_open(MetalClaims& claims, const TerrainMesh& mesh, std::string& error);

// depth is extractsmetal after the extraction multiplier. A square already dug to
// that depth contributes nothing more.
MexTrace metal_claim(MetalClaims& claims, const TerrainMesh& mesh, const std::string& def_name, float x, float z,
                     float depth);

}  // namespace bar
