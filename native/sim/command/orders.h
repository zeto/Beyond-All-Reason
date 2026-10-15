#pragma once

#include "move/unit_sim.h"

#include <string>
#include <vector>

namespace bar {

enum class OrderKind { Move, Build, Make, Reclaim, Attack };

// A move line is `frame unit x z`. A build line is `frame unit build def x z`.
// A make line is `frame unit make def`. The product is placed on that unit.
// A reclaim line is `frame unit reclaim def x z`. The def is the unit whose wreck is reclaimed.
// An attack line is `frame unit attack target`. The target is the sim index to shoot.
struct ScriptOrder {
    int frame = 0;
    int unit = 0;
    OrderKind kind = OrderKind::Move;
    float x = 0;
    float z = 0;
    int target = -1;
    std::string build_def;
};

bool load_orders(const std::string& path, std::vector<ScriptOrder>& orders, std::string& error);

bool apply_orders(UnitSim& sim, const std::vector<ScriptOrder>& orders, int frame, const TerrainMesh& terrain,
                  std::string& error);

}  // namespace bar