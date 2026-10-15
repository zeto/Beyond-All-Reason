#include "weapon/dgun.h"

#include "gadget_host.h"
#include "move/unit_sim.h"
#include "smf.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
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

bool read_aabb(lua_State* state, const std::string& def_name, DgunBox& box, bool allow_cylinder, std::string& error) {
    lua_getfield(state, -1, "collisionvolumetype");
    const char* type_text = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    const std::string type = lower_copy(type_text);
    const bool cylinder = type == "cyly" || type == "cylx" || type == "cylz" || type == "cylinder";
    if (type != "box" && !(allow_cylinder && cylinder)) {
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

bool load_health_and_volume(const std::string& game_root, const std::string& def_name, float& health, DgunBox& box,
                            bool allow_cylinder, std::string& error) {
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
    const bool box_ok = read_aabb(state, def_name, box, allow_cylinder, error);
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

void muzzle_of(const UnitSim& sim, int index, const DgunBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy + box.hy;
    z = sim.z[at] + box.oz;
}

void aim_of(const UnitSim& sim, int index, const DgunBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy;
    z = sim.z[at] + box.oz;
}

bool slab(float origin, float direction, float min_bound, float max_bound, float& tmin, float& tmax) {
    if (std::fabs(direction) < 1.0e-8f) {
        return origin >= min_bound && origin <= max_bound;
    }
    float enter = (min_bound - origin) / direction;
    float leave = (max_bound - origin) / direction;
    if (enter > leave) {
        const float swap = enter;
        enter = leave;
        leave = swap;
    }
    tmin = tmin > enter ? tmin : enter;
    tmax = tmax < leave ? tmax : leave;
    return tmin <= tmax;
}

bool segment_hits(float px, float py, float pz, float vx, float vy, float vz, float cx, float cy, float cz, float hx,
                  float hy, float hz) {
    float tmin = 0.f;
    float tmax = 1.f;
    if (!slab(px, vx, cx - hx, cx + hx, tmin, tmax) || !slab(py, vy, cy - hy, cy + hy, tmin, tmax) ||
        !slab(pz, vz, cz - hz, cz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    return true;
}

bool in_sphere(float mx, float my, float mz, float ax, float ay, float az, float range) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    return dist > 0.f && dist <= range;
}

bool load_dgun(const std::string& game_root, const std::string& def_name, DgunDuel& duel, std::string& error) {
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
            if (std::string(type) != "DGun") {
                error = def_name + " weapon 1 is not a DGun";
                lua_close(state);
                return false;
            }
            bool present = false;
            const float file_range = lua_number(state, "range", 10.f, present);
            const float reload = lua_number(state, "reloadtime", 1.f, present);
            const float burst_f = lua_number(state, "burst", 1.f, present);
            const float rate = lua_number(state, "burstrate", 0.f, present);
            const float accuracy = lua_number(state, "accuracy", 0.f, present);
            const float spray = lua_number(state, "sprayangle", 0.f, present);
            const float energy = lua_number(state, "energypershot", 0.f, present);
            const float metal = lua_number(state, "metalpershot", 0.f, present);
            const float velocity = lua_number(state, "weaponvelocity", 0.f, present);
            const bool gravity_affected = lua_flag(state, "gravityaffected", false);
            if (accuracy != 0.f || spray != 0.f) {
                error = def_name + " dgun sprays";
                lua_close(state);
                return false;
            }
            if (metal != 0.f) {
                error = def_name + " dgun spends metal";
                lua_close(state);
                return false;
            }
            if (energy != 0.f) {
                error = def_name + " dgun spends energy";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "waterweapon", false)) {
                error = def_name + " dgun is a water weapon";
                lua_close(state);
                return false;
            }
            if (!lua_flag(state, "noexplode", false)) {
                error = def_name + " dgun expires on impact";
                lua_close(state);
                return false;
            }
            if (!gravity_affected) {
                error = def_name + " dgun ignores gravity";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "groundbounce", false) || lua_flag(state, "waterbounce", false)) {
                error = def_name + " dgun bounces";
                lua_close(state);
                return false;
            }
            const int burst = static_cast<int>(burst_f);
            if (burst < 1 || static_cast<float>(burst) != burst_f) {
                error = def_name + " dgun burst is partial";
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
            if (!(damage > 0.f) || !(file_range > 0.f) || !(reload > 0.f) || !(velocity > 0.f)) {
                error = def_name + " dgun is missing range or damage";
                lua_close(state);
                return false;
            }
            duel.damage = damage;
            duel.range = file_range;
            duel.speed = velocity / kSimHz;
            duel.burst = burst;
            duel.salvo_delay = static_cast<int>(rate * kSimHz);
            duel.reload_frames = std::max(1, static_cast<int>(reload * kSimHz));
            duel.waterweapon = false;
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

void fire_shot(DgunDuel& duel, const UnitSim& sim) {
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, duel.attacker, duel.attacker_box, mx, my, mz);
    aim_of(sim, duel.target, duel.target_box, ax, ay, az);
    duel.muzzle_x = mx;
    duel.muzzle_y = my;
    duel.muzzle_z = mz;
    ++duel.shots;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.f) || !(duel.speed > 0.f)) {
        ++duel.ground;
        return;
    }
    const float inv = 1.f / dist;
    DgunShot shot;
    shot.x = mx;
    shot.y = my;
    shot.z = mz;
    shot.ox = mx;
    shot.oy = my;
    shot.oz = mz;
    shot.vx = dx * inv * duel.speed;
    shot.vy = dy * inv * duel.speed;
    shot.vz = dz * inv * duel.speed;
    duel.shots_live.push_back(shot);
}

void step_shots(DgunDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    const DgunBox& box = duel.target_box;
    const size_t at = static_cast<size_t>(duel.target);
    const float cx = sim.x[at] + box.ox;
    const float cy = sim.y[at] + box.oy;
    const float cz = sim.z[at] + box.oz;
    const float range2 = duel.range * duel.range;
    for (size_t i = 0; i < duel.shots_live.size();) {
        DgunShot& shot = duel.shots_live[i];
        const bool hit =
            segment_hits(shot.x, shot.y, shot.z, shot.vx, shot.vy, shot.vz, cx, cy, cz, box.hx, box.hy, box.hz);
        if (hit) {
            if (duel.die == 0) {
                duel.target_hp = std::max(
                    0.f, duel.target_hp - bar::behavior_adjust_damage(sim, duel.attacker, duel.target, duel.damage));
                ++duel.hits;
                if (!(duel.target_hp > 0.f)) {
                    duel.target_hp = 0.f;
                    duel.die = sim_frame;
                }
            }
        } else {
            const float ground = terrain_height_at(terrain, shot.x, shot.z);
            if (shot.y < ground || shot.y < 0.f) {
                ++duel.ground;
                duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        shot.vy += duel.gravity;
        const float traveled_x = shot.x - shot.ox;
        const float traveled_y = shot.y - shot.oy;
        const float traveled_z = shot.z - shot.oz;
        if (traveled_x * traveled_x + traveled_y * traveled_y + traveled_z * traveled_z > range2) {
            duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

}  // namespace

bool dgun_begin(DgunDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                const TerrainMesh& terrain, std::string& error) {
    if (duel.active) {
        error = "a dgun duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "dgun order has no unit";
        return false;
    }
    DgunDuel next;
    if (!load_dgun(game_root, sim.def_name[static_cast<size_t>(attacker)], next, error)) {
        return false;
    }
    next.gravity = terrain.gravity;
    float attacker_hp = 0;
    if (!load_health_and_volume(game_root, sim.def_name[static_cast<size_t>(attacker)], attacker_hp, next.attacker_box,
                                true, error)) {
        return false;
    }
    (void)attacker_hp;
    if (!load_health_and_volume(game_root, sim.def_name[static_cast<size_t>(target)], next.target_hp, next.target_box,
                                false, error)) {
        return false;
    }
    next.target_hp_max = next.target_hp;
    next.attacker = attacker;
    next.target = target;
    next.attacker_def = sim.def_name[static_cast<size_t>(attacker)];
    next.target_def = sim.def_name[static_cast<size_t>(target)];
    next.active = true;
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, attacker, next.attacker_box, mx, my, mz);
    aim_of(sim, target, next.target_box, ax, ay, az);
    if (sim.y[static_cast<size_t>(target)] < 0.f) {
        error = "dgun target is underwater";
        return false;
    }
    if (!in_sphere(mx, my, mz, ax, ay, az, next.range)) {
        error = "dgun target is out of range";
        return false;
    }
    next.muzzle_x = mx;
    next.muzzle_y = my;
    next.muzzle_z = mz;
    duel = std::move(next);
    return true;
}

void dgun_step(DgunDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    if (!duel.active || duel.die != 0) {
        return;
    }
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, duel.attacker, duel.attacker_box, mx, my, mz);
    aim_of(sim, duel.target, duel.target_box, ax, ay, az);
    const bool ready = in_sphere(mx, my, mz, ax, ay, az, duel.range) && sim.y[static_cast<size_t>(duel.target)] >= 0.f;
    if (duel.salvo_left == 0 && duel.next_salvo <= sim_frame && sim_frame >= duel.reload_status && ready) {
        duel.reload_status = sim_frame + duel.reload_frames;
        duel.salvo_left = duel.burst;
        duel.next_salvo = sim_frame;
    }
    if (duel.salvo_left > 0 && duel.next_salvo <= sim_frame) {
        --duel.salvo_left;
        duel.next_salvo = sim_frame + duel.salvo_delay;
        fire_shot(duel, sim);
    }
    step_shots(duel, sim, terrain, sim_frame);
}

DgunTrace dgun_trace(const DgunDuel& duel) {
    DgunTrace trace;
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
    trace.range = duel.range;
    trace.reload_frames = duel.reload_frames;
    trace.speed = duel.speed;
    trace.gravity = duel.gravity;
    return trace;
}

}  // namespace bar
