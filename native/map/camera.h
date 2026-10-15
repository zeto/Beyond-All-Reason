#pragma once

#include <string>

namespace bar {

struct Camera {
    float target_x = 0;
    float target_y = 0;
    float target_z = 0;
    float distance = 4000;
    float yaw = 0.4f;
    float tilt = 0.7f;
    // Added to the eye height. Zero keeps the ground-unit lock camera unchanged.
    float eye_lift = 0;
    float fovy_deg = 45;
    int view_width = 1280;
    int view_height = 720;
};

struct CameraInput {
    float mouse_x = 0;
    float mouse_y = 0;
    float delta_x = 0;
    float delta_y = 0;
    float wheel = 0;
    float dt = 0;
    bool middle = false;
    bool right = false;
    int view_width = 1280;
    int view_height = 720;
};

struct CameraPose {
    float eye[3];
    float target[3];
    float right[3];
    float up[3];
    float forward[3];
    float view_proj[16];
};

void camera_pointer_ray(const Camera& camera, const CameraPose& pose, float mouse_x, float mouse_y, float origin[3],
                        float direction[3]);

void camera_apply_input(Camera& camera, const CameraInput& input);
CameraPose camera_pose(const Camera& camera);

// Pins the yaw-0 frame: the eye sits north of the target, east lands on screen left,
// and a middle-drag to the right grabs the ground toward the east.
bool camera_frame_ok(std::string& error);

// Returns false when the point is behind the camera. Screen y grows up from the bottom.
bool world_to_screen(const Camera& camera, const CameraPose& pose, float x, float y, float z, float& screen_x,
                     float& screen_y);

}  // namespace bar
