#include "startscript.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace bar {
namespace {

std::string trim_value(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == ';' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool value_after(const std::string& line, const char* key, std::string& value) {
    const auto at = lower_copy(line).find(lower_copy(key));
    if (at == std::string::npos) {
        return false;
    }
    const auto eq = line.find('=', at);
    if (eq == std::string::npos) {
        return false;
    }
    value = trim_value(line.substr(eq + 1));
    return !value.empty();
}

int unit_header(const std::string& line) {
    const std::string lower = lower_copy(line);
    const auto at = lower.find("[unit");
    if (at == std::string::npos || lower.find(']') == std::string::npos) {
        return -1;
    }
    const char* cursor = lower.c_str() + at + 5;
    char* end = nullptr;
    const long number = std::strtol(cursor, &end, 10);
    if (end == cursor || number < 0 || number > 31) {
        return -1;
    }
    return static_cast<int>(number);
}

int team_header(const std::string& line) {
    const std::string lower = lower_copy(line);
    const auto at = lower.find("[team");
    if (at == std::string::npos || lower.find(']') == std::string::npos) {
        return -1;
    }
    const char* cursor = lower.c_str() + at + 5;
    char* end = nullptr;
    const long number = std::strtol(cursor, &end, 10);
    if (end == cursor || number < 0 || number > 15) {
        return -1;
    }
    return static_cast<int>(number);
}

bool other_section(const std::string& line) {
    return line.find('[') != std::string::npos && line.find(']') != std::string::npos && team_header(line) < 0;
}

std::string trim_copy(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

// The key is the whole token before '='. startmetal must not match startmetalstorage.
bool assignment(const std::string& line, std::string& key, std::string& value) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    key = lower_copy(trim_copy(line.substr(0, eq)));
    value = trim_value(line.substr(eq + 1));
    return !key.empty() && !value.empty();
}

bool truthy(const std::string& value) {
    const std::string lower = lower_copy(value);
    return lower == "true" || lower == "1";
}

void store_option(ResourceOverride& options, const std::string& key, const std::string& value) {
    if (key == "startmetal") {
        options.has_start_metal = true;
        options.start_metal = std::strtof(value.c_str(), nullptr);
    } else if (key == "startenergy") {
        options.has_start_energy = true;
        options.start_energy = std::strtof(value.c_str(), nullptr);
    } else if (key == "startmetalstorage") {
        options.has_start_metal_storage = true;
        options.start_metal_storage = std::strtof(value.c_str(), nullptr);
    } else if (key == "startenergystorage") {
        options.has_start_energy_storage = true;
        options.start_energy_storage = std::strtof(value.c_str(), nullptr);
    } else if (key == "multiplier_energyproduction") {
        options.has_energy_production = true;
        options.energy_production = std::strtof(value.c_str(), nullptr);
    } else if (key == "multiplier_resourceincome") {
        options.has_resource_income = true;
        options.resource_income = std::strtof(value.c_str(), nullptr);
    } else if (key == "multiplier_metalextraction") {
        options.has_metal_extraction = true;
        options.metal_extraction = std::strtof(value.c_str(), nullptr);
    } else if (key == "proposed_unit_reworks") {
        options.has_reworks = true;
        options.reworks = truthy(value);
    } else if (key == "bonusstartresourcemultiplier") {
        options.has_bonus = true;
        options.bonus = truthy(value);
    }
}

}  // namespace

bool parse_start_script(const std::string& path, StartScript& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not read start script " + path;
        return false;
    }
    int current = -1;
    int extra_at = -1;
    std::string line;
    while (std::getline(in, line)) {
        const int placed = unit_header(line);
        if (placed >= 0) {
            if (static_cast<int>(out.extras.size()) <= placed) {
                out.extras.resize(static_cast<size_t>(placed) + 1);
            }
            extra_at = placed;
            current = -1;
            continue;
        }
        const int header = team_header(line);
        if (header >= 0) {
            if (static_cast<int>(out.teams.size()) <= header) {
                out.teams.resize(static_cast<size_t>(header) + 1);
            }
            out.teams[static_cast<size_t>(header)].team = header;
            current = header;
            extra_at = -1;
            continue;
        }
        if (other_section(line)) {
            current = -1;
            extra_at = -1;
        }
        if (extra_at >= 0) {
            std::string key;
            std::string option;
            if (!assignment(line, key, option)) {
                continue;
            }
            ScriptUnit& unit = out.extras[static_cast<size_t>(extra_at)];
            if (key == "def") {
                unit.def_name = lower_copy(option);
            } else if (key == "x") {
                unit.x = std::strtof(option.c_str(), nullptr);
                unit.has_x = true;
            } else if (key == "z") {
                unit.z = std::strtof(option.c_str(), nullptr);
                unit.has_z = true;
            } else if (key == "team") {
                unit.team = std::atoi(option.c_str());
            }
            continue;
        }
        std::string value;
        if (out.map_name.empty() && value_after(line, "MapName", value)) {
            out.map_name = value;
        }
        if (current < 0) {
            std::string key;
            std::string option;
            if (assignment(line, key, option)) {
                if (key == "startpostype") {
                    out.start_pos_type = std::atoi(option.c_str());
                } else {
                    store_option(out.options, key, option);
                }
            }
            continue;
        }
        TeamStart& team = out.teams[static_cast<size_t>(current)];
        if (value_after(line, "AllyTeam", value)) {
            team.ally_team = std::atoi(value.c_str());
        } else if (value_after(line, "Side", value)) {
            team.side = value;
        } else if (value_after(line, "StartPosX", value)) {
            team.start_x = std::strtof(value.c_str(), nullptr);
            team.has_start = true;
        } else if (value_after(line, "StartPosZ", value)) {
            team.start_z = std::strtof(value.c_str(), nullptr);
            team.has_start = true;
        } else if (value_after(line, "Unit", value)) {
            team.unit_def = value;
        } else if (value_after(line, "LuaAI", value)) {
            team.lua_ai = value;
        } else if (value_after(line, "RGBColor", value)) {
            char* end = nullptr;
            const float r = std::strtof(value.c_str(), &end);
            const float g = std::strtof(end, &end);
            const float b = std::strtof(end, &end);
            if (end != value.c_str()) {
                team.color_r = r;
                team.color_g = g;
                team.color_b = b;
                team.has_color = true;
            }
        }
    }
    if (out.map_name.empty()) {
        error = "start script has no MapName: " + path;
        return false;
    }
    return true;
}

}  // namespace bar
