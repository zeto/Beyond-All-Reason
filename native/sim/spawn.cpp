#include "unit_spawn.h"

#include "pool.h"
#include "unitdef.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace bar {
namespace {

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string commander_for_side(const std::string& side) {
    const std::string lower = lower_copy(side);
    if (lower.find("cor") != std::string::npos) {
        return "corcom";
    }
    if (lower.find("leg") != std::string::npos) {
        return "legcom";
    }
    return "armcom";
}

bool map_start(const TerrainMesh& terrain, int team, float& x, float& z) {
    for (const MapStart& start : terrain.starts) {
        if (start.team == team) {
            x = start.x;
            z = start.z;
            return true;
        }
    }
    return false;
}

void append_placed(ModelMesh& dst, const ModelMesh& src) {
    const uint32_t base = static_cast<uint32_t>(dst.vertices.size());
    for (const TerrainVertex& vertex : src.vertices) {
        dst.vertices.push_back(vertex);
    }
    for (uint32_t index : src.indices) {
        dst.indices.push_back(base + index);
    }
    if (dst.color_bc3.empty()) {
        dst.color_bc3 = src.color_bc3;
        dst.other_bc3 = src.other_bc3;
        dst.color_mips = src.color_mips;
        dst.other_mips = src.other_mips;
        dst.tex_size = src.tex_size;
        dst.normal_bc3 = src.normal_bc3;
        dst.normal_mips = src.normal_mips;
        dst.normal_size = src.normal_size;
        dst.s3o_radius = src.s3o_radius;
        dst.s3o_height = src.s3o_height;
    }
    if (dst.pieces.empty()) {
        dst.pieces = src.pieces;
    }
}

}  // namespace

bool spawn_commanders(const std::string& game_root, const std::string& data_dir, const StartScript& script,
                      const TerrainMesh& terrain, std::vector<UnitSpawn>& units, ModelMesh& model, std::string& error) {
    units.clear();
    model = ModelMesh{};
    if (script.teams.empty()) {
        return true;
    }
    GameContent content;
    if (!content.open(data_dir, error)) {
        return false;
    }
    int next_id = 1;
    for (const TeamStart& team : script.teams) {
        float x = 0;
        float z = 0;
        if (team.has_start) {
            x = team.start_x;
            z = team.start_z;
        } else if (!map_start(terrain, team.team, x, z)) {
            error = "team " + std::to_string(team.team) + " has no start position";
            return false;
        }
        const std::string def_name = team.unit_def.empty() ? commander_for_side(team.side) : lower_copy(team.unit_def);
        std::string object_name;
        if (!load_unit_object(game_root, def_name, object_name, error)) {
            return false;
        }
        ModelMesh loaded;
        if (!load_s3o(content, object_name, loaded, error)) {
            return false;
        }
        // cus_gl4 GetNormal: customParams.normaltex when the file exists, else blankNormalMap.
        std::string normal_path;
        if (!load_unit_normaltex(game_root, def_name, normal_path, error)) {
            return false;
        }
        // blank_normal.dds is a 1x1 flat normal; an empty normal_bc3 renders the same tbn (0, 0, 1).
        std::string normal_error;
        if (!normal_path.empty() && !load_model_normal(content, normal_path, loaded, normal_error)) {
            loaded.normal_bc3.clear();
            loaded.normal_mips.clear();
            loaded.normal_size = 0;
        }
        if (!model.color_bc3.empty() && loaded.tex_size != model.tex_size) {
            error = "a second unit uses a different texture";
            return false;
        }
        UnitMotion motion;
        if (!load_unit_motion(game_root, def_name, motion, error)) {
            return false;
        }
        const float ground = terrain_height_at(terrain, x, z);
        const float surface = ground < 0.f ? 0.f : ground;
        float y = motion.floater ? motion.waterline : ground;
        if (motion.can_fly) {
            y = surface + motion.cruise_altitude;
        } else if (!motion.move_class.empty()) {
            MoveClass move_class;
            if (!load_move_class(game_root, motion.move_class, move_class, error)) {
                return false;
            }
            if (move_class.hover) {
                y = surface;
            }
        }
        if (model.vertices.empty()) {
            append_placed(model, loaded);
        }
        UnitSpawn spawn;
        spawn.id = next_id++;
        spawn.team = team.team;
        spawn.def_name = def_name;
        spawn.x = x;
        spawn.y = y;
        spawn.z = z;
        spawn.color_r = team.color_r;
        spawn.color_g = team.color_g;
        spawn.color_b = team.color_b;
        units.push_back(spawn);
    }
    for (const ScriptUnit& extra : script.extras) {
        if (extra.def_name.empty() || !extra.has_x || !extra.has_z) {
            error = "a placed unit needs Def, X, and Z";
            return false;
        }
        UnitMotion motion;
        if (!load_unit_motion(game_root, extra.def_name, motion, error)) {
            return false;
        }
        const float ground = terrain_height_at(terrain, extra.x, extra.z);
        const float surface = ground < 0.f ? 0.f : ground;
        float y = motion.floater ? motion.waterline : ground;
        if (motion.can_fly) {
            y = surface + motion.cruise_altitude;
        } else if (!motion.move_class.empty()) {
            MoveClass move_class;
            if (!load_move_class(game_root, motion.move_class, move_class, error)) {
                return false;
            }
            if (move_class.hover) {
                y = surface;
            }
        }
        float color_r = 0.9f;
        float color_g = 0.15f;
        float color_b = 0.12f;
        for (const TeamStart& team : script.teams) {
            if (team.team == extra.team && team.has_color) {
                color_r = team.color_r;
                color_g = team.color_g;
                color_b = team.color_b;
            }
        }
        UnitSpawn spawn;
        spawn.id = next_id++;
        spawn.team = extra.team;
        spawn.def_name = extra.def_name;
        spawn.x = extra.x;
        spawn.y = y;
        spawn.z = extra.z;
        spawn.color_r = color_r;
        spawn.color_g = color_g;
        spawn.color_b = color_b;
        units.push_back(spawn);
    }
    return !units.empty();
}

bool write_spawn_trace(const std::string& path, const std::vector<UnitSpawn>& units, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "could not write spawn trace " + path;
        return false;
    }
    out << "{\"f\":0,\"units\":[";
    for (size_t i = 0; i < units.size(); ++i) {
        const UnitSpawn& unit = units[i];
        if (i != 0) {
            out << ',';
        }
        out << "{\"id\":" << unit.id << ",\"def\":\"" << unit.def_name << "\",\"t\":" << unit.team << ",\"x\":" << unit.x
            << ",\"y\":" << unit.y << ",\"z\":" << unit.z << "}";
    }
    out << "]}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar
