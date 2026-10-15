#include "weapon/bomb.h"

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
constexpr int kBombTtl = 1000;
constexpr float kNormEps = 1.0e-12f;

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

bool read_box(lua_State* state, const std::string& def_name, BombBox& box, std::string& error) {
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

bool load_health_and_box(const std::string& game_root, const std::string& def_name, float& health, BombBox& box,
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

void owner_velocity(const UnitSim& sim, int index, float& vx, float& vy, float& vz) {
    const size_t at = static_cast<size_t>(index);
    vx = std::sin(sim.heading[at]) * sim.speed[at];
    vy = at < sim.vy.size() ? sim.vy[at] : 0.f;
    vz = std::cos(sim.heading[at]) * sim.speed[at];
}

void safe_normalize(float& x, float& y, float& z) {
    const float sq = x * x + y * y + z * z;
    if (!(sq > kNormEps)) {
        return;
    }
    const float inv = 1.f / std::sqrt(sq);
    x *= inv;
    y *= inv;
    z *= inv;
}

float predicted_fall(float muzzle_y, float impact_y, float vy, float gravity) {
    if (muzzle_y <= impact_y || gravity == 0.f) {
        return 0.f;
    }
    const float d = impact_y - muzzle_y;
    const float tt = vy * vy + 2.f * d * gravity;
    if (!(tt >= 0.f)) {
        return 0.f;
    }
    return (-vy - std::sqrt(tt)) / gravity;
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

void build_heights(BombDuel& duel, const TerrainMesh& terrain, float x, float z) {
    const int stride = std::max(terrain.mesh_stride, 1);
    const int square = std::max(terrain.square_size, 1);
    const float step = static_cast<float>(square * stride);
    const float aligned = std::floor(z / step) * step;
    const int count = static_cast<int>(std::ceil(9000.f / step)) + 2;
    duel.height_step = step;
    duel.height_z0 = aligned;
    duel.heights.clear();
    duel.heights.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        duel.heights.push_back(terrain_height_at(terrain, x, aligned + step * static_cast<float>(i)));
    }
}

bool load_bomb(const std::string& game_root, const std::string& def_name, const UnitSim& sim, int attacker,
               BombDuel& duel, std::string& error) {
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
    const size_t at = static_cast<size_t>(attacker);
    duel.speed = sim.max_speed[at];
    duel.accel = sim.max_acc[at];
    duel.cruise = sim.cruise[at];
    duel.pitch = sim.max_pitch[at];
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
            if (std::string(type) != "AircraftBomb") {
                error = def_name + " weapon 1 is not an AircraftBomb";
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
            float arc = 0.f;
            lua_has_number(state, "trajectoryheight", arc);
            float my_gravity = 0.f;
            lua_has_number(state, "mygravity", my_gravity);
            if (accuracy != 0.f || spray != 0.f) {
                error = def_name + " bomb sprays";
                lua_close(state);
                return false;
            }
            if (metal != 0.f) {
                error = def_name + " bomb spends metal";
                lua_close(state);
                return false;
            }
            if (energy != 0.f) {
                error = def_name + " bomb spends energy";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "waterweapon", false)) {
                error = def_name + " bomb is a water weapon";
                lua_close(state);
                return false;
            }
            if (arc != 0.f) {
                error = def_name + " bomb arcs";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "groundbounce", false) || lua_flag(state, "waterbounce", false)) {
                error = def_name + " bomb bounces";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "noexplode", false)) {
                error = def_name + " bomb does not expire";
                lua_close(state);
                return false;
            }
            const int burst = static_cast<int>(burst_f);
            if (burst < 1 || static_cast<float>(burst) != burst_f) {
                error = def_name + " bomb burst is partial";
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
            if (!(damage > 0.f) || !(file_range > 0.f) || !(reload > 0.f)) {
                error = def_name + " bomb is missing range or damage";
                lua_close(state);
                return false;
            }
            duel.damage = damage;
            duel.range = file_range;
            duel.burst = burst;
            duel.salvo_delay = static_cast<int>(rate * kSimHz);
            duel.reload_frames = std::max(1, static_cast<int>(reload * kSimHz));
            duel.ttl = kBombTtl;
            duel.waterweapon = false;
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
        error = def_name + " bomb burst delay is negative";
        return false;
    }
    return true;
}

bool drop_window(const BombDuel& duel, float mx, float my, float mz, float vx, float vy, float vz, float ax, float ay,
                 float az) {
    if (my < ay) {
        return false;
    }
    const float fall = predicted_fall(my, ay, vy, duel.gravity);
    const float speed2d = std::sqrt(vx * vx + vz * vz);
    const float drop = static_cast<float>(std::max(1, duel.burst - 1) * duel.salvo_delay) * speed2d * 0.5f;
    const float px = mx + vx * fall;
    const float pz = mz + vz * fall;
    const float dx = ax - px;
    const float dz = az - pz;
    return dx * dx + dz * dz < drop * drop;
}

void fire_shot(BombDuel& duel, const UnitSim& sim, float mx, float my, float mz, float ax, float ay, float az) {
    float vx = 0;
    float vy = 0;
    float vz = 0;
    owner_velocity(sim, duel.attacker, vx, vy, vz);
    duel.muzzle_x = mx;
    duel.muzzle_y = my;
    duel.muzzle_z = mz;
    ++duel.shots;
    float dif_x = ax - mx;
    float dif_y = 0.f;
    float dif_z = az - mz;
    float dir_x = vx;
    float dir_y = vy;
    float dir_z = vz;
    safe_normalize(dir_x, dir_y, dir_z);
    dir_y = std::min(0.f, dir_y);
    safe_normalize(dir_x, dir_y, dir_z);
    const float dot = dif_x * dir_x + dif_y * dir_y + dif_z * dir_z;
    dif_x -= dir_x * dot;
    dif_y -= dir_y * dot;
    dif_z -= dir_z * dot;
    const float predict = predicted_fall(my, ay, vy, duel.gravity);
    const float scale = std::max(0.01f, predict);
    dif_x /= scale;
    dif_y /= scale;
    dif_z /= scale;
    const float sq = dif_x * dif_x + dif_y * dif_y + dif_z * dif_z;
    if (sq > 1.f) {
        const float inv = 1.f / std::sqrt(sq);
        dif_x *= inv;
        dif_y *= inv;
        dif_z *= inv;
    }
    BombShot shot;
    shot.x = mx;
    shot.y = my;
    shot.z = mz;
    shot.vx = vx + dif_x;
    shot.vy = vy + dif_y;
    shot.vz = vz + dif_z;
    shot.ttl = kBombTtl;
    duel.ttl = shot.ttl;
    duel.shots_live.push_back(shot);
}

void step_shots(BombDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    const BombBox& box = duel.target_box;
    const size_t at = static_cast<size_t>(duel.target);
    const float cx = sim.x[at] + box.ox;
    const float cy = sim.y[at] + box.oy;
    const float cz = sim.z[at] + box.oz;
    for (size_t i = 0; i < duel.shots_live.size();) {
        BombShot& shot = duel.shots_live[i];
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
        if (shot.y < ground || (!duel.waterweapon && shot.y < 0.f)) {
            ++duel.ground;
            duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        shot.vy += duel.gravity;
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        if (--shot.ttl == 0) {
            ++duel.ground;
            duel.shots_live.erase(duel.shots_live.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

}  // namespace

bool bomb_begin(BombDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                const TerrainMesh& terrain, std::string& error) {
    if (duel.active) {
        error = "a bomb duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "bomb order has no unit";
        return false;
    }
    if (!sim.airplane[static_cast<size_t>(attacker)]) {
        error = sim.def_name[static_cast<size_t>(attacker)] + " weapon 1 is not an AircraftBomb";
        return false;
    }
    BombDuel next;
    if (!load_bomb(game_root, sim.def_name[static_cast<size_t>(attacker)], sim, attacker, next, error)) {
        return false;
    }
    if (next.gravity == 0.f) {
        next.gravity = terrain.gravity;
    }
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
    const size_t tgt = static_cast<size_t>(target);
    const float aim_y = sim.y[tgt] + next.target_box.oy;
    if (sim.y[tgt] < 0.f || aim_y < 0.f) {
        error = "bomb target is underwater";
        return false;
    }
    build_heights(next, terrain, sim.x[static_cast<size_t>(attacker)], sim.z[static_cast<size_t>(attacker)]);
    next.muzzle_x = sim.x[static_cast<size_t>(attacker)];
    next.muzzle_y = sim.y[static_cast<size_t>(attacker)];
    next.muzzle_z = sim.z[static_cast<size_t>(attacker)];
    duel = std::move(next);
    return true;
}

void bomb_step(BombDuel& duel, const UnitSim& sim, const TerrainMesh& terrain, int sim_frame) {
    if (!duel.active || duel.die != 0) {
        return;
    }
    const size_t attacker = static_cast<size_t>(duel.attacker);
    const size_t target = static_cast<size_t>(duel.target);
    const float mx = sim.x[attacker];
    const float my = sim.y[attacker];
    const float mz = sim.z[attacker];
    const float ax = sim.x[target] + duel.target_box.ox;
    const float ay = sim.y[target] + duel.target_box.oy;
    const float az = sim.z[target] + duel.target_box.oz;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    owner_velocity(sim, duel.attacker, vx, vy, vz);
    const bool ready = sim.y[target] >= 0.f && drop_window(duel, mx, my, mz, vx, vy, vz, ax, ay, az);
    if (duel.salvo_left == 0 && duel.next_salvo <= sim_frame && sim_frame >= duel.reload_status && ready) {
        duel.reload_status = sim_frame + duel.reload_frames;
        duel.salvo_left = duel.burst;
        duel.next_salvo = sim_frame;
    }
    if (duel.salvo_left > 0 && duel.next_salvo <= sim_frame) {
        --duel.salvo_left;
        duel.next_salvo = sim_frame + duel.salvo_delay;
        fire_shot(duel, sim, mx, my, mz, ax, ay, az);
    }
    step_shots(duel, sim, terrain, sim_frame);
}

BombTrace bomb_trace(const BombDuel& duel) {
    BombTrace trace;
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
    trace.burst = duel.burst;
    trace.salvo_delay = duel.salvo_delay;
    trace.speed = duel.speed;
    trace.accel = duel.accel;
    trace.cruise = duel.cruise;
    trace.pitch = duel.pitch;
    trace.ttl = duel.ttl;
    trace.gravity = duel.gravity;
    trace.waterweapon = duel.waterweapon ? 1 : 0;
    trace.height_z0 = duel.height_z0;
    trace.height_step = duel.height_step;
    trace.heights = duel.heights;
    return trace;
}

}  // namespace bar
