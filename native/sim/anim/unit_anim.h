#pragma once

#include "s3o.h"

#include <string>
#include <vector>

namespace bar {

// One commander. The Lua 5.1 host runs that unit's LUS script. Piece matrices are column-major
// and already include the parent chain. A hidden piece has matrix element [15] set to 0.
struct UnitAnim {
    bool ready = false;
    int frame = 0;
    int piece_count = 0;
    std::vector<float> matrices;
    struct Impl;
    Impl* impl = nullptr;

    UnitAnim();
    ~UnitAnim();
    UnitAnim(const UnitAnim&) = delete;
    UnitAnim& operator=(const UnitAnim&) = delete;
};

// Pins the piece matrix: +X rotation sends local +y to +z, and +Y rotation sends local +z to +x.
bool anim_convention_ok(std::string& error);

// Loads scripts/Units/<def>_lus.lua from the game tree and builds the bind-pose matrices.
bool anim_open(UnitAnim& anim, const ModelMesh& model, const std::string& script_path, float def_speed_per_sec,
               std::string& error);

// Bind-pose piece matrices and no script. Used for units this slice does not host.
bool anim_bind_pose(UnitAnim& anim, const ModelMesh& model, std::string& error);

// Version-4 cob. Create, StartMoving, and StopMoving run. Other defs stay on the bind pose.
bool anim_open_cob(UnitAnim& anim, const ModelMesh& model, const std::string& script_path, float def_speed_per_sec,
                   std::string& error);

bool anim_is_cob(const UnitAnim& anim);

// Runs script.Create. Turns issued there have not moved yet; the bind pose is still current.
bool anim_start(UnitAnim& anim, std::string& error);

void anim_set_speed(UnitAnim& anim, float elmos_per_frame);

// Heading in radians and the terrain height under the unit. Cob queries read these. Lua ignores them.
void anim_set_query(UnitAnim& anim, float heading, float ground_y);

// Calls StartMoving or StopMoving on the false/true edge. Spring uses speed, not the order itself.
void anim_set_moving(UnitAnim& anim, bool moving);

// Wakes scripts whose Sleep has elapsed, then integrates turns, moves, and spins by 1/30 s.
void anim_step(UnitAnim& anim);

// One json line: frame plus lthigh, rthigh, and dish angles in radians.
bool anim_write_sample(const UnitAnim& anim, const std::string& path, int frame, bool append, std::string& error);

// One json line: frame, both wheel X angles, how many times the left wheel spin was integrated, and
// whether smoke1 is hidden.
bool anim_write_cob_sample(const UnitAnim& anim, const std::string& path, int frame, bool append, std::string& error);

}  // namespace bar
