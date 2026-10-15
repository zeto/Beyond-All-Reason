#pragma once

#include "camera.h"

#include <string>
#include <vector>

namespace bar {

struct Bookmark {
    std::string id;
    Camera camera;
};

bool load_bookmarks(const std::string& path, const std::string& expect_map, std::vector<Bookmark>& out,
                    std::string& error);

}  // namespace bar
