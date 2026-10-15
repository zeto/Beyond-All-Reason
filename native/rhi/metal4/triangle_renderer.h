#pragma once

#include <string>

namespace bar {

struct FrameRequest {
    void* metal_layer = nullptr;
    const char* screenshot_ppm = nullptr;
};

// Metal 4 types stay inside the .mm file. The shell only sees this class.
class TriangleRenderer {
public:
    TriangleRenderer();
    ~TriangleRenderer();

    TriangleRenderer(const TriangleRenderer&) = delete;
    TriangleRenderer& operator=(const TriangleRenderer&) = delete;

    bool init(const std::string& shader_path, std::string& error);
    bool draw(const FrameRequest& request, std::string& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace bar
