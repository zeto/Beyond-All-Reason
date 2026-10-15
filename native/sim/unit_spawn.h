#pragma once

#include "s3o.h"
#include "smf.h"
#include "startscript.h"

#include <string>
#include <vector>

namespace bar {

struct UnitSpawn {
    int id = 0;
    int team = 0;
    std::string def_name;
    float x = 0;
    float y = 0;
    float z = 0;
    float color_r = 0.9f;
    float color_g = 0.15f;
    float color_b = 0.12f;
};

// One commander per team. An empty team list leaves the model empty and succeeds.
bool spawn_commanders(const std::string& game_root, const std::string& data_dir, const StartScript& script,
                      const TerrainMesh& terrain, std::vector<UnitSpawn>& units, ModelMesh& model, std::string& error);

bool write_spawn_trace(const std::string& path, const std::vector<UnitSpawn>& units, std::string& error);

}  // namespace bar
