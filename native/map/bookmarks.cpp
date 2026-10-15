#include "bookmarks.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

namespace bar {
namespace {

std::string read_text(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not read bookmarks " + path;
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* skip(const char* cursor, const char* end) {
    while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    return cursor;
}

bool find_key(const std::string& text, const char* key, std::size_t& value_at) {
    const std::string pattern = std::string("\"") + key + "\"";
    std::size_t at = 0;
    while ((at = text.find(pattern, at)) != std::string::npos) {
        const char* cursor = skip(text.data() + at + pattern.size(), text.data() + text.size());
        if (cursor < text.data() + text.size() && *cursor == ':') {
            value_at = static_cast<std::size_t>(cursor - text.data() + 1);
            return true;
        }
        at += pattern.size();
    }
    return false;
}

bool json_string(const std::string& text, const char* key, std::string& out) {
    std::size_t value_at = 0;
    if (!find_key(text, key, value_at)) {
        return false;
    }
    const char* cursor = skip(text.data() + value_at, text.data() + text.size());
    if (cursor >= text.data() + text.size() || *cursor != '"') {
        return false;
    }
    ++cursor;
    const char* end = cursor;
    while (end < text.data() + text.size() && *end != '"') {
        ++end;
    }
    if (end >= text.data() + text.size()) {
        return false;
    }
    out.assign(cursor, end);
    return true;
}

std::optional<float> json_number(const std::string& text, const char* key) {
    std::size_t value_at = 0;
    if (!find_key(text, key, value_at)) {
        return std::nullopt;
    }
    const char* cursor = skip(text.data() + value_at, text.data() + text.size());
    char* parsed = nullptr;
    const float value = std::strtof(cursor, &parsed);
    if (parsed == cursor) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

bool load_bookmarks(const std::string& path, const std::string& expect_map, std::vector<Bookmark>& out,
                    std::string& error) {
    const std::string text = read_text(path, error);
    if (text.empty()) {
        if (error.empty()) {
            error = "bookmarks file is empty: " + path;
        }
        return false;
    }
    std::string map_name;
    if (!json_string(text, "map", map_name)) {
        error = "bookmarks have no map name: " + path;
        return false;
    }
    if (map_name != expect_map) {
        error = "bookmarks are for \"" + map_name + "\" but the start script asks for \"" + expect_map + "\"";
        return false;
    }

    const auto shots_key = text.find("\"shots\"");
    if (shots_key == std::string::npos) {
        error = "bookmarks have no shots: " + path;
        return false;
    }
    std::size_t cursor = text.find('[', shots_key);
    if (cursor == std::string::npos) {
        error = "bookmarks shots are not a list: " + path;
        return false;
    }
    while (true) {
        const auto open = text.find('{', cursor);
        const auto close_list = text.find(']', cursor);
        if (open == std::string::npos || (close_list != std::string::npos && close_list < open)) {
            break;
        }
        const auto close = text.find('}', open);
        if (close == std::string::npos) {
            error = "bookmarks shot is missing a closing brace";
            return false;
        }
        const std::string shot = text.substr(open, close - open + 1);
        Bookmark bookmark;
        if (!json_string(shot, "id", bookmark.id)) {
            error = "a bookmark is missing id";
            return false;
        }
        const auto x = json_number(shot, "x");
        const auto z = json_number(shot, "z");
        const auto distance = json_number(shot, "distance");
        const auto tilt = json_number(shot, "tilt");
        const auto yaw = json_number(shot, "yaw");
        if (!x || !z || !distance || !tilt || !yaw) {
            error = "bookmark " + bookmark.id + " is missing x, z, distance, tilt, or yaw";
            return false;
        }
        bookmark.camera.target_x = *x;
        bookmark.camera.target_z = *z;
        if (const auto y = json_number(shot, "y")) {
            bookmark.camera.target_y = *y;
        }
        bookmark.camera.distance = *distance;
        bookmark.camera.tilt = *tilt;
        bookmark.camera.yaw = *yaw;
        bookmark.camera.view_width = 1280;
        bookmark.camera.view_height = 720;
        out.push_back(bookmark);
        cursor = close + 1;
    }
    if (out.empty()) {
        error = "bookmarks list is empty: " + path;
        return false;
    }
    return true;
}

}  // namespace bar
