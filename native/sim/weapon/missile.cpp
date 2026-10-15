#include "weapon/missile.h"

#include "gadget_host.h"
#include "move/unit_sim.h"
#include "smf.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace bar {
namespace {

constexpr float kSimHz = 30.f;

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool open_unit_table(lua_State* state, const std::string& game_root, const std::string& def_name, std::string& error) {
    const std::filesystem::path root = std::filesystem::path(game_root) / "units";
    const std::filesystem::path direct = root / (def_name + ".lua");
    std::filesystem::path path;
    std::error_code ec;
    if (std::filesystem::is_regular_file(direct, ec)) {
        path = direct;
    } else {
        const std::string file_name = def_name + ".lua";
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() == file_name) {
                path = it->path();
                break;
            }
        }
    }
    if (path.empty()) {
        error = "unit def not found: " + def_name;
        return false;
    }
    if (luaL_loadfile(state, path.string().c_str()) != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "unit def failed";
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "unit def did not return a table: " + def_name;
        return false;
    }
    lua_getfield(state, -1, def_name.c_str());
    if (!lua_istable(state, -1)) {
        error = "unit def has no " + def_name + " table";
        return false;
    }
    return true;
}

bool read_box(lua_State* state, const std::string& def_name, MissileBox& box, std::string& error) {
    lua_getfield(state, -1, "collisionvolumetype");
    const char* type = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    if (lower_copy(type) != "box") {
        error = def_name + " collision volume is not a box";
        return false;
    }
    lua_getfield(state, -1, "collisionvolumescales");
    const char* scales = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    lua_getfield(state, -1, "collisionvolumeoffsets");
    const char* offsets = lua_isstring(state, -1) ? lua_tostring(state, -1) : "0 0 0";
    lua_pop(state, 1);
    std::istringstream scale_text(scales);
    std::istringstream offset_text(offsets);
    float sx = 0;
    float sy = 0;
    float sz = 0;
    if (!(scale_text >> sx >> sy >> sz) || !(sx > 0.f) || !(sy > 0.f) || !(sz > 0.f)) {
        error = def_name + " collision scales are missing";
        return false;
    }
    float ox = 0;
    float oy = 0;
    float oz = 0;
    if (!(offset_text >> ox >> oy >> oz)) {
        ox = 0;
        oy = 0;
        oz = 0;
    }
    box.hx = sx * 0.5f;
    box.hy = sy * 0.5f;
    box.hz = sz * 0.5f;
    box.ox = ox;
    box.oy = oy;
    box.oz = oz;
    return true;
}

bool load_health_and_box(const std::string& game_root, const std::string& def_name, float& health, MissileBox& box,
                         std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "health");
    if (!lua_isnumber(state, -1) || !(lua_tonumber(state, -1) > 0.0)) {
        error = def_name + " has no health";
        lua_close(state);
        return false;
    }
    health = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    const bool box_ok = read_box(state, def_name, box, error);
    lua_close(state);
    return box_ok;
}

float lua_number(lua_State* state, const char* key, float fallback, bool& present) {
    lua_getfield(state, -1, key);
    present = lua_isnumber(state, -1) != 0;
    const float value = present ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
    lua_pop(state, 1);
    return value;
}

bool lua_flag(lua_State* state, const char* key, bool fallback) {
    lua_getfield(state, -1, key);
    bool value = fallback;
    if (lua_isboolean(state, -1)) {
        value = lua_toboolean(state, -1) != 0;
    } else if (lua_isnumber(state, -1)) {
        value = lua_tonumber(state, -1) != 0.0;
    } else if (lua_isstring(state, -1)) {
        const std::string text = lower_copy(lua_tostring(state, -1));
        value = text == "true" || text == "1";
    }
    lua_pop(state, 1);
    return value;
}

bool lua_has_number(lua_State* state, const char* key, float& value) {
    lua_getfield(state, -1, key);
    const bool present = lua_isnumber(state, -1) != 0;
    if (present) {
        value = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    return present;
}

void muzzle_of(const UnitSim& sim, int index, const MissileBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy + box.hy;
    z = sim.z[at] + box.oz;
}

void aim_of(const UnitSim& sim, int index, const MissileBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy;
    z = sim.z[at] + box.oz;
}

float range_2d(float range, float height_diff, float height_mod) {
    const float ydiff = height_diff * height_mod;
    const float root = range * range - ydiff * ydiff;
    if (!(root > 0.f)) {
        return 0.f;
    }
    return std::sqrt(root);
}

bool in_range(float mx, float my, float mz, float ax, float ay, float az, float range, float height_mod) {
    const float dx = ax - mx;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float weapon_range = range_2d(range, ay - my, height_mod);
    return dist > 0.f && dist <= weapon_range;
}

float per_frame(float elmos_per_second) {
    return static_cast<float>(static_cast<double>(elmos_per_second) / static_cast<double>(kSimHz));
}

bool load_missile(const std::string& game_root, const std::string& def_name, MissileDuel& duel, std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    if (!open_unit_table(state, game_root, def_name, error)) {
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "weapons");
    if (!lua_istable(state, -1)) {
        error = def_name + " has no weapons";
        lua_close(state);
        return false;
    }
    lua_rawgeti(state, -1, 1);
    if (!lua_istable(state, -1)) {
        error = def_name + " has no weapon 1";
        lua_close(state);
        return false;
    }
    lua_getfield(state, -1, "def");
    if (!lua_isstring(state, -1)) {
        error = def_name + " weapon 1 has no def";
        lua_close(state);
        return false;
    }
    const std::string want = lower_copy(lua_tostring(state, -1));
    lua_pop(state, 1);
    lua_pop(state, 1);
    lua_pop(state, 1);
    lua_getfield(state, -1, "weapondefs");
    if (!lua_istable(state, -1)) {
        error = def_name + " has no weapondefs";
        lua_close(state);
        return false;
    }
    bool found = false;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        const bool name = lua_type(state, -2) == LUA_TSTRING && lower_copy(lua_tostring(state, -2)) == want;
        if (name && lua_istable(state, -1)) {
            found = true;
            lua_getfield(state, -1, "weapontype");
            const char* type = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
            lua_pop(state, 1);
            if (std::string(type) != "MissileLauncher") {
                error = def_name + " weapon 1 is not a MissileLauncher";
                lua_close(state);
                return false;
            }
            bool present = false;
            const float file_range = lua_number(state, "range", 10.f, present);
            const float reload = lua_number(state, "reloadtime", 1.f, present);
            const float start_velocity = lua_number(state, "startvelocity", 0.f, present);
            const float velocity = lua_number(state, "weaponvelocity", 0.f, present);
            const float acceleration = lua_number(state, "weaponacceleration", 0.f, present);
            const float accuracy = lua_number(state, "accuracy", 0.f, present);
            const float spray = lua_number(state, "sprayangle", 0.f, present);
            const float burst = lua_number(state, "burst", 1.f, present);
            const float energy = lua_number(state, "energypershot", 0.f, present);
            const float metal = lua_number(state, "metalpershot", 0.f, present);
            const float turn = lua_number(state, "turnrate", 0.f, present);
            const float wobble = lua_number(state, "wobble", 0.f, present);
            const float dance = lua_number(state, "dance", 0.f, present);
            const float flight = lua_number(state, "flighttime", 0.f, present);
            const float my_gravity = lua_number(state, "mygravity", 0.f, present);
            float keyed = 0.f;
            const bool low_mod = lua_has_number(state, "heightmod", keyed);
            const float low_value = keyed;
            const bool high_mod = lua_has_number(state, "heightMod", keyed);
            if (low_mod) {
                duel.height_mod = low_value;
            } else if (high_mod) {
                duel.height_mod = keyed;
            } else {
                duel.height_mod = 0.2f;
            }
            float arc = 0.f;
            if ((lua_has_number(state, "trajectoryheight", arc) && arc != 0.f) ||
                (lua_has_number(state, "trajectoryHeight", arc) && arc != 0.f)) {
                error = def_name + " missile arcs";
                lua_close(state);
                return false;
            }
            if ((lua_has_number(state, "cylindertargeting", keyed) && keyed >= 0.01f) ||
                (lua_has_number(state, "cylinderTargeting", keyed) && keyed >= 0.01f)) {
                error = def_name + " missile uses cylinder targeting";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "tracks", false) || lua_flag(state, "burnblow", false) ||
                lua_flag(state, "fixedlauncher", false)) {
                error = def_name + " missile homes, burns, or leaves the launcher";
                lua_close(state);
                return false;
            }
            if (accuracy != 0.f || spray != 0.f || burst != 1.f || energy != 0.f || metal != 0.f || turn != 0.f ||
                wobble != 0.f || dance != 0.f || flight != 0.f || my_gravity != 0.f) {
                error = def_name + " missile is not a single straight rocket";
                lua_close(state);
                return false;
            }
            lua_getfield(state, -1, "damage");
            float damage = 1.f;
            if (lua_istable(state, -1)) {
                lua_getfield(state, -1, "default");
                if (lua_isnumber(state, -1)) {
                    damage = static_cast<float>(lua_tonumber(state, -1));
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            if (!(damage > 0.f) || !(velocity > 0.f) || !(start_velocity > 0.f) || !(file_range > 0.f) ||
                !(reload > 0.f) || !(duel.height_mod > 0.f)) {
                error = def_name + " missile is missing range, speed, or damage";
                lua_close(state);
                return false;
            }
            duel.speed = per_frame(start_velocity);
            duel.max_speed = per_frame(velocity);
            duel.accel = static_cast<float>(static_cast<double>(acceleration) /
                                            (static_cast<double>(kSimHz) * static_cast<double>(kSimHz)));
            duel.range = file_range;
            duel.reload_frames = std::max(1, static_cast<int>(reload * kSimHz));
            duel.damage = damage;
        }
        lua_pop(state, 1);
    }
    lua_close(state);
    if (!found) {
        error = def_name + " weapon def " + want + " is missing";
        return false;
    }
    return true;
}

bool fire_rocket(MissileDuel& duel, const UnitSim& sim) {
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, duel.attacker, duel.attacker_box, mx, my, mz);
    aim_of(sim, duel.target, duel.target_box, ax, ay, az);
    if (!in_range(mx, my, mz, ax, ay, az, duel.range, duel.height_mod)) {
        return false;
    }
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.f) || !(duel.max_speed > 0.f)) {
        return false;
    }
    const float span = dist > duel.range ? dist : duel.range;
    const int ttl = static_cast<int>(std::ceil(span / duel.max_speed));
    if (ttl <= 0) {
        return false;
    }
    MissileRocket rocket;
    rocket.alive = true;
    rocket.ttl = ttl;
    rocket.x = mx;
    rocket.y = my;
    rocket.z = mz;
    rocket.speed = duel.speed;
    rocket.vx = dx / dist * duel.speed;
    rocket.vy = dy / dist * duel.speed;
    rocket.vz = dz / dist * duel.speed;
    duel.rockets.push_back(rocket);
    duel.muzzle_x = mx;
    duel.muzzle_y = my;
    duel.muzzle_z = mz;
    ++duel.shots;
    return true;
}

}  // namespace

bool missile_begin(MissileDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                   std::string& error) {
    if (duel.active) {
        error = "a missile duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "missile order has no unit";
        return false;
    }
    MissileDuel next;
    if (!load_missile(game_root, sim.def_name[static_cast<size_t>(attacker)], next, error)) {
        return false;
    }
    float attacker_hp = 0;
    if (!load_health_and_box(game_root, sim.def_name[static_cast<size_t>(attacker)], attacker_hp, next.attacker_box,
                             error)) {
        return false;
    }
    (void)attacker_hp;
    if (!load_health_and_box(game_root, sim.def_name[static_cast<size_t>(target)], next.target_hp, next.target_box,
                             error)) {
        return false;
    }
    next.target_hp_max = next.target_hp;
    next.attacker = attacker;
    next.target = target;
    next.attacker_def = sim.def_name[static_cast<size_t>(attacker)];
    next.target_def = sim.def_name[static_cast<size_t>(target)];
    next.active = true;
    next.reload_ready = 0;
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, attacker, next.attacker_box, mx, my, mz);
    aim_of(sim, target, next.target_box, ax, ay, az);
    if (!in_range(mx, my, mz, ax, ay, az, next.range, next.height_mod)) {
        error = "missile target is out of range";
        return false;
    }
    next.muzzle_x = mx;
    next.muzzle_y = my;
    next.muzzle_z = mz;
    duel = std::move(next);
    return true;
}

void missile_step(MissileDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    if (!duel.active) {
        return;
    }
    if (duel.die == 0 && sim_frame >= duel.reload_ready) {
        if (fire_rocket(duel, sim)) {
            duel.reload_ready = sim_frame + duel.reload_frames;
        }
    }
    const size_t target = static_cast<size_t>(duel.target);
    for (MissileRocket& rocket : duel.rockets) {
        if (!rocket.alive) {
            continue;
        }
        if (duel.die == 0 &&
            missile_rocket_hit(rocket, sim.x[target], sim.y[target], sim.z[target], duel.target_box)) {
            duel.target_hp =
                std::max(0.f, duel.target_hp - bar::behavior_adjust_damage(sim, duel.attacker, duel.target, duel.damage));
            ++duel.hits;
            if (!(duel.target_hp > 0.f)) {
                duel.target_hp = 0.f;
                duel.die = sim_frame;
            }
            rocket.alive = false;
            continue;
        }
        const float ground = terrain_height_at(terrain, rocket.x, rocket.z);
        if (rocket.y < ground) {
            ++duel.ground;
            rocket.alive = false;
            continue;
        }
        missile_rocket_advance(rocket, duel.accel, duel.max_speed);
    }
}

MissileTrace missile_trace(const MissileDuel& duel) {
    MissileTrace trace;
    if (!duel.active) {
        return trace;
    }
    trace.def_name = duel.attacker_def;
    trace.target_def = duel.target_def;
    trace.health = duel.target_hp;
    trace.die = duel.die;
    trace.hits = duel.hits;
    trace.shots = duel.shots;
    trace.ground = duel.ground;
    trace.damage = duel.damage;
    trace.speed = duel.speed;
    trace.max_speed = duel.max_speed;
    trace.accel = duel.accel;
    trace.range = duel.range;
    trace.reload_frames = duel.reload_frames;
    trace.height_mod = duel.height_mod;
    return trace;
}

}  // namespace bar
