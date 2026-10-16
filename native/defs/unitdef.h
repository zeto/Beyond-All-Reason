#pragma once

#include <string>
#include <vector>

namespace bar {

struct UnitMotion {
    bool can_move = false;
    bool floater = false;
    bool can_fly = false;
    // A gunship (hoverattack) can stop and face. An airplane cannot.
    bool hover_attack = false;
    float speed_per_sec = 0;
    float max_acc = 0;
    float max_dec = 0;
    float turn_rate_raw = 0;
    float waterline = 0;
    float cruise_altitude = 0;
    // Airplane control surfaces, radians per frame. 0 on a ground unit.
    float max_rudder = 0;
    float max_aileron = 0;
    float max_bank = 0;
    float max_pitch = 0;
    // Minimum turn radius in elmos. A hint the yaw rate may not outrun.
    float turn_radius = 0;
    std::string move_class;
};

// Depth defaults match Spring when the move class omits a field: min -1e6, max +1e6.
// A ship with no maxslope gets 90 so the sea floor is not the constraint.
struct MoveClass {
    float max_slope_deg = 0;
    float max_water_depth = 1.0e6f;
    float min_water_depth = -1.0e6f;
    // A HOVER move class rides the water surface. Underwater slope is not the ground it climbs.
    bool hover = false;
};

// extractsmetal is the dig depth. energy_upkeep is energy per second while the extractor is on.
// metal_extractor is the custom param alldefs_post uses before it scales the depth.
struct UnitExtractor {
    bool active = false;
    float depth = 0;
    float energy_upkeep = 0;
    float metal_extractor = 0;
};

// energy_upkeep below zero is energy produced while the unit is on. wind_generator
// is the cap on map wind, not a multiplier. 0 means the unit is not that building.
struct UnitPower {
    bool active = false;
    float energy_upkeep = 0;
    float wind_generator = 0;
};

// Continuous income is per second. Storage is the unit's own tank, not the team's.
struct UnitYield {
    float metal_make = 0;
    float energy_make = 0;
    float metal_storage = 0;
    float energy_storage = 0;
};

// Path of units/**/<name>.lua under game_root, empty when absent.
std::string find_unit_file(const std::string& game_root, const std::string& def_name);

// Loads units/<name>.lua and reads objectname. The table is the unit def itself.
bool load_unit_object(const std::string& game_root, const std::string& def_name, std::string& object_name,
                      std::string& error);

// customparams.normaltex, empty when the def has none.
bool load_unit_normaltex(const std::string& game_root, const std::string& def_name, std::string& path,
                         std::string& error);

bool load_unit_yield(const std::string& game_root, const std::string& def_name, UnitYield& yield, std::string& error);

bool load_unit_extractor(const std::string& game_root, const std::string& def_name, UnitExtractor& extractor,
                         std::string& error);

bool load_unit_power(const std::string& game_root, const std::string& def_name, UnitPower& power, std::string& error);

// Raw unit-file numbers. Build-power and build-distance multipliers are applied by the sim.
struct UnitBuild {
    float build_time = 0;
    float metal_cost = 0;
    float energy_cost = 0;
    float health = 0;
    int footprint_x = 0;
    int footprint_z = 0;
};

struct UnitBuilder {
    bool builder = false;
    float worker_time = 0;
    float build_distance = 0;
    std::vector<std::string> options;
};

bool load_unit_build(const std::string& game_root, const std::string& def_name, UnitBuild& build, std::string& error);

bool load_unit_builder(const std::string& game_root, const std::string& def_name, UnitBuilder& builder,
                       std::string& error);

// Fields ai_simpleai reads while classifying UnitDefs. Missing files leave ok false.
struct UnitRole {
    bool ok = false;
    bool commander = false;
    bool builder = false;
    bool can_move = false;
    bool building = false;
    float extracts_metal = 0;
    float energy_make = 0;
    float energy_upkeep = 0;
    bool has_upkeep = false;
    float wind_generator = 0;
    float tidal_generator = 0;
    float transport_capacity = 0;
    bool solar = false;
    bool metal_extractor = false;
    bool converter = false;
    int footprint_x = 0;
    int footprint_z = 0;
    std::vector<std::string> options;
    // Factory is an immobile yardmap builder. can_move stays the file value.
    bool yardmap = false;
    float speed = 0;
    float worker_time = 0;
    bool can_fly = false;
    bool factory = false;
    bool has_weapon = false;
};

bool load_unit_role(const std::string& game_root, const std::string& def_name, UnitRole& role, std::string& error);

// Raw corpse numbers from featuredefs.dead. Wreck-metal post-processing is applied by the sim.
struct UnitWreck {
    bool present = false;
    bool reclaimable = false;
    bool has_reclaim_time = false;
    float reclaim_time = 0;
    float metal = 0;
    float energy = 0;
    bool commander = false;
    bool effigy = false;
    bool has_health = false;
    bool has_metal_cost = false;
    bool has_energy_cost = false;
    float health = 0;
    float metal_cost = 0;
    float energy_cost = 0;
};

// Reclaim speed is the file value when present, otherwise workerTime. Scale is applied by the sim.
struct UnitReclaim {
    bool can_reclaim = false;
    bool explicit_speed = false;
    float reclaim_speed = 0;
    float worker_time = 0;
    float build_distance = 0;
};

bool load_unit_health(const std::string& game_root, const std::string& def_name, float& health, std::string& error);

bool load_unit_max_weapon_range(const std::string& game_root, const std::string& def_name, float& range,
                               std::string& error);

bool load_unit_wreck(const std::string& game_root, const std::string& def_name, UnitWreck& wreck, std::string& error);

bool load_unit_reclaim(const std::string& game_root, const std::string& def_name, UnitReclaim& reclaim,
                       std::string& error);

// Sensor radii from the unit file. A missing air sight is 1.5 times sight. A _scav copy uses the base file.
struct UnitSensors {
    float sight = 0;
    float air = 0;
    float radar = 0;
    float sonar = 0;
    float sight_emit = 20;
    float radar_emit = 20;
};

bool load_unit_sensors(const std::string& game_root, const std::string& def_name, UnitSensors& sensors,
                       std::string& error);

bool load_unit_motion(const std::string& game_root, const std::string& def_name, UnitMotion& motion,
                      std::string& error);

// Movement-class slope is the value the engine uses after the file's 1.5 round trip.
bool load_move_class(const std::string& game_root, const std::string& class_name, MoveClass& move_class,
                     std::string& error);

}  // namespace bar
