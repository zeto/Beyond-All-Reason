#include "weapon/laser.h"

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

bool read_box(lua_State* state, const std::string& def_name, LaserBox& box, std::string& error) {
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

bool load_health_and_box(const std::string& game_root, const std::string& def_name, float& health, LaserBox& box,
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

bool load_laser(const std::string& game_root, const std::string& def_name, LaserDuel& duel, std::string& error) {
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
            if (std::string(type) != "LaserCannon") {
                error = def_name + " weapon 1 is not a LaserCannon";
                lua_close(state);
                return false;
            }
            bool present = false;
            const float file_range = lua_number(state, "range", 10.f, present);
            const float reload = lua_number(state, "reloadtime", 1.f, present);
            const float velocity = lua_number(state, "weaponvelocity", 0.f, present);
            const float accuracy = lua_number(state, "accuracy", 0.f, present);
            const float spray = lua_number(state, "sprayangle", 0.f, present);
            const float burst = lua_number(state, "burst", 1.f, present);
            const float energy = lua_number(state, "energypershot", 0.f, present);
            if (accuracy != 0.f || spray != 0.f || burst != 1.f || energy != 0.f) {
                error = def_name + " laser is not a single accurate shot";
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
            lua_getfield(state, -1, "burnblow");
            const bool burn_set = !lua_isnil(state, -1);
            const bool burn = burn_set ? lua_toboolean(state, -1) != 0 : true;
            lua_pop(state, 1);
            if (!(damage > 0.f) || !(velocity > 0.f) || !(file_range > 0.f) || !(reload > 0.f)) {
                error = def_name + " laser is missing range, speed, or damage";
                lua_close(state);
                return false;
            }
            const double per_frame = static_cast<double>(velocity) / static_cast<double>(kSimHz);
            const double rounded = std::max(1.0, std::floor(static_cast<double>(file_range) / per_frame)) * per_frame;
            duel.speed = static_cast<float>(per_frame);
            duel.range = static_cast<float>(rounded);
            duel.reload_frames = std::max(1, static_cast<int>(static_cast<double>(reload) * static_cast<double>(kSimHz)));
            duel.damage = damage;
            duel.burnblow = burn;
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

void muzzle_of(const UnitSim& sim, int index, const LaserBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy + box.hy;
    z = sim.z[at] + box.oz;
}

float distance3(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float dz = bz - az;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool fire_bolt(LaserDuel& duel, const UnitSim& sim) {
    float mx = 0;
    float my = 0;
    float mz = 0;
    muzzle_of(sim, duel.attacker, duel.attacker_box, mx, my, mz);
    const size_t target = static_cast<size_t>(duel.target);
    const float tx = sim.x[target];
    const float ty = sim.y[target];
    const float tz = sim.z[target];
    const float dist = distance3(mx, my, mz, tx, ty, tz);
    if (!(dist > 0.f) || dist > duel.range) {
        return false;
    }
    LaserBolt bolt;
    bolt.alive = true;
    bolt.check = true;
    bolt.owner = duel.attacker;
    bolt.x = mx;
    bolt.y = my;
    bolt.z = mz;
    bolt.dx = (tx - mx) / dist;
    bolt.dy = (ty - my) / dist;
    bolt.dz = (tz - mz) / dist;
    bolt.speed = duel.speed;
    const int ttlreq = static_cast<int>(std::ceil(static_cast<double>(dist) / static_cast<double>(duel.speed)));
    const int ttlmax = static_cast<int>(std::floor(static_cast<double>(duel.range) / static_cast<double>(duel.speed))) - 1;
    bolt.ttl = duel.burnblow ? std::min(ttlreq, ttlmax) : std::max(ttlreq, ttlmax);
    duel.bolts.push_back(bolt);
    duel.muzzle_x = mx;
    duel.muzzle_y = my;
    duel.muzzle_z = mz;
    ++duel.shots;
    return true;
}

}  // namespace

bool laser_begin(LaserDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                 std::string& error) {
    if (duel.active) {
        error = "a laser duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "laser order has no unit";
        return false;
    }
    LaserDuel next;
    if (!load_laser(game_root, sim.def_name[static_cast<size_t>(attacker)], next, error)) {
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
    muzzle_of(sim, attacker, next.attacker_box, mx, my, mz);
    const float dist = distance3(mx, my, mz, sim.x[static_cast<size_t>(target)], sim.y[static_cast<size_t>(target)],
                                 sim.z[static_cast<size_t>(target)]);
    if (!(dist > 0.f) || dist > next.range) {
        error = "laser target is out of range";
        return false;
    }
    next.muzzle_x = mx;
    next.muzzle_y = my;
    next.muzzle_z = mz;
    duel = std::move(next);
    return true;
}

void laser_step(LaserDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    if (!duel.active) {
        return;
    }
    if (duel.die == 0 && sim_frame >= duel.reload_ready) {
        if (fire_bolt(duel, sim)) {
            duel.reload_ready = sim_frame + duel.reload_frames;
        }
    }
    for (LaserBolt& bolt : duel.bolts) {
        if (!bolt.alive || !bolt.check) {
            continue;
        }
        const float ground = terrain_height_at(terrain, bolt.x, bolt.z);
        if (bolt.y < ground) {
            ++duel.ground;
            bolt.alive = false;
            bolt.check = false;
            continue;
        }
        float best = 0;
        int hit = -1;
        for (int index = 0; index < static_cast<int>(sim.x.size()); ++index) {
            if (index == bolt.owner) {
                continue;
            }
            const LaserBox& box = index == duel.target ? duel.target_box : LaserBox{};
            if (index != duel.target) {
                continue;
            }
            float travel = 0;
            if (!laser_bolt_hit(bolt, sim.x[static_cast<size_t>(index)], sim.y[static_cast<size_t>(index)],
                                sim.z[static_cast<size_t>(index)], box, travel)) {
                continue;
            }
            if (hit < 0 || travel < best) {
                hit = index;
                best = travel;
            }
        }
        if (hit == duel.target && duel.die == 0) {
            duel.target_hp = std::max(
                0.f, duel.target_hp - bar::behavior_adjust_damage(sim, bolt.owner, hit, duel.damage));
            ++duel.hits;
            if (!(duel.target_hp > 0.f)) {
                duel.target_hp = 0.f;
                duel.die = sim_frame;
            }
            bolt.alive = false;
            bolt.check = false;
            continue;
        }
        laser_bolt_advance(bolt);
    }
}

LaserTrace laser_trace(const LaserDuel& duel) {
    LaserTrace trace;
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
    trace.range = duel.range;
    trace.reload_frames = duel.reload_frames;
    return trace;
}

}  // namespace bar
