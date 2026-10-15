#pragma once

#include <string>

namespace bar {

// Executes modinfo.lua and reads the returned `name` field.
bool load_game_name(const std::string& game_root, std::string& name, std::string& error);

}  // namespace bar
