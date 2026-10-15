#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

// A directory, or an .sd7 7-Zip archive read through the 7z tool.
class Archive {
public:
    static bool read_file(const std::string& archive_path, const std::string& inner_path,
                          std::vector<uint8_t>& out, std::string& error);
};

// "Supreme Isthmus v2.1" -> maps/supreme_isthmus_v2.1.sd7 inside map_dir.
bool find_map_archive(const std::string& map_dir, const std::string& map_name, std::string& archive_path,
                      std::string& error);

}  // namespace bar
