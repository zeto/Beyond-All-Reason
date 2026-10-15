#include "weapon/beam.h"

#include "gadget_host.h"
#include "move/unit_sim.h"

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

bool read_box(lua_State* state, const std::string& def_name, BeamBox& box, std::string& error) {
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

bool load_health_and_box(const std::string& game_root, const std::string& def_name, float& health, BeamBox& box,
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

void muzzle_of(const UnitSim& sim, int index, const BeamBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy + box.hy;
    z = sim.z[at] + box.oz;
}

void aim_of(const UnitSim& sim, int index, const BeamBox& box, float& x, float& y, float& z) {
    const size_t at = static_cast<size_t>(index);
    x = sim.x[at] + box.ox;
    y = sim.y[at] + box.oy;
    z = sim.z[at] + box.oz;
}

bool beam_slab(float origin, float direction, float min_bound, float max_bound, float& tmin, float& tmax) {
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

// Distance along the normalized aim ray to the first entry of the target box.
bool ray_entry(float mx, float my, float mz, float ax, float ay, float az, float cx, float cy, float cz, float hx,
               float hy, float hz, float& length) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.f)) {
        return false;
    }
    const float dir_x = dx / dist;
    const float dir_y = dy / dist;
    const float dir_z = dz / dist;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!beam_slab(mx, dir_x, cx - hx, cx + hx, tmin, tmax) ||
        !beam_slab(my, dir_y, cy - hy, cy + hy, tmin, tmax) ||
        !beam_slab(mz, dir_z, cz - hz, cz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    length = tmin < 0.f ? 0.f : tmin;
    return true;
}

bool load_beam(const std::string& game_root, const std::string& def_name, BeamDuel& duel, std::string& error) {
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
            if (std::string(type) != "BeamLaser") {
                error = def_name + " weapon 1 is not a BeamLaser";
                lua_close(state);
                return false;
            }
            bool present = false;
            const float file_range = lua_number(state, "range", 10.f, present);
            const float reload = lua_number(state, "reloadtime", 1.f, present);
            const float beamtime = lua_number(state, "beamtime", 1.f, present);
            const float accuracy = lua_number(state, "accuracy", 0.f, present);
            const float spray = lua_number(state, "sprayangle", 0.f, present);
            const float energy = lua_number(state, "energypershot", 0.f, present);
            float min_intensity = lua_number(state, "minintensity", 0.f, present);
            if (!present) {
                min_intensity = lua_number(state, "minIntensity", 0.f, present);
            }
            float keyed = 0.f;
            if ((lua_has_number(state, "heightmod", keyed) && keyed != 1.f) ||
                (lua_has_number(state, "heightMod", keyed) && keyed != 1.f)) {
                error = def_name + " beam heightmod is not 1";
                lua_close(state);
                return false;
            }
            if ((lua_has_number(state, "cylindertargeting", keyed) && keyed >= 0.01f) ||
                (lua_has_number(state, "cylinderTargeting", keyed) && keyed >= 0.01f)) {
                error = def_name + " beam uses cylinder targeting";
                lua_close(state);
                return false;
            }
            if ((lua_has_number(state, "targetborder", keyed) && keyed != 0.f) ||
                (lua_has_number(state, "targetBorder", keyed) && keyed != 0.f)) {
                error = def_name + " beam uses a target border";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "beamburst", false)) {
                error = def_name + " beam burst is not part of this duel";
                lua_close(state);
                return false;
            }
            if (lua_flag(state, "sweepfire", false) || lua_flag(state, "sweepFire", false)) {
                error = def_name + " beam sweep is not part of this duel";
                lua_close(state);
                return false;
            }
            if (!lua_flag(state, "impactonly", false)) {
                error = def_name + " beam is not impact only";
                lua_close(state);
                return false;
            }
            if (accuracy != 0.f || spray != 0.f || energy != 0.f) {
                error = def_name + " beam is not a single accurate free shot";
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
            if (!(damage > 0.f) || !(file_range > 0.f) || !(reload > 0.f) || !(beamtime > 0.f)) {
                error = def_name + " beam is missing range, time, or damage";
                lua_close(state);
                return false;
            }
            duel.damage = damage;
            duel.range = file_range;
            duel.beamtime = beamtime;
            duel.min_intensity = min_intensity;
            duel.reload_frames = std::max(1, static_cast<int>(reload * kSimHz));
            duel.salvo_size = std::max(1, static_cast<int>(beamtime * kSimHz));
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

void fire_tick(BeamDuel& duel, const UnitSim& sim, int sim_frame) {
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
    const BeamBox& box = duel.target_box;
    const size_t at = static_cast<size_t>(duel.target);
    const float cx = sim.x[at] + box.ox;
    const float cy = sim.y[at] + box.oy;
    const float cz = sim.z[at] + box.oz;
    float length = 0;
    if (!ray_entry(mx, my, mz, ax, ay, az, cx, cy, cz, box.hx, box.hy, box.hz, length) || !(length < duel.range)) {
        ++duel.ground;
        return;
    }
    const float intensity = std::max(duel.min_intensity, 1.f - length / (duel.range * 2.f));
    const float scale = 1.f / static_cast<float>(duel.salvo_size);
    duel.intensity = intensity;
    const float dealt = duel.damage * (intensity * scale);
    duel.target_hp =
        std::max(0.f, duel.target_hp - bar::behavior_adjust_damage(sim, duel.attacker, duel.target, dealt));
    ++duel.hits;
    if (!(duel.target_hp > 0.f)) {
        duel.target_hp = 0.f;
        duel.die = sim_frame;
    }
}

}  // namespace

bool beam_begin(BeamDuel& duel, const std::string& game_root, int attacker, int target, const UnitSim& sim,
                std::string& error) {
    if (duel.active) {
        error = "a beam duel is already active";
        return false;
    }
    if (attacker < 0 || target < 0 || attacker >= static_cast<int>(sim.x.size()) ||
        target >= static_cast<int>(sim.x.size()) || attacker == target) {
        error = "beam order has no unit";
        return false;
    }
    BeamDuel next;
    if (!load_beam(game_root, sim.def_name[static_cast<size_t>(attacker)], next, error)) {
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
    if (!in_sphere(mx, my, mz, ax, ay, az, next.range)) {
        error = "beam target is out of range";
        return false;
    }
    next.muzzle_x = mx;
    next.muzzle_y = my;
    next.muzzle_z = mz;
    duel = std::move(next);
    return true;
}

void beam_step(BeamDuel& duel, const UnitSim& sim, int sim_frame) {
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
    const bool ready = in_sphere(mx, my, mz, ax, ay, az, duel.range);
    // The reload stamp happens when the salvo starts. Delay is 0, so the first tick is this frame.
    if (duel.salvo_left == 0 && duel.next_salvo <= sim_frame && sim_frame >= duel.reload_status && ready) {
        duel.reload_status = sim_frame + duel.reload_frames;
        duel.salvo_left = duel.salvo_size;
        duel.next_salvo = sim_frame;
    }
    if (duel.salvo_left > 0 && duel.next_salvo <= sim_frame) {
        --duel.salvo_left;
        duel.next_salvo = sim_frame;
        fire_tick(duel, sim, sim_frame);
    }
}

BeamTrace beam_trace(const BeamDuel& duel) {
    BeamTrace trace;
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
    trace.beamtime = duel.beamtime;
    trace.salvo_size = duel.salvo_size;
    trace.intensity = duel.intensity;
    return trace;
}

}  // namespace bar
