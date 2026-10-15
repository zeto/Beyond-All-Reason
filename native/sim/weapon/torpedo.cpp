#include "weapon/torpedo.h"

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
constexpr float kThrust = 0.2f;

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

bool read_box(lua_State* state, const std::string& def_name, TorpedoBox& box, std::string& error) {
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

bool load_health_and_box(const std::string& game_root, const std::string& def_name, float& health, TorpedoBox& box,
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

void muzzle_of(const UnitSim& sim, int index, const TorpedoBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy + box.hy;
    z = sim.z[at] + box.oz;
}

void aim_of(const UnitSim& sim, int index, const TorpedoBox& box, float& x, float& y, float& z) {
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

bool in_sphere(float mx, float my, float mz, float ax, float ay, float az, float range) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    return dist > 0.f && dist <= range;
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

int fuel_frames(float dist, float range, float projectile_speed, int flight_frames) {
    if (flight_frames > 0) {
        return flight_frames;
    }
    if (!(projectile_speed > 0.f)) {
        return 1;
    }
    const float life = std::max(dist, range) / projectile_speed + 25.f;
    const int ttl = static_cast<int>(std::ceil(life));
    return ttl < 1 ? 1 : ttl;
}

void set_speed_and_dir(TorpedoShot& shot) {
    const float mag = std::sqrt(shot.vx * shot.vx + shot.vy * shot.vy + shot.vz * shot.vz);
    shot.speed_w = mag;
    if (!(mag > 0.f)) {
        return;
    }
    shot.dx = shot.vx / mag;
    shot.dy = shot.vy / mag;
    shot.dz = shot.vz / mag;
}

bool load_torpedo(const std::string& game_root, const std::string& def_name, TorpedoDuel& duel, std::string& error) {
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
            if (std::string(type) != "TorpedoLauncher") {
                error = def_name + " weapon 1 is not a TorpedoLauncher";
                lua_close(state);
                return false;
            }
            bool present = false;
            const float file_range = lua_number(state, "range", 10.f, present);
            const float reload = lua_number(state, "reloadtime", 1.f, present);
            const float burst_f = lua_number(state, "burst", 1.f, present);
            float rate = 0.f;
            if (!lua_has_number(state, "burstrate", rate)) {
                rate = 0.f;
            }
            const float accuracy = lua_number(state, "accuracy", 0.f, present);
            const float spray = lua_number(state, "sprayangle", 0.f, present);
            const float energy = lua_number(state, "energypershot", 0.f, present);
            const float metal = lua_number(state, "metalpershot", 0.f, present);
            const float start_velocity = lua_number(state, "startvelocity", 0.f, present);
            const float max_velocity = lua_number(state, "weaponvelocity", 0.f, present);
            float flight = 0.f;
            lua_has_number(state, "flighttime", flight);
            float arc = 0.f;
            lua_has_number(state, "trajectoryheight", arc);
            float my_gravity = 0.f;
            lua_has_number(state, "mygravity", my_gravity);
            if (lua_flag(state, "tracks", false)) {
                error = def_name + " torpedo tracks";
                lua_close(state);
                return false;
            }
            if (accuracy != 0.f || spray != 0.f) {
                error = def_name + " torpedo sprays";
                lua_close(state);
                return false;
            }
            if (metal != 0.f) {
                error = def_name + " torpedo spends metal";
                lua_close(state);
                return false;
            }
            if (energy != 0.f) {
                error = def_name + " torpedo spends energy";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "submissile", false)) {
                error = def_name + " torpedo is a submissile";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "fixedlauncher", false)) {
                error = def_name + " torpedo launcher is fixed";
                lua_close(state);
                return false;
            }
            if (arc != 0.f) {
                error = def_name + " torpedo arcs";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "groundbounce", false) || lua_flag(state, "waterbounce", false)) {
                error = def_name + " torpedo bounces";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "noexplode", false)) {
                error = def_name + " torpedo does not expire";
                lua_close(state);
                return false;
            }
            if (!lua_flag(state, "waterweapon", false)) {
                error = def_name + " torpedo is not a water weapon";
                lua_close(state);
                return false;
            }
            const int burst = static_cast<int>(burst_f);
            if (burst < 1 || static_cast<float>(burst) != burst_f) {
                error = def_name + " torpedo burst is partial";
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
            if (!(damage > 0.f) || !(file_range > 0.f) || !(reload > 0.f) || !(start_velocity > 0.f) ||
                !(max_velocity > 0.f)) {
                error = def_name + " torpedo is missing range or damage";
                lua_close(state);
                return false;
            }
            duel.damage = damage;
            duel.range = file_range;
            duel.speed = start_velocity / kSimHz;
            duel.max_speed = max_velocity / kSimHz;
            duel.flight_frames = static_cast<int>(flight * kSimHz);
            duel.burst = burst;
            duel.salvo_delay = static_cast<int>(rate * kSimHz);
            duel.reload_frames = std::max(1, static_cast<int>(reload * kSimHz));
            duel.waterweapon = true;
            duel.gravity = my_gravity != 0.f ? -my_gravity : 0.f;
        }
        lua_pop(state, 1);
    }
    lua_close(state);
    if (!found) {
        error = def_name + " weapon def " + want + " is missing";
        return false;
    }
    if (duel.salvo_delay < 0) {
        error = def_name + " torpedo burst delay is negative";
        return false;
    }
    return true;
}

void fire_shot(TorpedoDuel& duel, const UnitSim& sim) {
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
    TorpedoShot shot;
    shot.dx = dx * inv;
    shot.dy = dy * inv;
    shot.dz = dz * inv;
    shot.x = mx;
    shot.y = my;
    shot.z = mz;
    shot.speed_w = duel.speed;
    shot.vx = shot.dx * shot.speed_w;
    shot.vy = shot.dy * shot.speed_w;
    shot.vz = shot.dz * shot.speed_w;
    shot.ttl = fuel_frames(dist, duel.range, duel.max_speed, duel.flight_frames);
    duel.ttl = shot.ttl;
    duel.shots_live.push_back(shot);
}

void advance_shot(TorpedoShot& shot, float gravity, float max_speed) {
    if (shot.y > 0.f) {
        shot.vy += gravity;
        set_speed_and_dir(shot);
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        return;
    }
    if (--shot.ttl > 0) {
        if (shot.speed_w < max_speed) {
            shot.speed_w += kThrust;
        }
        const float len = std::sqrt(shot.dx * shot.dx + shot.dy * shot.dy + shot.dz * shot.dz);
        if (len > 0.f) {
            shot.dx /= len;
            shot.dy /= len;
            shot.dz /= len;
        }
        shot.vx = shot.dx * shot.speed_w;
        shot.vy = shot.dy * shot.speed_w;
        shot.vz = shot.dz * shot.speed_w;
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        return;
    }
    shot.vx *= 0.98f;
    shot.vy = shot.vy * 0.98f + gravity;
    shot.vz *= 0.98f;
    set_speed_and_dir(shot);
    shot.x += shot.vx;
    shot.y += shot.vy;
    shot.z += shot.vz;
}

void step_shots(TorpedoDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    const TorpedoBox& box = duel.target_box;
    const size_t at = static_cast<size_t>(duel.target);
    const float cx = sim.x[at] + box.ox;
    const float cy = sim.y[at] + box.oy;
    const float cz = sim.z[at] + box.oz;
    for (size_t i = 0; i < duel.shots_live.size();) {
        TorpedoShot& shot = duel.shots_live[i];
        if (segment_hits(shot.x, shot.y, shot.z, shot.vx, shot.vy, shot.vz, cx, cy, cz, box.hx, box.hy, box.hz)) {
            if (duel.die == 0) {
                duel.target_hp = std::max(
                    0.f, duel.target_hp - bar::behavior_adjust_damage(sim, duel.attacker, duel.target, duel.damage));
                ++duel.hits;
                if (!(duel.target_hp > 0.f)) {
                    duel.target_hp = 0.f;
                    duel.die = sim_frame;
                }
            }
            duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const float ground = terrain_height_at(terrain, shot.x, shot.z);
        if (shot.y < ground) {
            ++duel.ground;
            duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        advance_shot(shot, duel.gravity, duel.max_speed);
        ++i;
    }
}

}  // namespace

bool torpedo_begin(TorpedoDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                   const TerrainMesh& terrain, std::string& error) {
    if (duel.active) {
        error = "a torpedo duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "torpedo order has no unit";
        return false;
    }
    TorpedoDuel next;
    if (!load_torpedo(game_root, sim.def_name[static_cast<size_t>(attacker)], next, error)) {
        return false;
    }
    if (next.gravity == 0.f) {
        next.gravity = terrain.gravity;
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
    next.reload_status = 0;
    next.next_salvo = 0;
    next.salvo_left = 0;
    float mx = 0;
    float my = 0;
    float mz = 0;
    float ax = 0;
    float ay = 0;
    float az = 0;
    muzzle_of(sim, attacker, next.attacker_box, mx, my, mz);
    aim_of(sim, target, next.target_box, ax, ay, az);
    if (!(ay <= 0.f)) {
        error = "torpedo target is not in water";
        return false;
    }
    if (!in_sphere(mx, my, mz, ax, ay, az, next.range)) {
        error = "torpedo target is out of range";
        return false;
    }
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    next.ttl = fuel_frames(dist, next.range, next.max_speed, next.flight_frames);
    next.muzzle_x = mx;
    next.muzzle_y = my;
    next.muzzle_z = mz;
    duel = std::move(next);
    return true;
}

void torpedo_step(TorpedoDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
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
    const bool ready = in_sphere(mx, my, mz, ax, ay, az, duel.range) && ay <= 0.f;
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

TorpedoTrace torpedo_trace(const TorpedoDuel& duel) {
    TorpedoTrace trace;
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
    trace.max_speed = duel.max_speed;
    trace.ttl = duel.ttl;
    trace.gravity = duel.gravity;
    trace.waterweapon = duel.waterweapon ? 1 : 0;
    return trace;
}

}  // namespace bar
