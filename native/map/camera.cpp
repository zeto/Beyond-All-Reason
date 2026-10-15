#include "camera.h"

#include <algorithm>
#include <cmath>

namespace bar {
namespace {

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

void normalize(float& x, float& y, float& z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length > 1e-8f) {
        x /= length;
        y /= length;
        z /= length;
    }
}

float dot(float ax, float ay, float az, float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

void mul_mat(const float* a, const float* b, float* out) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

}  // namespace

void camera_apply_input(Camera& camera, const CameraInput& input) {
    camera.view_width = std::max(1, input.view_width);
    camera.view_height = std::max(1, input.view_height);

    if (input.wheel != 0) {
        camera.distance *= std::exp(-input.wheel * 0.12f);
        camera.distance = clampf(camera.distance, 180.f, 28000.f);
    }
    if (input.right) {
        camera.yaw -= input.delta_x * 0.005f;
        camera.tilt += input.delta_y * 0.004f;
        camera.tilt = clampf(camera.tilt, 0.08f, 1.35f);
    }

    const float tilt = camera.tilt;
    const float yaw = camera.yaw;
    const float forward_x = std::sin(yaw);
    const float forward_z = std::cos(yaw);
    // right = forward × world up = (-forward_z, forward_x). Facing south, this is west.
    const float right_x = -forward_z;
    const float right_z = forward_x;

    float move_x = 0;
    float move_z = 0;
    if (input.middle) {
        const float span = 2.f * camera.distance * std::tan(camera.fovy_deg * 0.5f * 3.14159265f / 180.f);
        const float world_per_pixel = span / static_cast<float>(camera.view_height);
        move_x += (-input.delta_x * right_x - input.delta_y * forward_x) * world_per_pixel;
        move_z += (-input.delta_x * right_z - input.delta_y * forward_z) * world_per_pixel;
    }

    const float margin = 14.f;
    const float speed = camera.distance * 0.85f * std::max(input.dt, 0.f);
    if (input.mouse_x < margin) {
        move_x -= right_x * speed;
        move_z -= right_z * speed;
    } else if (input.mouse_x > camera.view_width - margin) {
        move_x += right_x * speed;
        move_z += right_z * speed;
    }
    if (input.mouse_y < margin) {
        move_x += forward_x * speed;
        move_z += forward_z * speed;
    } else if (input.mouse_y > camera.view_height - margin) {
        move_x -= forward_x * speed;
        move_z -= forward_z * speed;
    }

    camera.target_x += move_x;
    camera.target_z += move_z;
    (void)tilt;
}

CameraPose camera_pose(const Camera& camera) {
    CameraPose pose{};
    const float tilt = camera.tilt;
    const float yaw = camera.yaw;
    const float horizontal = std::sin(tilt) * camera.distance;
    pose.eye[0] = camera.target_x - std::sin(yaw) * horizontal;
    pose.eye[1] = std::cos(tilt) * camera.distance + camera.eye_lift;
    pose.eye[2] = camera.target_z - std::cos(yaw) * horizontal;
    pose.target[0] = camera.target_x;
    pose.target[1] = camera.target_y;
    pose.target[2] = camera.target_z;

    float fx = pose.target[0] - pose.eye[0];
    float fy = pose.target[1] - pose.eye[1];
    float fz = pose.target[2] - pose.eye[2];
    normalize(fx, fy, fz);
    // right = look × world up = (-fz, 0, fx)
    float sx = -fz;
    float sy = 0;
    float sz = fx;
    normalize(sx, sy, sz);
    const float ux = sy * fz - sz * fy;
    const float uy = sz * fx - sx * fz;
    const float uz = sx * fy - sy * fx;
    pose.right[0] = sx;
    pose.right[1] = sy;
    pose.right[2] = sz;
    pose.up[0] = ux;
    pose.up[1] = uy;
    pose.up[2] = uz;
    pose.forward[0] = fx;
    pose.forward[1] = fy;
    pose.forward[2] = fz;

    float view[16] = {};
    view[0] = sx;
    view[1] = ux;
    view[2] = -fx;
    view[4] = sy;
    view[5] = uy;
    view[6] = -fy;
    view[8] = sz;
    view[9] = uz;
    view[10] = -fz;
    view[12] = -dot(sx, sy, sz, pose.eye[0], pose.eye[1], pose.eye[2]);
    view[13] = -dot(ux, uy, uz, pose.eye[0], pose.eye[1], pose.eye[2]);
    view[14] = dot(fx, fy, fz, pose.eye[0], pose.eye[1], pose.eye[2]);
    view[15] = 1;

    const float aspect = static_cast<float>(std::max(camera.view_width, 1)) / static_cast<float>(std::max(camera.view_height, 1));
    const float f = 1.f / std::tan(camera.fovy_deg * 0.5f * 3.14159265f / 180.f);
    const float near_z = 4.f;
    const float far_z = 80000.f;
    float proj[16] = {};
    proj[0] = f / aspect;
    proj[5] = f;
    proj[10] = far_z / (near_z - far_z);
    proj[11] = -1.f;
    proj[14] = (near_z * far_z) / (near_z - far_z);

    mul_mat(proj, view, pose.view_proj);
    return pose;
}

void camera_pointer_ray(const Camera& camera, const CameraPose& pose, float mouse_x, float mouse_y, float origin[3],
                        float direction[3]) {
    const float width = static_cast<float>(std::max(camera.view_width, 1));
    const float height = static_cast<float>(std::max(camera.view_height, 1));
    const float ndc_x = (mouse_x / width) * 2.f - 1.f;
    const float ndc_y = 1.f - (mouse_y / height) * 2.f;
    const float aspect = width / height;
    const float f = 1.f / std::tan(camera.fovy_deg * 0.5f * 3.14159265f / 180.f);
    const float cx = ndc_x * aspect / f;
    const float cy = ndc_y / f;
    origin[0] = pose.eye[0];
    origin[1] = pose.eye[1];
    origin[2] = pose.eye[2];
    direction[0] = pose.right[0] * cx + pose.up[0] * cy + pose.forward[0];
    direction[1] = pose.right[1] * cx + pose.up[1] * cy + pose.forward[1];
    direction[2] = pose.right[2] * cx + pose.up[2] * cy + pose.forward[2];
}

namespace {

bool project(const float* view_proj, float x, float y, float z, float& ndc_x, float& ndc_y) {
    const float cx = view_proj[0] * x + view_proj[4] * y + view_proj[8] * z + view_proj[12];
    const float cy = view_proj[1] * x + view_proj[5] * y + view_proj[9] * z + view_proj[13];
    const float cw = view_proj[3] * x + view_proj[7] * y + view_proj[11] * z + view_proj[15];
    if (!(cw > 0.01f)) {
        return false;
    }
    ndc_x = cx / cw;
    ndc_y = cy / cw;
    return true;
}

}  // namespace

bool camera_frame_ok(std::string& error) {
    Camera camera;
    camera.target_x = 0;
    camera.target_z = 0;
    camera.distance = 1000;
    camera.yaw = 0;
    camera.tilt = 1.f;
    camera.view_width = 1280;
    camera.view_height = 720;
    const CameraPose pose = camera_pose(camera);
    if (!(pose.eye[2] < -100.f) || std::fabs(pose.eye[0]) > 1.f) {
        error = "yaw 0 should place the eye north of the target";
        return false;
    }

    float target_x = 0;
    float target_y = 0;
    float east_x = 0;
    float east_y = 0;
    float south_x = 0;
    float south_y = 0;
    if (!project(pose.view_proj, 0, 0, 0, target_x, target_y) ||
        !project(pose.view_proj, 200, 0, 0, east_x, east_y) ||
        !project(pose.view_proj, 0, 0, 400, south_x, south_y)) {
        error = "camera test points fell behind the near plane";
        return false;
    }
    if (std::fabs(target_x) > 0.02f || std::fabs(target_y) > 0.02f) {
        error = "look-at target is not at the center of the view";
        return false;
    }
    // Facing south, east is screen-left.
    if (!(east_x < -0.05f)) {
        error = "east did not land on screen left while looking south";
        return false;
    }
    if (!(south_y > target_y + 0.02f)) {
        error = "a point further south did not land above the target";
        return false;
    }

    Camera pan = camera;
    pan.target_x = 1000;
    pan.target_z = 1000;
    CameraInput input;
    input.middle = true;
    input.delta_x = 30;
    input.mouse_x = 640;
    input.mouse_y = 360;
    input.view_width = 1280;
    input.view_height = 720;
    camera_apply_input(pan, input);
    if (!(pan.target_x > 1000.f)) {
        error = "middle-drag right did not grab the ground toward the east";
        return false;
    }
    return true;
}

}  // namespace bar
