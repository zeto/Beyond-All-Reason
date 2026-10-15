#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// One ground sight test. Aircraft and underwater units are not governed here.
// The ray walks los squares and compares terrain to the line between the tops
// of the two collision boxes. A unit past sightdistance is not targetable.
struct LosCheck {
    std::string tag;
    std::string attacker_def;
    std::string target_def;
    int attacker = -1;
    int target = -1;
    int see = 0;
    int far = 0;
    int weapon = 0;
    float dist = 0;
    float sight = 0;
    float square = 0;
    float attacker_lift = 0;
    float target_lift = 0;
    std::vector<float> heights;
};

struct LosDuel {
    bool active = false;
    std::vector<LosCheck> checks;
};

// applied is false for aircraft, underwater units, and units without a box.
// A false return is a bad order. When applied and see is 0, the caller must
// not start a weapon.
bool los_consider(const std::string& game_root, int attacker, int target, const UnitSim& sim, const TerrainMesh& terrain,
                  bool& applied, LosCheck& check, std::string& error);

// Elmo size of one ground-LOS square from modrules losMipLevel. The weapon ray uses the same square.
bool los_map_square(const std::string& game_root, const TerrainMesh& terrain, float& square);

}  // namespace bar
