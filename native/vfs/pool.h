#pragma once

#include <string>
#include <utility>
#include <vector>

namespace bar {

// Reads one installed game package index and the gzip blobs in the player's pool.
class GameContent {
public:
    bool open(const std::string& data_dir, std::string& error);
    bool read(const std::string& inner_path, std::vector<uint8_t>& out, std::string& error) const;

private:
    std::string pool_dir_;
    std::vector<std::pair<std::string, std::string>> index_;
};

}  // namespace bar
