#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string oracle = "native/oracle";
    std::string actual = "native/oracle/out";
    std::string suite = "m0";
};

const char* kShots[] = {"overview", "shore", "cliff", "metal"};

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << "\n";
            return false;
        }
        const std::string value = argv[++i];
        if (arg == "--oracle") {
            options.oracle = value;
        } else if (arg == "--actual") {
            options.actual = value;
        } else if (arg == "--suite") {
            options.suite = value;
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            return false;
        }
    }
    return true;
}

bool contains_commander(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"def\":\"armcom\"") != std::string::npos ||
            line.find("\"def\":\"corcom\"") != std::string::npos ||
            line.find("\"def\":\"legcom\"") != std::string::npos) {
            return true;
        }
    }
    return false;
}

int gate_m0(const Options& options) {
    const std::string oracle_trace = options.oracle + "/traces/boot.jsonl";
    const std::string actual_trace = options.actual + "/boot.jsonl";
    const bool oracle_ok = contains_commander(oracle_trace);
    const bool actual_ok = contains_commander(actual_trace);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    if (oracle_ok && actual_ok) {
        failure << "# boot passed\n\nScenario boot has a commander on both sides.\n";
        std::cout << "boot PASS\n";
        return 0;
    }

    failure << "# boot failed\n\n";
    failure << "Scenario: boot\n\n";
    failure << "Frame: 0\n\n";
    if (!actual_ok) {
        failure << "The client trace has no commander.\n\n";
        failure << "Client trace: missing a commander in `" << actual_trace << "`.\n\n";
    } else {
        failure << "The client trace has a commander. The oracle trace does not, so boot stays red.\n\n";
    }
    if (!oracle_ok) {
        failure << "Oracle trace: missing a commander in `" << oracle_trace << "`. ";
        failure << "Recoil 2026.07.04 is pinned in `PIN`, and no spring-headless binary was available to capture the trace.\n\n";
    }
    failure << "Entity: commander\n\n";
    failure << "Field: spawn\n\n";
    failure << "Oracle value: " << (oracle_ok ? "present" : "absent") << "\n\n";
    failure << "Native value: " << (actual_ok ? "present" : "absent") << "\n\n";
    failure << "Tolerance: a boot trace must contain armcom, corcom, or legcom on both sides.\n";

    std::cout << "boot FAIL missing commander\n";
    return 1;
}

bool read_text(const std::string& path, std::string& text) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool toml_number(const std::string& text, const std::string& key, double& out) {
    const auto at = text.find(key);
    if (at == std::string::npos) {
        return false;
    }
    const auto eq = text.find('=', at + key.size());
    if (eq == std::string::npos || eq > at + key.size() + 4) {
        return false;
    }
    const char* cursor = text.c_str() + eq + 1;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (std::strncmp(cursor, "true", 4) == 0) {
        out = 1;
        return true;
    }
    if (std::strncmp(cursor, "false", 5) == 0) {
        out = 0;
        return true;
    }
    char* end = nullptr;
    out = std::strtod(cursor, &end);
    return end != cursor;
}

bool read_ppm(const std::string& path, int& width, int& height, std::vector<uint8_t>& rgb, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "missing";
        return false;
    }
    std::string magic;
    int max_value = 0;
    in >> magic >> width >> height >> max_value;
    if (!in || magic != "P6" || max_value != 255 || width < 1 || height < 1) {
        error = "not a P6 ppm";
        return false;
    }
    in.get();
    rgb.resize(static_cast<size_t>(width) * height * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (in.gcount() != static_cast<std::streamsize>(rgb.size())) {
        error = "short ppm";
        return false;
    }
    return true;
}

double luminance_stddev(const std::vector<uint8_t>& rgb) {
    const size_t pixels = rgb.size() / 3;
    double sum = 0;
    for (size_t i = 0; i < pixels; ++i) {
        sum += 0.2126 * rgb[i * 3] + 0.7152 * rgb[i * 3 + 1] + 0.0722 * rgb[i * 3 + 2];
    }
    const double mean = sum / static_cast<double>(pixels);
    double variance = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const double y = 0.2126 * rgb[i * 3] + 0.7152 * rgb[i * 3 + 1] + 0.0722 * rgb[i * 3 + 2];
        const double delta = y - mean;
        variance += delta * delta;
    }
    return std::sqrt(variance / static_cast<double>(pixels));
}

int gate_m1(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double width = 0;
    double height = 0;
    double stddev_min = 0;
    double flip_required = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "bookmark_width", width) ||
        !toml_number(tolerance_text, "bookmark_height", height) ||
        !toml_number(tolerance_text, "bookmark_stddev_min", stddev_min) ||
        !toml_number(tolerance_text, "flip_required", flip_required) || flip_required < 1) {
        std::cerr << "gate: m1 tolerances are missing or flip_required is not set\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    bool structural = true;
    failure << "# terrain bookmarks\n\n";
    failure << "Scenario: terrain_isthmus\n\n";
    for (const char* id : kShots) {
        const std::string actual = options.actual + "/m1/" + id + ".ppm";
        const std::string reference = options.oracle + "/reference/" + id + ".ppm";
        int image_w = 0;
        int image_h = 0;
        std::vector<uint8_t> rgb;
        std::string image_error;
        const bool image_ok = read_ppm(actual, image_w, image_h, rgb, image_error);
        double stddev = 0;
        if (image_ok) {
            stddev = luminance_stddev(rgb);
        }
        const bool size_ok = image_ok && image_w == static_cast<int>(width) && image_h == static_cast<int>(height);
        const bool varied = size_ok && stddev >= stddev_min;
        if (!varied) {
            structural = false;
        }
        std::ifstream reference_file(reference);
        const bool reference_ok = static_cast<bool>(reference_file);

        std::cout << id << " ";
        if (!image_ok) {
            std::cout << "FAIL " << image_error << " (" << actual << ")\n";
        } else if (!size_ok) {
            std::cout << "FAIL size " << image_w << "x" << image_h << "\n";
        } else if (!varied) {
            std::cout << "FAIL flat stddev " << stddev << "\n";
        } else {
            std::cout << "image stddev " << stddev;
            if (!reference_ok) {
                std::cout << " FAIL flip reference missing\n";
            } else {
                std::cout << " FAIL flip not scored\n";
            }
        }

        failure << "## " << id << "\n\n";
        failure << "Frame: bookmark\n\n";
        failure << "Field: picture\n\n";
        if (!image_ok) {
            failure << "Native value: " << image_error << " at `" << actual << "`.\n\n";
        } else {
            failure << "Native value: " << image_w << "x" << image_h << ", luminance stddev " << stddev << ".\n\n";
        }
        failure << "Tolerance: " << static_cast<int>(width) << "x" << static_cast<int>(height)
                << ", stddev >= " << stddev_min << ", and a FLIP score against the Recoil bookmark.\n\n";
        if (!reference_ok) {
            failure << "Oracle value: absent. `" << reference
                    << "` is not on disk, so FLIP is unmet. A structured image is not a FLIP pass.\n\n";
        } else {
            failure << "Oracle value: present at `" << reference
                    << "`, but this gate does not compute FLIP yet, so the shot stays red.\n\n";
        }
    }

    if (!structural) {
        failure << "The bookmark images are missing, the wrong size, or flat. FLIP is also unmet.\n";
        std::cout << "m1 FAIL bookmarks\n";
        return 1;
    }
    failure << "The four bookmarks vary and have the required size. ";
    failure << "They do not pass, because no FLIP score against a Recoil frame is available.\n";
    std::cout << "m1 FAIL flip\n";
    return 1;
}

struct SpawnRecord {
    bool ok = false;
    std::string def;
    int team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
};

bool json_number_field(const std::string& object, const char* key, double& out) {
    const std::string pattern = std::string("\"") + key + "\":";
    const auto at = object.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(object.c_str() + at + pattern.size(), &end);
    return end != object.c_str() + at + pattern.size();
}

bool json_string_field(const std::string& object, const char* key, std::string& out) {
    const std::string pattern = std::string("\"") + key + "\":\"";
    const auto at = object.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    const auto start = at + pattern.size();
    const auto end = object.find('"', start);
    if (end == std::string::npos) {
        return false;
    }
    out = object.substr(start, end - start);
    return true;
}

bool first_unit(const std::string& path, SpawnRecord& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto units = line.find("\"units\"");
        if (units == std::string::npos) {
            continue;
        }
        const auto open = line.find('{', units);
        const auto close = open == std::string::npos ? std::string::npos : line.find('}', open);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        const std::string object = line.substr(open, close - open + 1);
        double team = 0;
        double x = 0;
        double y = 0;
        double z = 0;
        if (!json_string_field(object, "def", out.def) || !json_number_field(object, "t", team) ||
            !json_number_field(object, "x", x) || !json_number_field(object, "y", y) ||
            !json_number_field(object, "z", z)) {
            continue;
        }
        out.team = static_cast<int>(team);
        out.x = x;
        out.y = y;
        out.z = z;
        out.ok = out.def == "armcom" || out.def == "corcom" || out.def == "legcom";
        return out.ok;
    }
    return false;
}

int count_team_pixels(const std::vector<uint8_t>& rgb) {
    int count = 0;
    const size_t pixels = rgb.size() / 3;
    for (size_t i = 0; i < pixels; ++i) {
        const int r = rgb[i * 3];
        const int g = rgb[i * 3 + 1];
        const int b = rgb[i * 3 + 2];
        if (r > g + 40 && r > b + 40 && r > 100) {
            ++count;
        }
    }
    return count;
}

int gate_m2(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double spawn_x = 0;
    double spawn_z = 0;
    double spawn_elmos = 0;
    double ground_y_min = 0;
    double ground_y_max = 0;
    double team_pixels_min = 0;
    double width = 0;
    double height = 0;
    double stddev_min = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "spawn_x", spawn_x) ||
        !toml_number(tolerance_text, "spawn_z", spawn_z) || !toml_number(tolerance_text, "spawn_elmos", spawn_elmos) ||
        !toml_number(tolerance_text, "ground_y_min", ground_y_min) ||
        !toml_number(tolerance_text, "ground_y_max", ground_y_max) ||
        !toml_number(tolerance_text, "team_pixels_min", team_pixels_min) ||
        !toml_number(tolerance_text, "commander_width", width) ||
        !toml_number(tolerance_text, "commander_height", height) ||
        !toml_number(tolerance_text, "commander_stddev_min", stddev_min)) {
        std::cerr << "gate: m2 tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string actual_trace = options.actual + "/boot.jsonl";
    const std::string oracle_trace = options.oracle + "/traces/boot.jsonl";
    const std::string picture_path = options.actual + "/m2/commander.ppm";
    SpawnRecord actual;
    SpawnRecord oracle;
    const bool actual_ok = first_unit(actual_trace, actual);
    const bool oracle_ok = first_unit(oracle_trace, oracle);

    bool position_ok = false;
    if (actual_ok && actual.team == 0 && std::fabs(actual.x - spawn_x) <= spawn_elmos &&
        std::fabs(actual.z - spawn_z) <= spawn_elmos && actual.y >= ground_y_min && actual.y <= ground_y_max) {
        position_ok = true;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    double stddev = 0;
    int team_pixels = 0;
    if (image_ok) {
        stddev = luminance_stddev(rgb);
        team_pixels = count_team_pixels(rgb);
    }
    const bool size_ok = image_ok && image_w == static_cast<int>(width) && image_h == static_cast<int>(height);
    const bool picture_ok = size_ok && stddev >= stddev_min && team_pixels >= static_cast<int>(team_pixels_min);

    bool oracle_match = false;
    if (oracle_ok && actual_ok && oracle.def == actual.def && oracle.team == actual.team &&
        std::fabs(oracle.x - actual.x) <= spawn_elmos && std::fabs(oracle.z - actual.z) <= spawn_elmos &&
        std::fabs(oracle.y - actual.y) <= spawn_elmos) {
        oracle_match = true;
    }

    failure << "# commander spawn\n\n";
    failure << "Scenario: boot\n\n";
    failure << "Frame: 0\n\n";
    failure << "Entity: commander\n\n";
    if (!actual_ok) {
        failure << "Field: spawn\n\n";
        failure << "Native value: no commander in `" << actual_trace << "`.\n\n";
    } else {
        failure << "Field: position\n\n";
        failure << "Native value: " << actual.def << " team " << actual.team << " at " << actual.x << " " << actual.y
                << " " << actual.z << ".\n\n";
    }
    failure << "Tolerance: team 0, xz within " << spawn_elmos << " elmos of " << spawn_x << " " << spawn_z
            << ", y inside " << ground_y_min << " .. " << ground_y_max << ".\n\n";
    failure << "Picture: ";
    if (!image_ok) {
        failure << image_error << " at `" << picture_path << "`.\n\n";
    } else {
        failure << image_w << "x" << image_h << ", stddev " << stddev << ", team pixels " << team_pixels << ".\n\n";
    }
    failure << "Picture tolerance: " << static_cast<int>(width) << "x" << static_cast<int>(height) << ", stddev >= "
            << stddev_min << ", team pixels >= " << static_cast<int>(team_pixels_min) << ".\n\n";
    if (!oracle_ok) {
        failure << "Oracle value: absent. `" << oracle_trace
                << "` is not a Recoil trace, so a matching client spawn is not a pass.\n";
    } else if (!oracle_match) {
        failure << "Oracle value: " << oracle.def << " team " << oracle.team << " at " << oracle.x << " " << oracle.y
                << " " << oracle.z << ".\n";
    } else {
        failure << "Oracle value: matches the client within " << spawn_elmos << " elmos.\n";
    }

    if (actual_ok) {
        std::cout << "spawn " << actual.def << " team " << actual.team << " at " << actual.x << " " << actual.y << " "
                  << actual.z;
    } else {
        std::cout << "spawn missing (" << actual_trace << ")";
    }
    std::cout << (position_ok ? " position ok\n" : " FAIL position\n");
    if (!image_ok) {
        std::cout << "commander FAIL " << image_error << "\n";
    } else {
        std::cout << "commander " << image_w << "x" << image_h << " stddev " << stddev << " team pixels " << team_pixels
                  << (picture_ok ? " picture ok\n" : " FAIL picture\n");
    }
    if (!oracle_ok) {
        std::cout << "oracle FAIL trace missing\n";
    } else if (!oracle_match) {
        std::cout << "oracle FAIL mismatch\n";
    } else {
        std::cout << "oracle match\n";
    }

    if (!position_ok) {
        std::cout << "m2 FAIL position\n";
        return 1;
    }
    if (!picture_ok) {
        std::cout << "m2 FAIL picture\n";
        return 1;
    }
    if (!oracle_ok) {
        std::cout << "m2 FAIL oracle trace missing\n";
        return 1;
    }
    if (!oracle_match) {
        std::cout << "m2 FAIL oracle\n";
        return 1;
    }
    std::cout << "m2 PASS\n";
    return 0;
}

struct Sample {
    bool ok = false;
    int frame = -1;
    double x = 0;
    double z = 0;
};

bool read_samples(const std::string& path, std::vector<Sample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        SpawnRecord unit;
        const auto units = line.find("\"units\"");
        const auto open = units == std::string::npos ? std::string::npos : line.find('{', units);
        const auto close = open == std::string::npos ? std::string::npos : line.find('}', open);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        const std::string object = line.substr(open, close - open + 1);
        double team = 0;
        double x = 0;
        double y = 0;
        double z = 0;
        double frame = 0;
        if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "x", x) ||
            !json_number_field(object, "z", z) || !json_number_field(line, "f", frame)) {
            continue;
        }
        (void)team;
        (void)y;
        if (unit.def != "armcom" && unit.def != "corcom" && unit.def != "legcom") {
            continue;
        }
        Sample sample;
        sample.ok = true;
        sample.frame = static_cast<int>(frame);
        sample.x = x;
        sample.z = z;
        out.push_back(sample);
    }
    return !out.empty();
}

int gate_m3(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double waypoint_x = 0;
    double waypoint_z = 0;
    double waypoint_elmos = 0;
    double sim_frame = 0;
    double early_frame = 0;
    double early_elmos = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "spawn_x", start_x) ||
        !toml_number(tolerance_text, "spawn_z", start_z) || !toml_number(tolerance_text, "waypoint_x", waypoint_x) ||
        !toml_number(tolerance_text, "waypoint_z", waypoint_z) ||
        !toml_number(tolerance_text, "waypoint_elmos", waypoint_elmos) ||
        !toml_number(tolerance_text, "sim_frame", sim_frame) || !toml_number(tolerance_text, "early_frame", early_frame) ||
        !toml_number(tolerance_text, "early_elmos", early_elmos)) {
        std::cerr << "gate: m3 tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string actual_trace = options.actual + "/move_kbot.jsonl";
    const std::string oracle_trace = options.oracle + "/traces/move_kbot.jsonl";
    std::vector<Sample> actual;
    std::vector<Sample> oracle;
    const bool actual_ok = read_samples(actual_trace, actual);
    const bool oracle_ok = read_samples(oracle_trace, oracle);
    const Sample* early = nullptr;
    const Sample* final_sample = nullptr;
    for (const Sample& sample : actual) {
        if (sample.frame == static_cast<int>(early_frame)) {
            early = &sample;
        }
        if (sample.frame >= static_cast<int>(sim_frame)) {
            final_sample = &sample;
        }
    }
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    bool early_ok = false;
    double early_distance = 0;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance <= early_elmos && early_distance >= 2.0;
    }
    bool arrived = false;
    double final_distance = 0;
    if (final_sample != nullptr) {
        final_distance = distance(final_sample->x, final_sample->z, waypoint_x, waypoint_z);
        arrived = final_distance <= waypoint_elmos;
    }
    bool oracle_match = false;
    if (oracle_ok && final_sample != nullptr) {
        const Sample& last = oracle.back();
        oracle_match = distance(last.x, last.z, final_sample->x, final_sample->z) <= waypoint_elmos;
    }

    failure << "# commander move\n\n";
    failure << "Scenario: move_kbot\n\n";
    failure << "Frame: " << static_cast<int>(sim_frame) << "\n\n";
    failure << "Entity: commander\n\n";
    failure << "Field: position\n\n";
    if (final_sample != nullptr) {
        failure << "Native value: frame " << final_sample->frame << " at " << final_sample->x << " " << final_sample->z
                << ", " << final_distance << " elmos from the waypoint.\n\n";
    } else {
        failure << "Native value: no sample at frame " << static_cast<int>(sim_frame) << " in `" << actual_trace
                << "`.\n\n";
    }
    failure << "Tolerance: at frame " << static_cast<int>(early_frame) << " the unit is between 2 and " << early_elmos
            << " elmos from the start, and by frame " << static_cast<int>(sim_frame) << " it is within "
            << waypoint_elmos << " elmos of " << waypoint_x << " " << waypoint_z << ".\n\n";
    if (!oracle_ok) {
        failure << "Oracle value: absent. `" << oracle_trace
                << "` is not a Recoil trace, so a matching client path is not a pass.\n";
    } else if (!oracle_match) {
        failure << "Oracle value: last sample at " << oracle.back().x << " " << oracle.back().z << ".\n";
    } else {
        failure << "Oracle value: matches the client within " << waypoint_elmos << " elmos.\n";
    }

    if (early != nullptr) {
        std::cout << "early frame " << early->frame << " distance " << early_distance
                  << (early_ok ? " early ok\n" : " FAIL early\n");
    } else {
        std::cout << "early FAIL missing frame " << static_cast<int>(early_frame) << "\n";
    }
    if (final_sample != nullptr) {
        std::cout << "arrive frame " << final_sample->frame << " distance " << final_distance
                  << (arrived ? " arrive ok\n" : " FAIL arrive\n");
    } else {
        std::cout << "arrive FAIL missing final sample\n";
    }
    if (!oracle_ok) {
        std::cout << "oracle FAIL trace missing\n";
    } else if (!oracle_match) {
        std::cout << "oracle FAIL mismatch\n";
    } else {
        std::cout << "oracle match\n";
    }

    if (!early_ok) {
        std::cout << "m3 FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "m3 FAIL position\n";
        return 1;
    }
    if (!oracle_ok) {
        std::cout << "m3 FAIL oracle trace missing\n";
        return 1;
    }
    if (!oracle_match) {
        std::cout << "m3 FAIL oracle\n";
        return 1;
    }
    std::cout << "m3 PASS\n";
    return 0;
}

struct PieceSample {
    int frame = -1;
    double lthigh_x = 0;
    double rthigh_x = 0;
    double dish_y = 0;
};

double shortest_arc(double from, double to) {
    constexpr double kPi = 3.141592653589793;
    double wrapped = std::fmod(to - from + 3.0 * kPi, 2.0 * kPi);
    if (wrapped < 0.0) {
        wrapped += 2.0 * kPi;
    }
    return wrapped - kPi;
}

bool read_piece_samples(const std::string& path, std::vector<PieceSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        double frame = 0;
        double left = 0;
        double right = 0;
        double dish = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(line, "lthigh_x", left) ||
            !json_number_field(line, "rthigh_x", right) || !json_number_field(line, "dish_y", dish)) {
            continue;
        }
        PieceSample sample;
        sample.frame = static_cast<int>(frame);
        sample.lthigh_x = left;
        sample.rthigh_x = right;
        sample.dish_y = dish;
        out.push_back(sample);
    }
    return !out.empty();
}

const PieceSample* piece_at(const std::vector<PieceSample>& samples, int frame) {
    for (const PieceSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

struct TeamStats {
    int count = 0;
    double cx = 0;
    double cy = 0;
    double box_w = 0;
    double box_h = 0;
    std::vector<uint8_t> mask;
};

bool team_stats(const std::vector<uint8_t>& rgb, int width, int height, TeamStats& out) {
    out = TeamStats{};
    out.mask.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    long sum_x = 0;
    long sum_y = 0;
    int min_x = width;
    int min_y = height;
    int max_x = 0;
    int max_y = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const int r = rgb[pixel * 3];
            const int g = rgb[pixel * 3 + 1];
            const int b = rgb[pixel * 3 + 2];
            if (!(r > g + 40 && r > b + 40 && r > 100)) {
                continue;
            }
            out.mask[pixel] = 1;
            ++out.count;
            sum_x += x;
            sum_y += y;
            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }
    if (out.count == 0) {
        return false;
    }
    out.cx = (static_cast<double>(sum_x) / out.count) / static_cast<double>(width);
    out.cy = (static_cast<double>(sum_y) / out.count) / static_cast<double>(height);
    out.box_w = static_cast<double>(max_x - min_x + 1) / static_cast<double>(width);
    out.box_h = static_cast<double>(max_y - min_y + 1) / static_cast<double>(height);
    return true;
}

struct HullStats {
    int count = 0;
    double cx = 0;
    double cy = 0;
    double box = 0;
};

// The ship is dark metal. A full-frame team-color count is the red shore, so the hull is the
// dark pixels in the locked middle of the frame. Open water in that window measured none.
bool hull_stats(const std::vector<uint8_t>& rgb, int width, int height, HullStats& out) {
    out = HullStats{};
    const int x0 = static_cast<int>(static_cast<double>(width) * 0.35);
    const int x1 = static_cast<int>(static_cast<double>(width) * 0.65);
    const int y0 = static_cast<int>(static_cast<double>(height) * 0.35);
    const int y1 = static_cast<int>(static_cast<double>(height) * 0.75);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    long sum_x = 0;
    long sum_y = 0;
    int min_x = width;
    int min_y = height;
    int max_x = 0;
    int max_y = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t pixel = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3;
            const int sum = rgb[pixel] + rgb[pixel + 1] + rgb[pixel + 2];
            if (sum >= 140) {
                continue;
            }
            ++out.count;
            sum_x += x;
            sum_y += y;
            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }
    if (out.count == 0) {
        return false;
    }
    out.cx = (static_cast<double>(sum_x) / out.count) / static_cast<double>(width);
    out.cy = (static_cast<double>(sum_y) / out.count) / static_cast<double>(height);
    const double box_w = static_cast<double>(max_x - min_x + 1) / static_cast<double>(width);
    const double box_h = static_cast<double>(max_y - min_y + 1) / static_cast<double>(height);
    out.box = std::max(box_w, box_h);
    return true;
}

// Same locked middle as the hull check. Team color here is the unit, not the whole shore.
int window_team_pixels(const std::vector<uint8_t>& rgb, int width, int height) {
    const int x0 = static_cast<int>(static_cast<double>(width) * 0.35);
    const int x1 = static_cast<int>(static_cast<double>(width) * 0.65);
    const int y0 = static_cast<int>(static_cast<double>(height) * 0.35);
    const int y1 = static_cast<int>(static_cast<double>(height) * 0.75);
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t pixel = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3;
            const int r = rgb[pixel];
            const int g = rgb[pixel + 1];
            const int b = rgb[pixel + 2];
            if (r > g + 40 && r > b + 40 && r > 100) {
                ++count;
            }
        }
    }
    return count;
}

int leg_xor(const TeamStats& left, const TeamStats& right, int width, int height) {
    if (left.mask.size() != right.mask.size()) {
        return -1;
    }
    const double split = (left.cy + right.cy) * 0.5 * static_cast<double>(height);
    int count = 0;
    for (int y = 0; y < height; ++y) {
        if (static_cast<double>(y) <= split) {
            continue;
        }
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            if (left.mask[pixel] != right.mask[pixel]) {
                ++count;
            }
        }
    }
    return count;
}

int gate_m3walk(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double thigh_delta_min = 0;
    double thigh_oppose_min = 0;
    double idle_thigh_max = 0;
    double idle_dish_min = 0;
    double walk_team_min = 0;
    double walk_team_max = 0;
    double shot_width = 0;
    double shot_height = 0;
    double leg_xor_min = 0;
    double walk_center_max = 0;
    double walk_box_max = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "thigh_delta_min", thigh_delta_min) ||
        !toml_number(tolerance_text, "thigh_oppose_min", thigh_oppose_min) ||
        !toml_number(tolerance_text, "idle_thigh_max", idle_thigh_max) ||
        !toml_number(tolerance_text, "idle_dish_min", idle_dish_min) ||
        !toml_number(tolerance_text, "walk_team_min", walk_team_min) ||
        !toml_number(tolerance_text, "walk_team_max", walk_team_max) ||
        !toml_number(tolerance_text, "walk_shot_width", shot_width) ||
        !toml_number(tolerance_text, "walk_shot_height", shot_height)) {
        std::cerr << "gate: m3walk tolerances are missing\n";
        return 2;
    }
    const bool have_xor = toml_number(tolerance_text, "leg_xor_min", leg_xor_min);
    const bool have_center = toml_number(tolerance_text, "walk_center_max", walk_center_max);
    const bool have_box = toml_number(tolerance_text, "walk_box_max", walk_box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }
    failure << "# commander walk\n\n";
    failure << "Scenario: move_kbot walk\n\n";
    failure << "This suite reads engine piece samples and locked-camera shots. It does not read a Recoil trace.\n\n";

    std::vector<PieceSample> walk_pieces;
    std::vector<PieceSample> idle_pieces;
    const std::string walk_piece_path = options.actual + "/m3walk/walk/pieces.jsonl";
    const std::string idle_piece_path = options.actual + "/m3walk/idle/pieces.jsonl";
    const bool walk_pieces_ok = read_piece_samples(walk_piece_path, walk_pieces);
    const bool idle_pieces_ok = read_piece_samples(idle_piece_path, idle_pieces);

    double max_step = 0;
    double max_oppose = 0;
    int step_pairs = 0;
    if (walk_pieces_ok) {
        for (size_t i = 0; i < walk_pieces.size(); ++i) {
            const PieceSample& sample = walk_pieces[i];
            if (sample.frame < 30 || sample.frame > 150) {
                continue;
            }
            const double oppose = std::fabs(shortest_arc(sample.lthigh_x, sample.rthigh_x));
            if (oppose > max_oppose) {
                max_oppose = oppose;
            }
            if (i == 0) {
                continue;
            }
            const PieceSample& prev = walk_pieces[i - 1];
            const int gap = sample.frame - prev.frame;
            if (prev.frame < 30 || gap <= 0 || gap > 20) {
                continue;
            }
            const double step = std::fabs(shortest_arc(prev.lthigh_x, sample.lthigh_x));
            if (step > max_step) {
                max_step = step;
            }
            ++step_pairs;
        }
    }
    const bool step_ok = step_pairs > 0 && max_step >= thigh_delta_min;
    const bool oppose_ok = max_oppose >= thigh_oppose_min;
    const PieceSample* idle_a = idle_pieces_ok ? piece_at(idle_pieces, 60) : nullptr;
    const PieceSample* idle_b = idle_pieces_ok ? piece_at(idle_pieces, 90) : nullptr;
    double idle_thigh = 0;
    double idle_dish = 0;
    const bool idle_pose_ok = idle_a != nullptr && idle_b != nullptr;
    if (idle_pose_ok) {
        idle_thigh = std::fabs(shortest_arc(idle_a->lthigh_x, idle_b->lthigh_x));
        idle_dish = std::fabs(shortest_arc(idle_a->dish_y, idle_b->dish_y));
    }
    const bool idle_still = idle_pose_ok && idle_thigh < idle_thigh_max;
    const bool dish_spins = idle_pose_ok && idle_dish > idle_dish_min;

    std::cout << "walk thigh step " << max_step << (step_ok ? " step ok\n" : " FAIL step\n");
    std::cout << "walk thigh oppose " << max_oppose << (oppose_ok ? " oppose ok\n" : " FAIL oppose\n");
    if (!idle_pose_ok) {
        std::cout << "idle FAIL missing frame 60 or 90\n";
    } else {
        std::cout << "idle thigh " << idle_thigh << (idle_still ? " still ok\n" : " FAIL still\n");
        std::cout << "idle dish " << idle_dish << (dish_spins ? " spin ok\n" : " FAIL spin\n");
    }

    const int walk_frames[] = {60, 75, 90, 105, 120, 135, 150};
    const int idle_frames[] = {60, 90};
    bool shots_ok = true;
    double worst_center = 0;
    double worst_box = 0;
    int idle_xor = -1;
    int walk_xor = -1;
    TeamStats idle_shot[2];
    TeamStats walk_shot[7];
    bool idle_mask[2] = {false, false};
    bool walk_mask[7] = {false, false, false, false, false, false, false};

    auto check_shot = [&](const std::string& path, TeamStats& stats, bool& ok) {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgb;
        std::string image_error;
        if (!read_ppm(path, width, height, rgb, image_error)) {
            std::cout << path << " FAIL " << image_error << "\n";
            shots_ok = false;
            return;
        }
        if (width != static_cast<int>(shot_width) || height != static_cast<int>(shot_height)) {
            std::cout << path << " FAIL size " << width << "x" << height << "\n";
            shots_ok = false;
            return;
        }
        if (!team_stats(rgb, width, height, stats)) {
            std::cout << path << " FAIL no team pixels\n";
            shots_ok = false;
            return;
        }
        const double center = std::sqrt((stats.cx - 0.5) * (stats.cx - 0.5) + (stats.cy - 0.5) * (stats.cy - 0.5));
        const double box = stats.box_w > stats.box_h ? stats.box_w : stats.box_h;
        if (center > worst_center) {
            worst_center = center;
        }
        if (box > worst_box) {
            worst_box = box;
        }
        const bool count_ok = stats.count >= static_cast<int>(walk_team_min) && stats.count <= static_cast<int>(walk_team_max);
        std::cout << path << " team " << stats.count << " center " << center << " box " << box
                  << (count_ok ? "\n" : " FAIL team count\n");
        if (!count_ok) {
            shots_ok = false;
        }
        ok = true;
    };

    for (int i = 0; i < 2; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "/m3walk/idle/f%03d.ppm", idle_frames[i]);
        check_shot(options.actual + name, idle_shot[i], idle_mask[i]);
    }
    for (int i = 0; i < 7; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "/m3walk/walk/f%03d.ppm", walk_frames[i]);
        check_shot(options.actual + name, walk_shot[i], walk_mask[i]);
    }
    if (idle_mask[0] && idle_mask[1]) {
        idle_xor = leg_xor(idle_shot[0], idle_shot[1], static_cast<int>(shot_width), static_cast<int>(shot_height));
        std::cout << "idle leg xor " << idle_xor << "\n";
    }
    if (shots_ok) {
        for (int i = 1; i < 7; ++i) {
            if (!walk_mask[i - 1] || !walk_mask[i]) {
                continue;
            }
            const int pair = leg_xor(walk_shot[i - 1], walk_shot[i], static_cast<int>(shot_width), static_cast<int>(shot_height));
            std::cout << "walk leg xor f" << walk_frames[i - 1] << " f" << walk_frames[i] << " " << pair << "\n";
            if (pair > walk_xor) {
                walk_xor = pair;
            }
        }
    }
    std::cout << "walk center max " << worst_center << " box max " << worst_box << "\n";

    const bool center_ok = have_center && worst_center <= walk_center_max;
    const bool box_ok = have_box && worst_box <= walk_box_max;
    const bool xor_ok = have_xor && idle_xor >= 0 && walk_xor >= 0 && idle_xor < leg_xor_min && walk_xor >= leg_xor_min;
    if (have_center) {
        std::cout << (center_ok ? "center ok\n" : "FAIL center\n");
    } else {
        std::cout << "center bar not set\n";
    }
    if (have_box) {
        std::cout << (box_ok ? "box ok\n" : "FAIL box\n");
    } else {
        std::cout << "box bar not set\n";
    }
    if (have_xor) {
        std::cout << "leg xor bar " << leg_xor_min << (xor_ok ? " xor ok\n" : " FAIL xor\n");
    } else {
        std::cout << "leg xor bar not set\n";
    }

    failure << "Walk thigh step: " << max_step << " (min " << thigh_delta_min << ")\n\n";
    failure << "Walk thigh oppose: " << max_oppose << " (min " << thigh_oppose_min << ")\n\n";
    failure << "Idle thigh delta: " << idle_thigh << " (max " << idle_thigh_max << ")\n\n";
    failure << "Idle dish delta: " << idle_dish << " (min " << idle_dish_min << ")\n\n";
    failure << "Idle leg xor: " << idle_xor << "\n\n";
    failure << "Walk leg xor: " << walk_xor << "\n\n";
    failure << "Center: " << worst_center << "\n\n";
    failure << "Box: " << worst_box << "\n\n";

    if (!walk_pieces_ok || !step_ok) {
        std::cout << "m3walk FAIL step\n";
        return 1;
    }
    if (!oppose_ok) {
        std::cout << "m3walk FAIL oppose\n";
        return 1;
    }
    if (!idle_still) {
        std::cout << "m3walk FAIL idle\n";
        return 1;
    }
    if (!dish_spins) {
        std::cout << "m3walk FAIL dish\n";
        return 1;
    }
    if (!shots_ok) {
        std::cout << "m3walk FAIL shots\n";
        return 1;
    }
    if (!have_xor || !have_center || !have_box) {
        std::cout << "m3walk FAIL visual bar not set\n";
        return 2;
    }
    if (!center_ok || !box_ok || !xor_ok) {
        std::cout << "m3walk FAIL picture\n";
        return 1;
    }
    std::cout << "m3walk PASS\n";
    failure << "The walk self-check passed on engine output. Suite m3 stays red without a Recoil trace.\n";
    return 0;
}

struct ShipSample {
    int frame = -1;
    double x = 0;
    double y = 0;
    double z = 0;
    double gy = 0;
    double spd = 0;
    double bank = 0;
    std::string def;
};

bool read_ship_samples(const std::string& path, std::vector<ShipSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto units = line.find("\"units\"");
        const auto open = units == std::string::npos ? std::string::npos : line.find('{', units);
        const auto close = open == std::string::npos ? std::string::npos : line.find('}', open);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        const std::string object = line.substr(open, close - open + 1);
        double x = 0;
        double y = 0;
        double z = 0;
        double gy = 0;
        double frame = 0;
        ShipSample sample;
        if (!json_string_field(object, "def", sample.def) || !json_number_field(object, "x", x) ||
            !json_number_field(object, "y", y) || !json_number_field(object, "z", z) ||
            !json_number_field(object, "gy", gy) || !json_number_field(line, "f", frame)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.x = x;
        sample.y = y;
        sample.z = z;
        sample.gy = gy;
        json_number_field(object, "spd", sample.spd);
        json_number_field(object, "bank", sample.bank);
        out.push_back(sample);
    }
    return !out.empty();
}

const ShipSample* ship_at(const std::vector<ShipSample>& samples, int frame) {
    for (const ShipSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

int gate_m3ship(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double water_x = 0;
    double water_z = 0;
    double land_x = 0;
    double land_z = 0;
    double early_at = 0;
    double early_min = 0;
    double early_max = 0;
    double done_at = 0;
    double shore_at = 0;
    double arrive_elmos = 0;
    double depth_ceiling = 0;
    double surface_slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "boat_start_x", start_x) ||
        !toml_number(tolerance_text, "boat_start_z", start_z) || !toml_number(tolerance_text, "boat_water_x", water_x) ||
        !toml_number(tolerance_text, "boat_water_z", water_z) || !toml_number(tolerance_text, "boat_land_x", land_x) ||
        !toml_number(tolerance_text, "boat_land_z", land_z) || !toml_number(tolerance_text, "boat_early_at", early_at) ||
        !toml_number(tolerance_text, "boat_early_min", early_min) ||
        !toml_number(tolerance_text, "boat_early_max", early_max) ||
        !toml_number(tolerance_text, "boat_done_at", done_at) || !toml_number(tolerance_text, "boat_shore_at", shore_at) ||
        !toml_number(tolerance_text, "boat_arrive_elmos", arrive_elmos) ||
        !toml_number(tolerance_text, "boat_depth_ceiling", depth_ceiling) ||
        !toml_number(tolerance_text, "boat_surface_slop", surface_slop)) {
        std::cerr << "gate: m3ship tolerances are missing\n";
        return 2;
    }
    double hull_min = 0;
    double hull_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "boat_hull_min", hull_min) &&
                             toml_number(tolerance_text, "boat_hull_max", hull_max) &&
                             toml_number(tolerance_text, "boat_shot_w", shot_w) &&
                             toml_number(tolerance_text, "boat_shot_h", shot_h) &&
                             toml_number(tolerance_text, "boat_center_max", center_max) &&
                             toml_number(tolerance_text, "boat_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m3ship/move_ship.jsonl";
    const std::string picture_path = options.actual + "/m3ship/hull-end.ppm";
    std::vector<ShipSample> samples;
    const bool trace_ok = read_ship_samples(trace_path, samples);
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    const ShipSample* early = ship_at(samples, static_cast<int>(early_at));
    const ShipSample* done = ship_at(samples, static_cast<int>(done_at));
    bool depth_ok = trace_ok;
    bool surface_ok = trace_ok;
    bool def_ok = trace_ok;
    bool shore_ok = false;
    int shore_samples = 0;
    double shallow_gy = 0;
    double worst_y = 0;
    bool saw_gy = false;
    for (const ShipSample& sample : samples) {
        if (sample.def != "armdecade") {
            def_ok = false;
        }
        if (sample.gy > depth_ceiling) {
            depth_ok = false;
        }
        if (std::fabs(sample.y) > surface_slop) {
            surface_ok = false;
        }
        if (!saw_gy || sample.gy > shallow_gy) {
            shallow_gy = sample.gy;
            saw_gy = true;
        }
        if (std::fabs(sample.y) > std::fabs(worst_y)) {
            worst_y = sample.y;
        }
        if (sample.frame >= static_cast<int>(shore_at)) {
            ++shore_samples;
            const double from_water = distance(sample.x, sample.z, water_x, water_z);
            const double from_land = distance(sample.x, sample.z, land_x, land_z);
            if (from_water > arrive_elmos || from_land <= arrive_elmos) {
                shore_ok = false;
            } else if (shore_samples == 1) {
                shore_ok = true;
            }
        }
    }
    if (shore_samples == 0) {
        shore_ok = false;
    }
    double early_distance = 0;
    bool early_ok = false;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance >= early_min && early_distance <= early_max;
    }
    double done_distance = 0;
    bool arrived = false;
    if (done != nullptr) {
        done_distance = distance(done->x, done->z, water_x, water_z);
        arrived = done_distance <= arrive_elmos;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# ship move\n\n";
    failure << "Scenario: move_ship\n\n";
    failure << "Entity: armdecade\n\n";
    failure << "Field: water depth and shore\n\n";
    if (early != nullptr) {
        failure << "Frame " << early->frame << " is " << early_distance << " elmos from the start.\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(early_at) << ".\n\n";
    }
    if (done != nullptr) {
        failure << "Frame " << done->frame << " is " << done_distance << " elmos from the water waypoint.\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(done_at) << ".\n\n";
    }
    failure << "Shallowest gy: " << shallow_gy << " (ceiling " << depth_ceiling << ")\n\n";
    failure << "Largest |y|: " << worst_y << " (slop " << surface_slop << ")\n\n";
    failure << "Shore samples: " << shore_samples << "\n\n";
    if (stats_ok) {
        failure << "Hull pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    } else {
        failure << "Picture: " << (image_ok ? "no dark hull in the locked middle" : image_error) << "\n\n";
    }

    if (!trace_ok || !def_ok) {
        std::cout << "m3ship FAIL trace\n";
        return 1;
    }
    if (!early_ok) {
        std::cout << "early distance " << early_distance << "\n";
        std::cout << "m3ship FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "arrive distance " << done_distance << "\n";
        std::cout << "m3ship FAIL arrive\n";
        return 1;
    }
    if (!depth_ok) {
        std::cout << "gy " << shallow_gy << "\n";
        std::cout << "m3ship FAIL depth\n";
        return 1;
    }
    if (!surface_ok) {
        std::cout << "y " << worst_y << "\n";
        std::cout << "m3ship FAIL surface\n";
        return 1;
    }
    if (!shore_ok) {
        std::cout << "m3ship FAIL shore\n";
        return 1;
    }
    std::cout << "early distance " << early_distance << "\n";
    std::cout << "arrive distance " << done_distance << "\n";
    std::cout << "gy " << shallow_gy << " y " << worst_y << "\n";
    if (stats_ok) {
        std::cout << "hull " << stats.count << " center " << center << " box " << stats.box << " size " << image_w
                  << "x" << image_h << "\n";
    }
    if (!have_visual) {
        std::cout << "m3ship FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < hull_min || stats.count > hull_max || center > center_max || stats.box > box_max) {
        std::cout << "m3ship FAIL picture\n";
        return 1;
    }
    std::cout << "m3ship PASS\n";
    failure << "The ship self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace.\n";
    return 0;
}

int gate_m3hover(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double water_x = 0;
    double water_z = 0;
    double cliff_x = 0;
    double cliff_z = 0;
    double early_at = 0;
    double early_min = 0;
    double early_max = 0;
    double done_at = 0;
    double stay_at = 0;
    double arrive_elmos = 0;
    double depth_ceiling = 0;
    double surface_slop = 0;
    double climb_max = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "hover_start_x", start_x) ||
        !toml_number(tolerance_text, "hover_start_z", start_z) || !toml_number(tolerance_text, "hover_water_x", water_x) ||
        !toml_number(tolerance_text, "hover_water_z", water_z) || !toml_number(tolerance_text, "hover_cliff_x", cliff_x) ||
        !toml_number(tolerance_text, "hover_cliff_z", cliff_z) || !toml_number(tolerance_text, "hover_early_at", early_at) ||
        !toml_number(tolerance_text, "hover_early_min", early_min) ||
        !toml_number(tolerance_text, "hover_early_max", early_max) || !toml_number(tolerance_text, "hover_done_at", done_at) ||
        !toml_number(tolerance_text, "hover_stay_at", stay_at) ||
        !toml_number(tolerance_text, "hover_arrive_elmos", arrive_elmos) ||
        !toml_number(tolerance_text, "hover_depth_ceiling", depth_ceiling) ||
        !toml_number(tolerance_text, "hover_surface_slop", surface_slop) ||
        !toml_number(tolerance_text, "hover_climb_max", climb_max)) {
        std::cerr << "gate: m3hover tolerances are missing\n";
        return 2;
    }
    double body_min = 0;
    double body_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "hover_body_min", body_min) &&
                             toml_number(tolerance_text, "hover_body_max", body_max) &&
                             toml_number(tolerance_text, "hover_shot_w", shot_w) &&
                             toml_number(tolerance_text, "hover_shot_h", shot_h) &&
                             toml_number(tolerance_text, "hover_center_max", center_max) &&
                             toml_number(tolerance_text, "hover_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m3hover/move_hover.jsonl";
    const std::string picture_path = options.actual + "/m3hover/craft-end.ppm";
    std::vector<ShipSample> samples;
    const bool trace_ok = read_ship_samples(trace_path, samples);
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    const ShipSample* early = ship_at(samples, static_cast<int>(early_at));
    const ShipSample* done = ship_at(samples, static_cast<int>(done_at));
    bool def_ok = trace_ok;
    bool climb_ok = trace_ok;
    bool depth_ok = false;
    bool surface_ok = false;
    bool stay_ok = false;
    int stay_samples = 0;
    int water_samples = 0;
    double highest_y = 0;
    bool saw_y = false;
    for (const ShipSample& sample : samples) {
        if (sample.def != "armsh") {
            def_ok = false;
        }
        if (!saw_y || sample.y > highest_y) {
            highest_y = sample.y;
            saw_y = true;
        }
        if (sample.y > climb_max) {
            climb_ok = false;
        }
        const bool on_water = sample.frame == static_cast<int>(done_at) || sample.frame >= static_cast<int>(stay_at);
        if (on_water) {
            ++water_samples;
            const bool depth = sample.gy <= depth_ceiling;
            const bool surface = std::fabs(sample.y) <= surface_slop;
            if (water_samples == 1) {
                depth_ok = depth;
                surface_ok = surface;
            } else if (!depth) {
                depth_ok = false;
            } else if (!surface) {
                surface_ok = false;
            }
        }
        if (sample.frame >= static_cast<int>(stay_at)) {
            ++stay_samples;
            const double from_water = distance(sample.x, sample.z, water_x, water_z);
            const double from_cliff = distance(sample.x, sample.z, cliff_x, cliff_z);
            const bool held = from_water <= arrive_elmos && from_cliff > arrive_elmos;
            if (stay_samples == 1) {
                stay_ok = held;
            } else if (!held) {
                stay_ok = false;
            }
        }
    }
    if (water_samples == 0) {
        depth_ok = false;
        surface_ok = false;
    }
    double early_distance = 0;
    bool early_ok = false;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance >= early_min && early_distance <= early_max;
    }
    double done_distance = 0;
    bool arrived = false;
    if (done != nullptr) {
        done_distance = distance(done->x, done->z, water_x, water_z);
        arrived = done_distance <= arrive_elmos;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const int team_window = image_ok ? window_team_pixels(rgb, image_w, image_h) : 0;
    const int team_frame = image_ok ? count_team_pixels(rgb) : 0;
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# hover move\n\n";
    failure << "Scenario: move_hover\n\n";
    failure << "Entity: armsh\n\n";
    failure << "Field: surface ride and cliff refusal\n\n";
    if (early != nullptr) {
        failure << "Frame " << early->frame << " is " << early_distance << " elmos from the start.\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(early_at) << ".\n\n";
    }
    if (done != nullptr) {
        failure << "Frame " << done->frame << " is " << done_distance << " elmos from the water waypoint. y "
                << done->y << " gy " << done->gy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(done_at) << ".\n\n";
    }
    failure << "Highest y: " << highest_y << " (ceiling " << climb_max << ")\n\n";
    failure << "Stay samples: " << stay_samples << "\n\n";
    if (stats_ok) {
        failure << "Dark pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    }
    failure << "Window team pixels: " << team_window << " frame team pixels: " << team_frame << "\n\n";

    if (!trace_ok || !def_ok) {
        std::cout << "m3hover FAIL trace\n";
        return 1;
    }
    if (!early_ok) {
        std::cout << "early distance " << early_distance << "\n";
        std::cout << "m3hover FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "arrive distance " << done_distance << "\n";
        std::cout << "m3hover FAIL arrive\n";
        return 1;
    }
    if (!climb_ok) {
        std::cout << "y " << highest_y << "\n";
        std::cout << "m3hover FAIL climb\n";
        return 1;
    }
    if (!depth_ok) {
        std::cout << "m3hover FAIL depth\n";
        return 1;
    }
    if (!surface_ok) {
        std::cout << "m3hover FAIL surface\n";
        return 1;
    }
    if (!stay_ok) {
        std::cout << "m3hover FAIL cliff\n";
        return 1;
    }
    std::cout << "early distance " << early_distance << "\n";
    std::cout << "arrive distance " << done_distance << "\n";
    std::cout << "highest y " << highest_y << "\n";
    if (image_ok) {
        std::cout << "window team " << team_window << " frame team " << team_frame << " dark "
                  << (stats_ok ? stats.count : 0) << " center " << center << " box " << (stats_ok ? stats.box : 0)
                  << " size " << image_w << "x" << image_h << "\n";
    } else {
        std::cout << "picture " << image_error << "\n";
    }
    if (!have_visual) {
        std::cout << "m3hover FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < body_min || stats.count > body_max || center > center_max || stats.box > box_max) {
        std::cout << "m3hover FAIL picture\n";
        return 1;
    }
    std::cout << "m3hover PASS\n";
    failure << "The hover self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace.\n";
    return 0;
}

int gate_m3gunship(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double goal_x = 0;
    double goal_z = 0;
    double early_at = 0;
    double early_min = 0;
    double early_max = 0;
    double done_at = 0;
    double arrive_elmos = 0;
    double cruise = 0;
    double alt_slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "gunship_start_x", start_x) ||
        !toml_number(tolerance_text, "gunship_start_z", start_z) || !toml_number(tolerance_text, "gunship_goal_x", goal_x) ||
        !toml_number(tolerance_text, "gunship_goal_z", goal_z) || !toml_number(tolerance_text, "gunship_early_at", early_at) ||
        !toml_number(tolerance_text, "gunship_early_min", early_min) ||
        !toml_number(tolerance_text, "gunship_early_max", early_max) ||
        !toml_number(tolerance_text, "gunship_done_at", done_at) ||
        !toml_number(tolerance_text, "gunship_arrive_elmos", arrive_elmos) ||
        !toml_number(tolerance_text, "gunship_cruise", cruise) || !toml_number(tolerance_text, "gunship_alt_slop", alt_slop)) {
        std::cerr << "gate: m3gunship tolerances are missing\n";
        return 2;
    }
    double body_min = 0;
    double body_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "gunship_body_min", body_min) &&
                             toml_number(tolerance_text, "gunship_body_max", body_max) &&
                             toml_number(tolerance_text, "gunship_shot_w", shot_w) &&
                             toml_number(tolerance_text, "gunship_shot_h", shot_h) &&
                             toml_number(tolerance_text, "gunship_center_max", center_max) &&
                             toml_number(tolerance_text, "gunship_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m3gunship/move_gunship.jsonl";
    const std::string picture_path = options.actual + "/m3gunship/craft-end.ppm";
    std::vector<ShipSample> samples;
    const bool trace_ok = read_ship_samples(trace_path, samples);
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    const ShipSample* early = ship_at(samples, static_cast<int>(early_at));
    const ShipSample* done = ship_at(samples, static_cast<int>(done_at));
    bool def_ok = trace_ok;
    bool alt_ok = trace_ok;
    double worst_alt = 0;
    for (const ShipSample& sample : samples) {
        if (sample.def != "armkam") {
            def_ok = false;
        }
        const double surface = sample.gy > 0 ? sample.gy : 0;
        const double alt = std::fabs(sample.y - surface - cruise);
        if (alt > worst_alt) {
            worst_alt = alt;
        }
        if (alt > alt_slop) {
            alt_ok = false;
        }
    }
    double early_distance = 0;
    bool early_ok = false;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance >= early_min && early_distance <= early_max;
    }
    double done_distance = 0;
    bool arrived = false;
    if (done != nullptr) {
        done_distance = distance(done->x, done->z, goal_x, goal_z);
        arrived = done_distance <= arrive_elmos;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const int team_window = image_ok ? window_team_pixels(rgb, image_w, image_h) : 0;
    const int team_frame = image_ok ? count_team_pixels(rgb) : 0;
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# gunship move\n\n";
    failure << "Scenario: move_gunship\n\n";
    failure << "Entity: armkam\n\n";
    failure << "Field: cruise over terrain\n\n";
    if (early != nullptr) {
        failure << "Frame " << early->frame << " is " << early_distance << " elmos from the start.\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(early_at) << ".\n\n";
    }
    if (done != nullptr) {
        failure << "Frame " << done->frame << " is " << done_distance << " elmos from the goal. y " << done->y
                << " gy " << done->gy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(done_at) << ".\n\n";
    }
    failure << "Worst altitude error: " << worst_alt << " (slop " << alt_slop << ")\n\n";
    if (stats_ok) {
        failure << "Dark pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    }
    failure << "Window team pixels: " << team_window << " frame team pixels: " << team_frame << "\n\n";

    if (!trace_ok || !def_ok) {
        std::cout << "m3gunship FAIL trace\n";
        return 1;
    }
    if (!early_ok) {
        std::cout << "early distance " << early_distance << "\n";
        std::cout << "m3gunship FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "arrive distance " << done_distance << "\n";
        std::cout << "m3gunship FAIL arrive\n";
        return 1;
    }
    if (!alt_ok) {
        std::cout << "altitude " << worst_alt << "\n";
        std::cout << "m3gunship FAIL altitude\n";
        return 1;
    }
    std::cout << "early distance " << early_distance << "\n";
    std::cout << "arrive distance " << done_distance << "\n";
    std::cout << "altitude " << worst_alt << "\n";
    if (image_ok) {
        std::cout << "window team " << team_window << " frame team " << team_frame << " dark "
                  << (stats_ok ? stats.count : 0) << " center " << center << " box " << (stats_ok ? stats.box : 0)
                  << " size " << image_w << "x" << image_h << "\n";
    } else {
        std::cout << "picture " << image_error << "\n";
    }
    if (!have_visual) {
        std::cout << "m3gunship FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < body_min || stats.count > body_max || center > center_max || stats.box > box_max) {
        std::cout << "m3gunship FAIL picture\n";
        return 1;
    }
    std::cout << "m3gunship PASS\n";
    failure << "The gunship self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace.\n";
    return 0;
}

int gate_m3plane(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double goal_x = 0;
    double goal_z = 0;
    double early_at = 0;
    double early_min = 0;
    double early_max = 0;
    double done_at = 0;
    double arrive_elmos = 0;
    double speed_min = 0;
    double bank_at = 0;
    double bank_min = 0;
    double cruise = 0;
    double alt_slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "plane_start_x", start_x) ||
        !toml_number(tolerance_text, "plane_start_z", start_z) || !toml_number(tolerance_text, "plane_goal_x", goal_x) ||
        !toml_number(tolerance_text, "plane_goal_z", goal_z) || !toml_number(tolerance_text, "plane_early_at", early_at) ||
        !toml_number(tolerance_text, "plane_early_min", early_min) ||
        !toml_number(tolerance_text, "plane_early_max", early_max) ||
        !toml_number(tolerance_text, "plane_done_at", done_at) ||
        !toml_number(tolerance_text, "plane_arrive_elmos", arrive_elmos) ||
        !toml_number(tolerance_text, "plane_speed_min", speed_min) ||
        !toml_number(tolerance_text, "plane_bank_at", bank_at) || !toml_number(tolerance_text, "plane_bank_min", bank_min) ||
        !toml_number(tolerance_text, "plane_cruise", cruise) || !toml_number(tolerance_text, "plane_alt_slop", alt_slop)) {
        std::cerr << "gate: m3plane tolerances are missing\n";
        return 2;
    }
    double body_min = 0;
    double body_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "plane_body_min", body_min) &&
                             toml_number(tolerance_text, "plane_body_max", body_max) &&
                             toml_number(tolerance_text, "plane_shot_w", shot_w) &&
                             toml_number(tolerance_text, "plane_shot_h", shot_h) &&
                             toml_number(tolerance_text, "plane_center_max", center_max) &&
                             toml_number(tolerance_text, "plane_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m3plane/move_plane.jsonl";
    const std::string picture_path = options.actual + "/m3plane/craft-end.ppm";
    std::vector<ShipSample> samples;
    const bool trace_ok = read_ship_samples(trace_path, samples);
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    const ShipSample* early = ship_at(samples, static_cast<int>(early_at));
    const ShipSample* done = ship_at(samples, static_cast<int>(done_at));
    const ShipSample* banked = ship_at(samples, static_cast<int>(bank_at));
    bool def_ok = trace_ok;
    bool alt_ok = trace_ok;
    bool speed_ok = false;
    int fast_samples = 0;
    double slowest = 0;
    double worst_alt = 0;
    for (const ShipSample& sample : samples) {
        if (sample.def != "armfig") {
            def_ok = false;
        }
        const double surface = sample.gy > 0 ? sample.gy : 0;
        const double alt = std::fabs(sample.y - surface - cruise);
        if (alt > worst_alt) {
            worst_alt = alt;
        }
        if (alt > alt_slop) {
            alt_ok = false;
        }
        if (sample.frame >= static_cast<int>(early_at)) {
            ++fast_samples;
            if (fast_samples == 1 || sample.spd < slowest) {
                slowest = sample.spd;
            }
            if (fast_samples == 1) {
                speed_ok = sample.spd >= speed_min;
            } else if (sample.spd < speed_min) {
                speed_ok = false;
            }
        }
    }
    if (fast_samples == 0) {
        speed_ok = false;
    }
    double early_distance = 0;
    bool early_ok = false;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance >= early_min && early_distance <= early_max;
    }
    double done_distance = 0;
    bool arrived = false;
    if (done != nullptr) {
        done_distance = distance(done->x, done->z, goal_x, goal_z);
        arrived = done_distance <= arrive_elmos && done->spd >= speed_min;
    }
    bool bank_ok = false;
    double bank_abs = 0;
    if (banked != nullptr) {
        bank_abs = std::fabs(banked->bank);
        bank_ok = bank_abs >= bank_min && banked->spd >= speed_min;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const int team_window = image_ok ? window_team_pixels(rgb, image_w, image_h) : 0;
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# airplane move\n\n";
    failure << "Scenario: move_plane\n\n";
    failure << "Entity: armfig\n\n";
    failure << "Field: speed, cruise, and bank\n\n";
    if (early != nullptr) {
        failure << "Frame " << early->frame << " is " << early_distance << " elmos from the start. speed " << early->spd
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(early_at) << ".\n\n";
    }
    if (done != nullptr) {
        failure << "Frame " << done->frame << " is " << done_distance << " elmos from the goal. y " << done->y
                << " gy " << done->gy << " speed " << done->spd << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(done_at) << ".\n\n";
    }
    if (banked != nullptr) {
        failure << "Frame " << banked->frame << " bank " << banked->bank << " speed " << banked->spd << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(bank_at) << ".\n\n";
    }
    failure << "Slowest speed after the early frame: " << slowest << " (floor " << speed_min << ")\n\n";
    failure << "Worst altitude error: " << worst_alt << " (slop " << alt_slop << ")\n\n";
    if (stats_ok) {
        failure << "Dark pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    }
    failure << "Window team pixels: " << team_window << "\n\n";

    if (!trace_ok || !def_ok) {
        std::cout << "m3plane FAIL trace\n";
        return 1;
    }
    if (!early_ok) {
        std::cout << "early distance " << early_distance << "\n";
        std::cout << "m3plane FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "arrive distance " << done_distance << "\n";
        std::cout << "m3plane FAIL arrive\n";
        return 1;
    }
    if (!speed_ok) {
        std::cout << "speed " << slowest << "\n";
        std::cout << "m3plane FAIL speed\n";
        return 1;
    }
    if (!alt_ok) {
        std::cout << "altitude " << worst_alt << "\n";
        std::cout << "m3plane FAIL altitude\n";
        return 1;
    }
    if (!bank_ok) {
        std::cout << "bank " << bank_abs << "\n";
        std::cout << "m3plane FAIL bank\n";
        return 1;
    }
    std::cout << "early distance " << early_distance << "\n";
    std::cout << "arrive distance " << done_distance << "\n";
    std::cout << "speed " << slowest << " bank " << bank_abs << " altitude " << worst_alt << "\n";
    if (image_ok) {
        std::cout << "window team " << team_window << " dark " << (stats_ok ? stats.count : 0) << " center " << center
                  << " box " << (stats_ok ? stats.box : 0) << " size " << image_w << "x" << image_h << "\n";
    } else {
        std::cout << "picture " << image_error << "\n";
    }
    if (!have_visual) {
        std::cout << "m3plane FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < body_min || stats.count > body_max || center > center_max || stats.box > box_max) {
        std::cout << "m3plane FAIL picture\n";
        return 1;
    }
    std::cout << "m3plane PASS\n";
    failure << "The airplane self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace.\n";
    return 0;
}

struct CobSample {
    int frame = -1;
    double lwheel_x = 0;
    double rwheel_x = 0;
    int spins = 0;
    int smoke1_hidden = 0;
};

bool read_cob_samples(const std::string& path, std::vector<CobSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        double frame = 0;
        double left = 0;
        double right = 0;
        double spins = 0;
        double hidden = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(line, "lwheel_x", left) ||
            !json_number_field(line, "rwheel_x", right) || !json_number_field(line, "spins", spins) ||
            !json_number_field(line, "smoke1_hidden", hidden)) {
            continue;
        }
        CobSample sample;
        sample.frame = static_cast<int>(frame);
        sample.lwheel_x = left;
        sample.rwheel_x = right;
        sample.spins = static_cast<int>(spins);
        sample.smoke1_hidden = static_cast<int>(hidden);
        out.push_back(sample);
    }
    return !out.empty();
}

const CobSample* cob_at(const std::vector<CobSample>& samples, int frame) {
    for (const CobSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

// Speed starts at 0. Each integrate adds cob_spin_accel to the per-second speed, capped at cob_spin_top,
// then adds that speed times pi/32768/30 to the angle. The same sum the wheel script encodes as 910 and 20020.
double cob_wheel_angle(int spins, double accel, double top) {
    constexpr double kPi = 3.141592653589793;
    double speed = 0;
    double angle = 0;
    for (int i = 0; i < spins; ++i) {
        speed += accel;
        if (speed > top) {
            speed = top;
        }
        angle += speed * kPi / 32768.0 / 30.0;
    }
    return angle;
}

int gate_m3cob(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double frame_at = 0;
    double spins_min = 0;
    double wheel_slop = 0;
    double spin_accel = 0;
    double spin_top = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "cob_frame", frame_at) ||
        !toml_number(tolerance_text, "cob_spins_min", spins_min) ||
        !toml_number(tolerance_text, "cob_wheel_slop", wheel_slop) ||
        !toml_number(tolerance_text, "cob_spin_accel", spin_accel) ||
        !toml_number(tolerance_text, "cob_spin_top", spin_top)) {
        std::cerr << "gate: m3cob tolerances are missing\n";
        return 2;
    }
    double body_min = 0;
    double body_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "cob_body_min", body_min) &&
                             toml_number(tolerance_text, "cob_body_max", body_max) &&
                             toml_number(tolerance_text, "cob_shot_w", shot_w) &&
                             toml_number(tolerance_text, "cob_shot_h", shot_h) &&
                             toml_number(tolerance_text, "cob_center_max", center_max) &&
                             toml_number(tolerance_text, "cob_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string piece_path = options.actual + "/m3cob/pieces.jsonl";
    const std::string picture_path = options.actual + "/m3cob/tank-end.ppm";
    std::vector<CobSample> samples;
    const bool pieces_ok = read_cob_samples(piece_path, samples);
    const CobSample* sample = cob_at(samples, static_cast<int>(frame_at));
    const double expected = sample != nullptr ? cob_wheel_angle(sample->spins, spin_accel, spin_top) : 0;
    const double left_err = sample != nullptr ? std::fabs(shortest_arc(expected, sample->lwheel_x)) : 0;
    const double right_err = sample != nullptr ? std::fabs(shortest_arc(expected, sample->rwheel_x)) : 0;
    const double pair_err =
        sample != nullptr ? std::fabs(shortest_arc(sample->lwheel_x, sample->rwheel_x)) : 0;
    const bool spins_ok = sample != nullptr && sample->spins >= static_cast<int>(spins_min);
    const bool angle_ok = sample != nullptr && left_err <= wheel_slop && right_err <= wheel_slop && pair_err <= wheel_slop;
    const bool smoke_ok = sample != nullptr && sample->smoke1_hidden >= 1;

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const int team_window = image_ok ? window_team_pixels(rgb, image_w, image_h) : 0;
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# cob wheel\n\n";
    failure << "Scenario: move_tank\n\n";
    failure << "Entity: armbull\n\n";
    failure << "Field: lwheel and rwheel X spin\n\n";
    if (sample != nullptr) {
        failure << "Frame " << sample->frame << " spins " << sample->spins << " expected " << expected << " left "
                << sample->lwheel_x << " right " << sample->rwheel_x << "\n\n";
        failure << "Wrapped error left " << left_err << " right " << right_err << " pair " << pair_err << " slop "
                << wheel_slop << "\n\n";
        failure << "smoke1 hidden " << sample->smoke1_hidden << "\n\n";
    } else {
        failure << "No cob sample at frame " << static_cast<int>(frame_at) << ".\n\n";
    }
    if (stats_ok) {
        failure << "Dark pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    }
    failure << "Window team pixels: " << team_window << "\n\n";

    if (!pieces_ok || sample == nullptr) {
        std::cout << "m3cob FAIL sample\n";
        return 1;
    }
    if (!spins_ok) {
        std::cout << "spins " << sample->spins << "\n";
        std::cout << "m3cob FAIL spins\n";
        return 1;
    }
    if (!angle_ok) {
        std::cout << "wheel left " << left_err << " right " << right_err << " pair " << pair_err << "\n";
        std::cout << "m3cob FAIL wheel\n";
        return 1;
    }
    if (!smoke_ok) {
        std::cout << "smoke1 hidden " << sample->smoke1_hidden << "\n";
        std::cout << "m3cob FAIL smoke\n";
        return 1;
    }
    std::cout << "spins " << sample->spins << " wheel " << left_err << " " << right_err << " pair " << pair_err
              << " smoke " << sample->smoke1_hidden << "\n";
    if (image_ok) {
        std::cout << "window team " << team_window << " dark " << (stats_ok ? stats.count : 0) << " center " << center
                  << " box " << (stats_ok ? stats.box : 0) << " size " << image_w << "x" << image_h << "\n";
    } else {
        std::cout << "picture " << image_error << "\n";
    }
    if (!have_visual) {
        std::cout << "m3cob FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < body_min || stats.count > body_max || center > center_max || stats.box > box_max) {
        std::cout << "m3cob FAIL picture\n";
        return 1;
    }
    std::cout << "m3cob PASS\n";
    failure << "The cob self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace.\n";
    return 0;
}

int gate_m3htank(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double start_x = 0;
    double start_z = 0;
    double wade_x = 0;
    double wade_z = 0;
    double deep_x = 0;
    double deep_z = 0;
    double early_at = 0;
    double early_min = 0;
    double early_max = 0;
    double done_at = 0;
    double stay_at = 0;
    double arrive_elmos = 0;
    double gy_min = 0;
    double gy_max = 0;
    double y_slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "htank_start_x", start_x) ||
        !toml_number(tolerance_text, "htank_start_z", start_z) || !toml_number(tolerance_text, "htank_wade_x", wade_x) ||
        !toml_number(tolerance_text, "htank_wade_z", wade_z) || !toml_number(tolerance_text, "htank_deep_x", deep_x) ||
        !toml_number(tolerance_text, "htank_deep_z", deep_z) || !toml_number(tolerance_text, "htank_early_at", early_at) ||
        !toml_number(tolerance_text, "htank_early_min", early_min) ||
        !toml_number(tolerance_text, "htank_early_max", early_max) || !toml_number(tolerance_text, "htank_done_at", done_at) ||
        !toml_number(tolerance_text, "htank_stay_at", stay_at) ||
        !toml_number(tolerance_text, "htank_arrive_elmos", arrive_elmos) ||
        !toml_number(tolerance_text, "htank_gy_min", gy_min) || !toml_number(tolerance_text, "htank_gy_max", gy_max) ||
        !toml_number(tolerance_text, "htank_y_slop", y_slop)) {
        std::cerr << "gate: m3htank tolerances are missing\n";
        return 2;
    }
    double body_min = 0;
    double body_max = 0;
    double shot_w = 0;
    double shot_h = 0;
    double center_max = 0;
    double box_max = 0;
    const bool have_visual = toml_number(tolerance_text, "htank_body_min", body_min) &&
                             toml_number(tolerance_text, "htank_body_max", body_max) &&
                             toml_number(tolerance_text, "htank_shot_w", shot_w) &&
                             toml_number(tolerance_text, "htank_shot_h", shot_h) &&
                             toml_number(tolerance_text, "htank_center_max", center_max) &&
                             toml_number(tolerance_text, "htank_box_max", box_max);

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m3htank/move_htank.jsonl";
    const std::string picture_path = options.actual + "/m3htank/tank-end.ppm";
    std::vector<ShipSample> samples;
    const bool trace_ok = read_ship_samples(trace_path, samples);
    auto distance = [](double ax, double az, double bx, double bz) {
        const double dx = ax - bx;
        const double dz = az - bz;
        return std::sqrt(dx * dx + dz * dz);
    };
    const ShipSample* early = ship_at(samples, static_cast<int>(early_at));
    const ShipSample* done = ship_at(samples, static_cast<int>(done_at));
    bool def_ok = trace_ok;
    bool shelf_ok = false;
    bool stay_ok = false;
    int shelf_samples = 0;
    int stay_samples = 0;
    for (const ShipSample& sample : samples) {
        if (sample.def != "armbull") {
            def_ok = false;
        }
        const bool on_shelf = sample.frame == static_cast<int>(done_at) || sample.frame >= static_cast<int>(stay_at);
        if (!on_shelf) {
            continue;
        }
        ++shelf_samples;
        const bool depth = sample.gy >= gy_min && sample.gy <= gy_max;
        const bool planted = std::fabs(sample.y - sample.gy) <= y_slop;
        if (shelf_samples == 1) {
            shelf_ok = depth && planted;
        } else if (!depth || !planted) {
            shelf_ok = false;
        }
        if (sample.frame >= static_cast<int>(stay_at)) {
            ++stay_samples;
            const double from_wade = distance(sample.x, sample.z, wade_x, wade_z);
            const double from_deep = distance(sample.x, sample.z, deep_x, deep_z);
            const bool held = from_wade <= arrive_elmos && from_deep > arrive_elmos;
            if (stay_samples == 1) {
                stay_ok = held;
            } else if (!held) {
                stay_ok = false;
            }
        }
    }
    if (shelf_samples == 0) {
        shelf_ok = false;
    }
    double early_distance = 0;
    bool early_ok = false;
    if (early != nullptr) {
        early_distance = distance(early->x, early->z, start_x, start_z);
        early_ok = early_distance >= early_min && early_distance <= early_max;
    }
    double done_distance = 0;
    bool arrived = false;
    if (done != nullptr) {
        done_distance = distance(done->x, done->z, wade_x, wade_z);
        arrived = done_distance <= arrive_elmos;
    }

    int image_w = 0;
    int image_h = 0;
    std::vector<uint8_t> rgb;
    std::string image_error;
    const bool image_ok = read_ppm(picture_path, image_w, image_h, rgb, image_error);
    HullStats stats;
    const bool stats_ok = image_ok && hull_stats(rgb, image_w, image_h, stats);
    const int team_window = image_ok ? window_team_pixels(rgb, image_w, image_h) : 0;
    const double center = stats_ok ? std::max(std::fabs(stats.cx - 0.5), std::fabs(stats.cy - 0.5)) : 0;

    failure << "# tank class\n\n";
    failure << "Scenario: move_htank\n\n";
    failure << "Entity: armbull\n\n";
    failure << "Field: HTANK4 wade and deep-water refusal\n\n";
    if (early != nullptr) {
        failure << "Frame " << early->frame << " is " << early_distance << " elmos from the start.\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(early_at) << ".\n\n";
    }
    if (done != nullptr) {
        failure << "Frame " << done->frame << " is " << done_distance << " elmos from the shelf. y " << done->y
                << " gy " << done->gy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(done_at) << ".\n\n";
    }
    failure << "Stay samples: " << stay_samples << "\n\n";
    if (stats_ok) {
        failure << "Dark pixels: " << stats.count << " center " << center << " box " << stats.box << "\n\n";
    }
    failure << "Window team pixels: " << team_window << "\n\n";

    if (!trace_ok || !def_ok) {
        std::cout << "m3htank FAIL trace\n";
        return 1;
    }
    if (!early_ok) {
        std::cout << "early distance " << early_distance << "\n";
        std::cout << "m3htank FAIL early\n";
        return 1;
    }
    if (!arrived) {
        std::cout << "arrive distance " << done_distance << "\n";
        std::cout << "m3htank FAIL arrive\n";
        return 1;
    }
    if (!shelf_ok) {
        std::cout << "gy " << (done != nullptr ? done->gy : 0) << " y " << (done != nullptr ? done->y : 0) << "\n";
        std::cout << "m3htank FAIL shelf\n";
        return 1;
    }
    if (!stay_ok) {
        std::cout << "m3htank FAIL deep\n";
        return 1;
    }
    std::cout << "early distance " << early_distance << "\n";
    std::cout << "arrive distance " << done_distance << " gy " << (done != nullptr ? done->gy : 0) << "\n";
    std::cout << "stay samples " << stay_samples << "\n";
    if (image_ok) {
        std::cout << "window team " << team_window << " dark " << (stats_ok ? stats.count : 0) << " center " << center
                  << " box " << (stats_ok ? stats.box : 0) << " size " << image_w << "x" << image_h << "\n";
    } else {
        std::cout << "picture " << image_error << "\n";
    }
    if (!have_visual) {
        std::cout << "m3htank FAIL visual bar not set\n";
        return 2;
    }
    if (!image_ok || image_w != static_cast<int>(shot_w) || image_h != static_cast<int>(shot_h) || !stats_ok ||
        stats.count < body_min || stats.count > body_max || center > center_max || stats.box > box_max) {
        std::cout << "m3htank FAIL picture\n";
        return 1;
    }
    std::cout << "m3htank PASS\n";
    failure << "The tank-class self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a "
               "Recoil trace.\n";
    return 0;
}

struct EcoSample {
    int frame = -1;
    double metal = 0;
    double energy = 0;
    double metal_storage = 0;
    double energy_storage = 0;
    double metal_income = 0;
    double energy_income = 0;
    std::string def;
};

bool read_eco_samples(const std::string& path, std::vector<EcoSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto units = line.find("\"units\"");
        const auto unit_open = units == std::string::npos ? std::string::npos : line.find('{', units);
        const auto unit_close = unit_open == std::string::npos ? std::string::npos : line.find('}', unit_open);
        const auto teams = line.find("\"teams\"");
        const auto team_open = teams == std::string::npos ? std::string::npos : line.find('{', teams);
        const auto team_close = team_open == std::string::npos ? std::string::npos : line.find('}', team_open);
        if (unit_open == std::string::npos || unit_close == std::string::npos || team_open == std::string::npos ||
            team_close == std::string::npos) {
            continue;
        }
        const std::string unit = line.substr(unit_open, unit_close - unit_open + 1);
        const std::string team = line.substr(team_open, team_close - team_open + 1);
        EcoSample sample;
        double frame = 0;
        if (!json_string_field(unit, "def", sample.def) || !json_number_field(line, "f", frame) ||
            !json_number_field(team, "m", sample.metal) || !json_number_field(team, "e", sample.energy) ||
            !json_number_field(team, "ms", sample.metal_storage) ||
            !json_number_field(team, "es", sample.energy_storage) ||
            !json_number_field(team, "mi", sample.metal_income) || !json_number_field(team, "ei", sample.energy_income)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        out.push_back(sample);
    }
    return !out.empty();
}

const EcoSample* eco_at(const std::vector<EcoSample>& samples, int frame) {
    for (const EcoSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

double eco_stock(double start, double income, double storage, double frame, double hz) {
    const double grown = start + income * frame / hz;
    if (grown > storage) {
        return storage;
    }
    if (grown < 0) {
        return 0;
    }
    return grown;
}

int gate_m4eco(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double income_metal = 0;
    double income_energy = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "eco_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "eco_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "eco_start_metal", start_metal) ||
        !toml_number(tolerance_text, "eco_start_energy", start_energy) ||
        !toml_number(tolerance_text, "eco_income_metal", income_metal) ||
        !toml_number(tolerance_text, "eco_income_energy", income_energy) ||
        !toml_number(tolerance_text, "eco_store_metal", store_metal) ||
        !toml_number(tolerance_text, "eco_store_energy", store_energy) ||
        !toml_number(tolerance_text, "eco_sim_hz", sim_hz) || !toml_number(tolerance_text, "eco_slop", slop) ||
        !(sim_hz > 0)) {
        std::cerr << "gate: m4eco tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m4eco/eco_team.jsonl";
    std::vector<EcoSample> samples;
    const bool trace_ok = read_eco_samples(trace_path, samples);
    const EcoSample* fill = eco_at(samples, static_cast<int>(fill_frame));
    const EcoSample* hold = eco_at(samples, static_cast<int>(hold_frame));
    const double fill_metal = eco_stock(start_metal, income_metal, store_metal, fill_frame, sim_hz);
    const double fill_energy = eco_stock(start_energy, income_energy, store_energy, fill_frame, sim_hz);
    const double hold_metal = eco_stock(start_metal, income_metal, store_metal, hold_frame, sim_hz);
    const double hold_energy = eco_stock(start_energy, income_energy, store_energy, hold_frame, sim_hz);
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    const bool fill_ok = fill != nullptr && fill->def == "armcom" && near(fill->metal, fill_metal) &&
                         near(fill->energy, fill_energy) && near(fill->metal_storage, store_metal) &&
                         near(fill->energy_storage, store_energy) && near(fill->metal_income, income_metal) &&
                         near(fill->energy_income, income_energy);
    const bool hold_ok = hold != nullptr && hold->def == "armcom" && near(hold->metal, hold_metal) &&
                         near(hold->energy, hold_energy) && near(hold->metal_storage, store_metal) &&
                         near(hold->energy_storage, store_energy) && near(hold->metal_income, income_metal) &&
                         near(hold->energy_income, income_energy);

    failure << "# team resources\n\n";
    failure << "Scenario: eco_team\n\n";
    failure << "Entity: armcom\n\n";
    failure << "Field: metal and energy stock\n\n";
    if (fill != nullptr) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
        failure << "Storage " << hold->metal_storage << " " << hold->energy_storage << " income " << hold->metal_income
                << " " << hold->energy_income << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4eco FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "store " << hold->metal_storage << " " << hold->energy_storage << " income " << hold->metal_income
              << " " << hold->energy_income << "\n";
    if (!fill_ok) {
        std::cout << "m4eco FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4eco FAIL hold\n";
        return 1;
    }
    std::cout << "m4eco PASS\n";
    failure << "The team-resource self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a "
               "Recoil trace. The M4 milestone stays red until mex income, construction, and reclaim are gated.\n";
    return 0;
}

struct MexUnit {
    std::string def;
    double x = 0;
    double z = 0;
};

struct MexClaim {
    std::string def;
    double x = 0;
    double z = 0;
    double depth = 0;
    double scale = 0;
    double raw = 0;
    double rate = 0;
    double squares = 0;
};

struct MexSample {
    int frame = -1;
    double metal = 0;
    double energy = 0;
    double metal_storage = 0;
    double energy_storage = 0;
    double metal_income = 0;
    double energy_income = 0;
    double mex_income = 0;
    double energy_upkeep = 0;
    std::vector<MexUnit> units;
    std::vector<MexClaim> mex;
};

bool json_objects(const std::string& line, const char* key, std::vector<std::string>& out) {
    const std::string pattern = std::string("\"") + key + "\":[";
    const auto at = line.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = at + pattern.size(); i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                out.push_back(line.substr(start, i - start + 1));
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return !out.empty();
}

bool read_mex_samples(const std::string& path, std::vector<MexSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> teams;
        std::vector<std::string> mexes;
        if (!json_objects(line, "units", units) || !json_objects(line, "teams", teams) ||
            !json_objects(line, "mex", mexes)) {
            continue;
        }
        MexSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(teams[0], "m", sample.metal) ||
            !json_number_field(teams[0], "e", sample.energy) || !json_number_field(teams[0], "ms", sample.metal_storage) ||
            !json_number_field(teams[0], "es", sample.energy_storage) ||
            !json_number_field(teams[0], "mi", sample.metal_income) ||
            !json_number_field(teams[0], "ei", sample.energy_income) ||
            !json_number_field(teams[0], "mx", sample.mex_income) ||
            !json_number_field(teams[0], "eu", sample.energy_upkeep)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        bool units_ok = true;
        for (const std::string& object : units) {
            MexUnit unit;
            double x = 0;
            double z = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z)) {
                units_ok = false;
                break;
            }
            unit.x = x;
            unit.z = z;
            sample.units.push_back(unit);
        }
        bool mex_ok = true;
        for (const std::string& object : mexes) {
            MexClaim claim;
            double x = 0;
            double z = 0;
            double depth = 0;
            double scale = 0;
            double raw = 0;
            double rate = 0;
            double squares = 0;
            if (!json_string_field(object, "def", claim.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z) || !json_number_field(object, "depth", depth) ||
                !json_number_field(object, "scale", scale) || !json_number_field(object, "raw", raw) ||
                !json_number_field(object, "rate", rate) || !json_number_field(object, "squares", squares)) {
                mex_ok = false;
                break;
            }
            claim.x = x;
            claim.z = z;
            claim.depth = depth;
            claim.scale = scale;
            claim.raw = raw;
            claim.rate = rate;
            claim.squares = squares;
            sample.mex.push_back(claim);
        }
        if (!units_ok || !mex_ok) {
            continue;
        }
        out.push_back(sample);
    }
    return !out.empty();
}

const MexSample* mex_at(const std::vector<MexSample>& samples, int frame) {
    for (const MexSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

const MexUnit* mex_unit(const MexSample& sample, const std::string& def, double x, double z, double slop) {
    for (const MexUnit& unit : sample.units) {
        if (unit.def == def && std::fabs(unit.x - x) <= slop && std::fabs(unit.z - z) <= slop) {
            return &unit;
        }
    }
    return nullptr;
}

const MexClaim* mex_claim_at(const MexSample& sample, double x, double z, double slop) {
    for (const MexClaim& claim : sample.mex) {
        if (std::fabs(claim.x - x) <= slop && std::fabs(claim.z - z) <= slop) {
            return &claim;
        }
    }
    return nullptr;
}

int gate_m4mex(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double commander_metal = 0;
    double commander_energy = 0;
    double commander_x = 0;
    double commander_z = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    double depth = 0;
    double scale = 0;
    double const_slop = 0;
    double upkeep = 0;
    double spot_x = 0;
    double spot_z = 0;
    double empty_x = 0;
    double empty_z = 0;
    double spot_raw = 0;
    double empty_raw = 0;
    double squares = 0;
    double pos_slop = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "mex_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "mex_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "mex_start_metal", start_metal) ||
        !toml_number(tolerance_text, "mex_start_energy", start_energy) ||
        !toml_number(tolerance_text, "mex_commander_metal", commander_metal) ||
        !toml_number(tolerance_text, "mex_commander_energy", commander_energy) ||
        !toml_number(tolerance_text, "mex_commander_x", commander_x) ||
        !toml_number(tolerance_text, "mex_commander_z", commander_z) ||
        !toml_number(tolerance_text, "mex_store_metal", store_metal) ||
        !toml_number(tolerance_text, "mex_store_energy", store_energy) ||
        !toml_number(tolerance_text, "mex_sim_hz", sim_hz) || !toml_number(tolerance_text, "mex_slop", slop) ||
        !toml_number(tolerance_text, "mex_depth", depth) || !toml_number(tolerance_text, "mex_scale", scale) ||
        !toml_number(tolerance_text, "mex_const_slop", const_slop) || !toml_number(tolerance_text, "mex_upkeep", upkeep) ||
        !toml_number(tolerance_text, "mex_spot_x", spot_x) || !toml_number(tolerance_text, "mex_spot_z", spot_z) ||
        !toml_number(tolerance_text, "mex_empty_x", empty_x) || !toml_number(tolerance_text, "mex_empty_z", empty_z) ||
        !toml_number(tolerance_text, "mex_spot_raw", spot_raw) || !toml_number(tolerance_text, "mex_empty_raw", empty_raw) ||
        !toml_number(tolerance_text, "mex_squares", squares) || !toml_number(tolerance_text, "mex_pos_slop", pos_slop) ||
        !(sim_hz > 0)) {
        std::cerr << "gate: m4mex tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m4mex/eco_mex.jsonl";
    std::vector<MexSample> samples;
    const bool trace_ok = read_mex_samples(trace_path, samples);
    const MexSample* fill = mex_at(samples, static_cast<int>(fill_frame));
    const MexSample* hold = mex_at(samples, static_cast<int>(hold_frame));
    const double spot_rate = depth * spot_raw * scale;
    const double empty_rate = depth * empty_raw * scale;
    const double mex_rate = spot_rate + empty_rate;
    const double pull = upkeep * 2.0;
    const double fill_metal = eco_stock(start_metal, commander_metal + mex_rate, store_metal, fill_frame, sim_hz);
    const double hold_metal = eco_stock(start_metal, commander_metal + mex_rate, store_metal, hold_frame, sim_hz);
    const double fill_energy = eco_stock(start_energy, commander_energy - pull, store_energy, fill_frame, sim_hz);
    const double hold_energy = eco_stock(start_energy, commander_energy - pull, store_energy, hold_frame, sim_hz);
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    auto tight = [&](double got, double want) { return std::fabs(got - want) <= const_slop; };
    auto claim_ok = [&](const MexClaim* claim, double raw, double rate) {
        return claim != nullptr && claim->def == "armmex" && tight(claim->depth, depth) && tight(claim->scale, scale) &&
               std::fabs(claim->raw - raw) <= 0.5 && std::fabs(claim->squares - squares) <= 0.5 && near(claim->rate, rate);
    };
    auto sample_ok = [&](const MexSample* sample) {
        if (sample == nullptr) {
            return false;
        }
        const MexClaim* spot = mex_claim_at(*sample, spot_x, spot_z, pos_slop);
        const MexClaim* empty = mex_claim_at(*sample, empty_x, empty_z, pos_slop);
        return mex_unit(*sample, "armcom", commander_x, commander_z, pos_slop) != nullptr &&
               mex_unit(*sample, "armmex", spot_x, spot_z, pos_slop) != nullptr &&
               mex_unit(*sample, "armmex", empty_x, empty_z, pos_slop) != nullptr && claim_ok(spot, spot_raw, spot_rate) &&
               claim_ok(empty, empty_raw, empty_rate) && near(sample->metal_income, commander_metal) &&
               near(sample->energy_income, commander_energy) && near(sample->mex_income, mex_rate) &&
               near(sample->energy_upkeep, pull) && near(sample->metal_storage, store_metal) &&
               near(sample->energy_storage, store_energy) && near(sample->metal, sample == fill ? fill_metal : hold_metal) &&
               near(sample->energy, sample == fill ? fill_energy : hold_energy);
    };
    const bool fill_ok = sample_ok(fill);
    const bool hold_ok = sample_ok(hold);

    failure << "# mex income\n\n";
    failure << "Scenario: eco_mex\n\n";
    failure << "Entity: armmex\n\n";
    failure << "Field: metal from the metal map\n\n";
    if (fill != nullptr) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
        failure << "Mex income " << fill->mex_income << " expected " << mex_rate << " upkeep " << fill->energy_upkeep
                << " expected " << pull << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Spot raw " << spot_raw << " empty raw " << empty_raw << " depth " << depth << " scale " << scale
            << "\n\n";

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4mex FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "mex " << fill->mex_income << " expected " << mex_rate << " upkeep " << fill->energy_upkeep
              << " expected " << pull << "\n";
    if (!fill_ok) {
        std::cout << "m4mex FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4mex FAIL hold\n";
        return 1;
    }
    std::cout << "m4mex PASS\n";
    failure << "The mex-income self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a "
               "Recoil trace. The M4 milestone stays red until solar, construction, and reclaim are gated.\n";
    return 0;
}

struct PwrUnit {
    std::string def;
    double x = 0;
    double z = 0;
};

struct PwrGen {
    std::string def;
    double x = 0;
    double z = 0;
    double upkeep = 0;
    double make = 0;
    double cap = 0;
    double wind = 0;
    double rate = 0;
};

struct PwrSample {
    int frame = -1;
    double metal = 0;
    double energy = 0;
    double metal_storage = 0;
    double energy_storage = 0;
    double metal_income = 0;
    double energy_income = 0;
    double mex_income = 0;
    double energy_upkeep = 0;
    double solar_income = 0;
    double wind_income = 0;
    std::vector<PwrUnit> units;
    std::vector<PwrGen> gens;
};

bool read_pwr_samples(const std::string& path, std::vector<PwrSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> teams;
        std::vector<std::string> gens;
        if (!json_objects(line, "units", units) || !json_objects(line, "teams", teams) ||
            !json_objects(line, "gen", gens)) {
            continue;
        }
        PwrSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(teams[0], "m", sample.metal) ||
            !json_number_field(teams[0], "e", sample.energy) || !json_number_field(teams[0], "ms", sample.metal_storage) ||
            !json_number_field(teams[0], "es", sample.energy_storage) ||
            !json_number_field(teams[0], "mi", sample.metal_income) ||
            !json_number_field(teams[0], "ei", sample.energy_income) ||
            !json_number_field(teams[0], "mx", sample.mex_income) ||
            !json_number_field(teams[0], "eu", sample.energy_upkeep) ||
            !json_number_field(teams[0], "so", sample.solar_income) ||
            !json_number_field(teams[0], "wi", sample.wind_income)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        bool units_ok = true;
        for (const std::string& object : units) {
            PwrUnit unit;
            double x = 0;
            double z = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z)) {
                units_ok = false;
                break;
            }
            unit.x = x;
            unit.z = z;
            sample.units.push_back(unit);
        }
        bool gens_ok = true;
        for (const std::string& object : gens) {
            PwrGen gen;
            double x = 0;
            double z = 0;
            double upkeep = 0;
            double make = 0;
            double cap = 0;
            double wind = 0;
            double rate = 0;
            if (!json_string_field(object, "def", gen.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z) || !json_number_field(object, "upkeep", upkeep) ||
                !json_number_field(object, "make", make) || !json_number_field(object, "cap", cap) ||
                !json_number_field(object, "wind", wind) || !json_number_field(object, "rate", rate)) {
                gens_ok = false;
                break;
            }
            gen.x = x;
            gen.z = z;
            gen.upkeep = upkeep;
            gen.make = make;
            gen.cap = cap;
            gen.wind = wind;
            gen.rate = rate;
            sample.gens.push_back(gen);
        }
        if (!units_ok || !gens_ok) {
            continue;
        }
        out.push_back(sample);
    }
    return !out.empty();
}

const PwrSample* pwr_at(const std::vector<PwrSample>& samples, int frame) {
    for (const PwrSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

const PwrUnit* pwr_unit(const PwrSample& sample, const std::string& def, double x, double z, double slop) {
    for (const PwrUnit& unit : sample.units) {
        if (unit.def == def && std::fabs(unit.x - x) <= slop && std::fabs(unit.z - z) <= slop) {
            return &unit;
        }
    }
    return nullptr;
}

const PwrGen* pwr_gen_at(const PwrSample& sample, double x, double z, double slop) {
    for (const PwrGen& gen : sample.gens) {
        if (std::fabs(gen.x - x) <= slop && std::fabs(gen.z - z) <= slop) {
            return &gen;
        }
    }
    return nullptr;
}

int gate_m4pwr(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double commander_metal = 0;
    double commander_energy = 0;
    double commander_x = 0;
    double commander_z = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    double const_slop = 0;
    double pos_slop = 0;
    double energy_scale = 0;
    double solar_x = 0;
    double solar_z = 0;
    double solar_upkeep = 0;
    double wind_x = 0;
    double wind_z = 0;
    double wind_cap = 0;
    double wind_min = 0;
    double wind_max = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "pwr_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "pwr_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "pwr_start_metal", start_metal) ||
        !toml_number(tolerance_text, "pwr_start_energy", start_energy) ||
        !toml_number(tolerance_text, "pwr_commander_metal", commander_metal) ||
        !toml_number(tolerance_text, "pwr_commander_energy", commander_energy) ||
        !toml_number(tolerance_text, "pwr_commander_x", commander_x) ||
        !toml_number(tolerance_text, "pwr_commander_z", commander_z) ||
        !toml_number(tolerance_text, "pwr_store_metal", store_metal) ||
        !toml_number(tolerance_text, "pwr_store_energy", store_energy) ||
        !toml_number(tolerance_text, "pwr_sim_hz", sim_hz) || !toml_number(tolerance_text, "pwr_slop", slop) ||
        !toml_number(tolerance_text, "pwr_const_slop", const_slop) ||
        !toml_number(tolerance_text, "pwr_pos_slop", pos_slop) ||
        !toml_number(tolerance_text, "pwr_energy_scale", energy_scale) ||
        !toml_number(tolerance_text, "pwr_solar_x", solar_x) || !toml_number(tolerance_text, "pwr_solar_z", solar_z) ||
        !toml_number(tolerance_text, "pwr_solar_upkeep", solar_upkeep) ||
        !toml_number(tolerance_text, "pwr_wind_x", wind_x) || !toml_number(tolerance_text, "pwr_wind_z", wind_z) ||
        !toml_number(tolerance_text, "pwr_wind_cap", wind_cap) || !toml_number(tolerance_text, "pwr_wind_min", wind_min) ||
        !toml_number(tolerance_text, "pwr_wind_max", wind_max) || !(sim_hz > 0)) {
        std::cerr << "gate: m4pwr tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m4pwr/eco_power.jsonl";
    std::vector<PwrSample> samples;
    const bool trace_ok = read_pwr_samples(trace_path, samples);
    const PwrSample* fill = pwr_at(samples, static_cast<int>(fill_frame));
    const PwrSample* hold = pwr_at(samples, static_cast<int>(hold_frame));
    const double strength = (wind_min + wind_max) * 0.5;
    const double scaled_cap = wind_cap * energy_scale;
    const double wind_rate = strength < scaled_cap ? strength : scaled_cap;
    const double solar_make = solar_upkeep < 0 ? -solar_upkeep * energy_scale : 0;
    const double commander_energy_rate = commander_energy * energy_scale;
    const double fill_metal = eco_stock(start_metal, commander_metal, store_metal, fill_frame, sim_hz);
    const double hold_metal = eco_stock(start_metal, commander_metal, store_metal, hold_frame, sim_hz);
    const double energy_rate = commander_energy_rate + solar_make + wind_rate;
    const double fill_energy = eco_stock(start_energy, energy_rate, store_energy, fill_frame, sim_hz);
    const double hold_energy = eco_stock(start_energy, energy_rate, store_energy, hold_frame, sim_hz);
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    auto tight = [&](double got, double want) { return std::fabs(got - want) <= const_slop; };
    auto sample_ok = [&](const PwrSample* sample) {
        if (sample == nullptr) {
            return false;
        }
        const PwrGen* solar = pwr_gen_at(*sample, solar_x, solar_z, pos_slop);
        const PwrGen* wind = pwr_gen_at(*sample, wind_x, wind_z, pos_slop);
        const bool solar_ok = solar != nullptr && solar->def == "armsolar" && tight(solar->upkeep, solar_upkeep) &&
                              near(solar->make, solar_make) && tight(solar->cap, 0) && tight(solar->wind, strength) &&
                              near(solar->rate, 0);
        const bool wind_ok = wind != nullptr && wind->def == "armwin" && tight(wind->upkeep, 0) && near(wind->make, 0) &&
                             tight(wind->cap, scaled_cap) && tight(wind->wind, strength) && near(wind->rate, wind_rate);
        return pwr_unit(*sample, "armcom", commander_x, commander_z, pos_slop) != nullptr &&
               pwr_unit(*sample, "armsolar", solar_x, solar_z, pos_slop) != nullptr &&
               pwr_unit(*sample, "armwin", wind_x, wind_z, pos_slop) != nullptr && solar_ok && wind_ok &&
               near(sample->metal_income, commander_metal) && near(sample->energy_income, commander_energy_rate) &&
               near(sample->mex_income, 0) && near(sample->energy_upkeep, 0) &&
               near(sample->solar_income, solar_make) && near(sample->wind_income, wind_rate) &&
               near(sample->metal_storage, store_metal) && near(sample->energy_storage, store_energy) &&
               near(sample->metal, sample == fill ? fill_metal : hold_metal) &&
               near(sample->energy, sample == fill ? fill_energy : hold_energy);
    };
    const bool fill_ok = sample_ok(fill);
    const bool hold_ok = sample_ok(hold);

    failure << "# solar and wind\n\n";
    failure << "Scenario: eco_power\n\n";
    failure << "Entity: armsolar, armwin\n\n";
    failure << "Field: energy from a negative upkeep and from the map wind\n\n";
    if (fill != nullptr) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
        failure << "Solar " << fill->solar_income << " expected " << solar_make << " wind " << fill->wind_income
                << " expected " << wind_rate << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Wind range " << wind_min << " " << wind_max << " strength " << strength << " cap " << scaled_cap
            << " solar upkeep " << solar_upkeep << "\n\n";

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4pwr FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "solar " << fill->solar_income << " expected " << solar_make << " wind " << fill->wind_income
              << " expected " << wind_rate << "\n";
    if (!fill_ok) {
        std::cout << "m4pwr FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4pwr FAIL hold\n";
        return 1;
    }
    std::cout << "m4pwr PASS\n";
    failure << "The solar and wind self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a "
               "Recoil trace. The M4 milestone stays red until construction and reclaim are gated.\n";
    return 0;
}

struct NanoUnit {
    std::string def;
    double x = 0;
    double z = 0;
};

struct NanoFrameSample {
    std::string def;
    double x = 0;
    double z = 0;
    double progress = 0;
    double health = 0;
    double build_time = 0;
    double metal_cost = 0;
    double energy_cost = 0;
    double worker_time = 0;
    double build_distance = 0;
    double power_scale = 0;
    double finished = 0;
    double decay = 0;
    double decay_time = 0;
    double decay_speed = 0;
};

struct NanoSample {
    int frame = -1;
    double metal = 0;
    double energy = 0;
    double metal_storage = 0;
    double energy_storage = 0;
    double metal_income = 0;
    double energy_income = 0;
    double mex_income = 0;
    double energy_upkeep = 0;
    double solar_income = 0;
    double wind_income = 0;
    std::vector<NanoUnit> units;
    NanoFrameSample frame_build;
    bool have_frame = false;
};

bool read_nano_samples(const std::string& path, std::vector<NanoSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> teams;
        std::vector<std::string> frames;
        if (!json_objects(line, "units", units) || !json_objects(line, "teams", teams) ||
            !json_objects(line, "nf", frames)) {
            continue;
        }
        NanoSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(teams[0], "m", sample.metal) ||
            !json_number_field(teams[0], "e", sample.energy) || !json_number_field(teams[0], "ms", sample.metal_storage) ||
            !json_number_field(teams[0], "es", sample.energy_storage) ||
            !json_number_field(teams[0], "mi", sample.metal_income) ||
            !json_number_field(teams[0], "ei", sample.energy_income) ||
            !json_number_field(teams[0], "mx", sample.mex_income) ||
            !json_number_field(teams[0], "eu", sample.energy_upkeep) ||
            !json_number_field(teams[0], "so", sample.solar_income) ||
            !json_number_field(teams[0], "wi", sample.wind_income)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        bool units_ok = true;
        for (const std::string& object : units) {
            NanoUnit unit;
            double x = 0;
            double z = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z)) {
                units_ok = false;
                break;
            }
            unit.x = x;
            unit.z = z;
            sample.units.push_back(unit);
        }
        const std::string& built = frames[0];
        double x = 0;
        double z = 0;
        if (!json_string_field(built, "def", sample.frame_build.def) || !json_number_field(built, "x", x) ||
            !json_number_field(built, "z", z) || !json_number_field(built, "bp", sample.frame_build.progress) ||
            !json_number_field(built, "hp", sample.frame_build.health) ||
            !json_number_field(built, "bt", sample.frame_build.build_time) ||
            !json_number_field(built, "mc", sample.frame_build.metal_cost) ||
            !json_number_field(built, "ec", sample.frame_build.energy_cost) ||
            !json_number_field(built, "wk", sample.frame_build.worker_time) ||
            !json_number_field(built, "bd", sample.frame_build.build_distance) ||
            !json_number_field(built, "sc", sample.frame_build.power_scale) ||
            !json_number_field(built, "fin", sample.frame_build.finished) ||
            !json_number_field(built, "cd", sample.frame_build.decay) ||
            !json_number_field(built, "ct", sample.frame_build.decay_time) ||
            !json_number_field(built, "cs", sample.frame_build.decay_speed)) {
            units_ok = false;
        }
        sample.frame_build.x = x;
        sample.frame_build.z = z;
        sample.have_frame = units_ok;
        if (!units_ok) {
            continue;
        }
        out.push_back(sample);
    }
    return !out.empty();
}

const NanoSample* nano_at(const std::vector<NanoSample>& samples, int frame) {
    for (const NanoSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

const NanoUnit* nano_unit(const NanoSample& sample, const std::string& def, double x, double z, double slop) {
    for (const NanoUnit& unit : sample.units) {
        if (unit.def == def && std::fabs(unit.x - x) <= slop && std::fabs(unit.z - z) <= slop) {
            return &unit;
        }
    }
    return nullptr;
}

double snap_build_axis(double click, double footprint) {
    const int size = static_cast<int>(footprint) * 2;
    if ((size & 2) != 0) {
        return std::floor(click / 16.0) * 16.0 + 8.0;
    }
    return std::floor((click + 8.0) / 16.0) * 16.0;
}

int gate_m4nano(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double commander_metal = 0;
    double commander_energy = 0;
    double commander_x = 0;
    double commander_z = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    double const_slop = 0;
    double pos_slop = 0;
    double worker = 0;
    double reach = 0;
    double power_scale = 0;
    double build_time = 0;
    double cost_metal = 0;
    double cost_energy = 0;
    double hp_max = 0;
    double hp_start = 0;
    double foot_x = 0;
    double foot_z = 0;
    double click_x = 0;
    double click_z = 0;
    double decay_on = 0;
    double decay_time = 0;
    double decay_speed = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "nano_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "nano_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "nano_start_metal", start_metal) ||
        !toml_number(tolerance_text, "nano_start_energy", start_energy) ||
        !toml_number(tolerance_text, "nano_commander_metal", commander_metal) ||
        !toml_number(tolerance_text, "nano_commander_energy", commander_energy) ||
        !toml_number(tolerance_text, "nano_commander_x", commander_x) ||
        !toml_number(tolerance_text, "nano_commander_z", commander_z) ||
        !toml_number(tolerance_text, "nano_store_metal", store_metal) ||
        !toml_number(tolerance_text, "nano_store_energy", store_energy) ||
        !toml_number(tolerance_text, "nano_sim_hz", sim_hz) || !toml_number(tolerance_text, "nano_slop", slop) ||
        !toml_number(tolerance_text, "nano_const_slop", const_slop) ||
        !toml_number(tolerance_text, "nano_pos_slop", pos_slop) || !toml_number(tolerance_text, "nano_worker", worker) ||
        !toml_number(tolerance_text, "nano_reach", reach) ||
        !toml_number(tolerance_text, "nano_power_scale", power_scale) ||
        !toml_number(tolerance_text, "nano_build_time", build_time) ||
        !toml_number(tolerance_text, "nano_cost_metal", cost_metal) ||
        !toml_number(tolerance_text, "nano_cost_energy", cost_energy) ||
        !toml_number(tolerance_text, "nano_hp_max", hp_max) || !toml_number(tolerance_text, "nano_hp_start", hp_start) ||
        !toml_number(tolerance_text, "nano_foot_x", foot_x) || !toml_number(tolerance_text, "nano_foot_z", foot_z) ||
        !toml_number(tolerance_text, "nano_click_x", click_x) || !toml_number(tolerance_text, "nano_click_z", click_z) ||
        !toml_number(tolerance_text, "nano_decay_on", decay_on) ||
        !toml_number(tolerance_text, "nano_decay_time", decay_time) ||
        !toml_number(tolerance_text, "nano_decay_speed", decay_speed) || !(sim_hz > 0) || !(build_time > 0)) {
        std::cerr << "gate: m4nano tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m4nano/eco_nano.jsonl";
    std::vector<NanoSample> samples;
    const bool trace_ok = read_nano_samples(trace_path, samples);
    const NanoSample* fill = nano_at(samples, static_cast<int>(fill_frame));
    const NanoSample* hold = nano_at(samples, static_cast<int>(hold_frame));
    const double site_x = snap_build_axis(click_x, foot_x);
    const double site_z = snap_build_axis(click_z, foot_z);
    const double scaled_worker = worker * power_scale;
    const double metal_rate = commander_metal - cost_metal * scaled_worker / build_time;
    const double energy_rate = commander_energy - cost_energy * scaled_worker / build_time;
    auto progress_at = [&](double frame) { return frame * (scaled_worker / sim_hz) / build_time; };
    auto health_at = [&](double frame) { return hp_start + hp_max * progress_at(frame); };
    const double fill_progress = progress_at(fill_frame);
    const double hold_progress = progress_at(hold_frame);
    const double fill_metal = eco_stock(start_metal, metal_rate, store_metal, fill_frame, sim_hz);
    const double hold_metal = eco_stock(start_metal, metal_rate, store_metal, hold_frame, sim_hz);
    const double fill_energy = eco_stock(start_energy, energy_rate, store_energy, fill_frame, sim_hz);
    const double hold_energy = eco_stock(start_energy, energy_rate, store_energy, hold_frame, sim_hz);
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    auto tight = [&](double got, double want) { return std::fabs(got - want) <= const_slop; };
    auto sample_ok = [&](const NanoSample* sample, double want_progress, double want_health, double want_metal,
                         double want_energy) {
        if (sample == nullptr || !sample->have_frame) {
            return false;
        }
        const NanoFrameSample& built = sample->frame_build;
        const bool open = built.def == "armwin" && built.finished == 0 && built.progress > 0 && built.progress < 1;
        return nano_unit(*sample, "armcom", commander_x, commander_z, pos_slop) != nullptr && open &&
               std::fabs(built.x - site_x) <= pos_slop && std::fabs(built.z - site_z) <= pos_slop &&
               tight(built.progress, want_progress) && tight(built.health, want_health) &&
               tight(built.build_time, build_time) && tight(built.metal_cost, cost_metal) &&
               tight(built.energy_cost, cost_energy) && tight(built.worker_time, scaled_worker) &&
               tight(built.build_distance, reach) && tight(built.power_scale, power_scale) &&
               tight(built.decay, decay_on) && tight(built.decay_time, decay_time) &&
               tight(built.decay_speed, decay_speed) && near(sample->metal_income, commander_metal) &&
               near(sample->energy_income, commander_energy) && near(sample->mex_income, 0) &&
               near(sample->energy_upkeep, 0) && near(sample->solar_income, 0) && near(sample->wind_income, 0) &&
               near(sample->metal_storage, store_metal) && near(sample->energy_storage, store_energy) &&
               near(sample->metal, want_metal) && near(sample->energy, want_energy);
    };
    const bool fill_ok = sample_ok(fill, fill_progress, health_at(fill_frame), fill_metal, fill_energy);
    const bool hold_ok = sample_ok(hold, hold_progress, health_at(hold_frame), hold_metal, hold_energy);

    failure << "# nanoframe\n\n";
    failure << "Scenario: eco_nano\n\n";
    failure << "Entity: armwin nanoframe\n\n";
    failure << "Field: build progress and resources spent after income\n\n";
    if (fill != nullptr && fill->have_frame) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
        failure << "Progress " << fill->frame_build.progress << " expected " << fill_progress << " health "
                << fill->frame_build.health << " expected " << health_at(fill_frame) << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_frame) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
        failure << "Progress " << hold->frame_build.progress << " expected " << hold_progress << " health "
                << hold->frame_build.health << " expected " << health_at(hold_frame) << "\n\n";
        failure << "Site " << hold->frame_build.x << " " << hold->frame_build.z << " expected " << site_x << " "
                << site_z << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Build power " << scaled_worker << " per second over " << build_time << ". Metal cost " << cost_metal
            << " energy cost " << cost_energy << ". The frame stays unfinished.\n\n";

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4nano FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "progress " << hold->frame_build.progress << " expected " << hold_progress << " health "
              << hold->frame_build.health << " expected " << health_at(hold_frame) << "\n";
    if (!fill_ok) {
        std::cout << "m4nano FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4nano FAIL hold\n";
        return 1;
    }
    std::cout << "m4nano PASS\n";
    failure << "The nanoframe self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a "
               "Recoil trace. The M4 milestone stays red until the lab and reclaim are gated.\n";
    return 0;
}

int gate_m4lab(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double commander_metal = 0;
    double commander_energy = 0;
    double commander_x = 0;
    double commander_z = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    double const_slop = 0;
    double pos_slop = 0;
    double worker = 0;
    double power_scale = 0;
    double build_time = 0;
    double cost_metal = 0;
    double cost_energy = 0;
    double hp_max = 0;
    double hp_start = 0;
    double pad_x = 0;
    double pad_z = 0;
    double decay_on = 0;
    double decay_time = 0;
    double decay_speed = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "lab_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "lab_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "lab_start_metal", start_metal) ||
        !toml_number(tolerance_text, "lab_start_energy", start_energy) ||
        !toml_number(tolerance_text, "lab_commander_metal", commander_metal) ||
        !toml_number(tolerance_text, "lab_commander_energy", commander_energy) ||
        !toml_number(tolerance_text, "lab_commander_x", commander_x) ||
        !toml_number(tolerance_text, "lab_commander_z", commander_z) ||
        !toml_number(tolerance_text, "lab_store_metal", store_metal) ||
        !toml_number(tolerance_text, "lab_store_energy", store_energy) ||
        !toml_number(tolerance_text, "lab_sim_hz", sim_hz) || !toml_number(tolerance_text, "lab_slop", slop) ||
        !toml_number(tolerance_text, "lab_const_slop", const_slop) ||
        !toml_number(tolerance_text, "lab_pos_slop", pos_slop) || !toml_number(tolerance_text, "lab_worker", worker) ||
        !toml_number(tolerance_text, "lab_power_scale", power_scale) ||
        !toml_number(tolerance_text, "lab_build_time", build_time) ||
        !toml_number(tolerance_text, "lab_cost_metal", cost_metal) ||
        !toml_number(tolerance_text, "lab_cost_energy", cost_energy) ||
        !toml_number(tolerance_text, "lab_hp_max", hp_max) || !toml_number(tolerance_text, "lab_hp_start", hp_start) ||
        !toml_number(tolerance_text, "lab_pad_x", pad_x) || !toml_number(tolerance_text, "lab_pad_z", pad_z) ||
        !toml_number(tolerance_text, "lab_decay_on", decay_on) ||
        !toml_number(tolerance_text, "lab_decay_time", decay_time) ||
        !toml_number(tolerance_text, "lab_decay_speed", decay_speed) || !(sim_hz > 0) || !(build_time > 0)) {
        std::cerr << "gate: m4lab tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const std::string trace_path = options.actual + "/m4lab/build_tank.jsonl";
    std::vector<NanoSample> samples;
    const bool trace_ok = read_nano_samples(trace_path, samples);
    const NanoSample* fill = nano_at(samples, static_cast<int>(fill_frame));
    const NanoSample* hold = nano_at(samples, static_cast<int>(hold_frame));
    const double scaled_worker = worker * power_scale;
    const double metal_rate = commander_metal - cost_metal * scaled_worker / build_time;
    const double energy_rate = commander_energy - cost_energy * scaled_worker / build_time;
    const double fill_progress = fill_frame * (scaled_worker / sim_hz) / build_time;
    const double fill_health = hp_start + hp_max * fill_progress;
    const double fill_metal = eco_stock(start_metal, metal_rate, store_metal, fill_frame, sim_hz);
    const double fill_energy = eco_stock(start_energy, energy_rate, store_energy, fill_frame, sim_hz);
    const double hold_metal = start_metal + commander_metal * hold_frame / sim_hz - cost_metal;
    const double hold_energy = start_energy + commander_energy * hold_frame / sim_hz - cost_energy;
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    auto tight = [&](double got, double want) { return std::fabs(got - want) <= const_slop; };
    auto placed = [&](const NanoSample* sample) {
        if (sample == nullptr) {
            return false;
        }
        bool product_unit = false;
        for (const NanoUnit& unit : sample->units) {
            if (unit.def == "armfav") {
                product_unit = true;
            }
        }
        return sample->units.size() == 2 && !product_unit &&
               nano_unit(*sample, "armcom", commander_x, commander_z, pos_slop) != nullptr &&
               nano_unit(*sample, "armvp", pad_x, pad_z, pos_slop) != nullptr;
    };
    auto costs_ok = [&](const NanoFrameSample& built) {
        return built.def == "armfav" && tight(built.build_time, build_time) && tight(built.metal_cost, cost_metal) &&
               tight(built.energy_cost, cost_energy) && tight(built.worker_time, scaled_worker) &&
               tight(built.power_scale, power_scale) && tight(built.decay, decay_on) &&
               tight(built.decay_time, decay_time) && tight(built.decay_speed, decay_speed) &&
               std::fabs(built.x - pad_x) <= pos_slop && std::fabs(built.z - pad_z) <= pos_slop;
    };
    auto team_ok = [&](const NanoSample& sample) {
        return near(sample.metal_income, commander_metal) && near(sample.energy_income, commander_energy) &&
               near(sample.mex_income, 0) && near(sample.energy_upkeep, 0) && near(sample.solar_income, 0) &&
               near(sample.wind_income, 0) && near(sample.metal_storage, store_metal) &&
               near(sample.energy_storage, store_energy);
    };
    const bool fill_ok = fill != nullptr && fill->have_frame && placed(fill) && costs_ok(fill->frame_build) &&
                         fill->frame_build.finished == 0 && tight(fill->frame_build.progress, fill_progress) &&
                         tight(fill->frame_build.health, fill_health) && team_ok(*fill) &&
                         near(fill->metal, fill_metal) && near(fill->energy, fill_energy);
    const bool hold_ok = hold != nullptr && hold->have_frame && placed(hold) && costs_ok(hold->frame_build) &&
                         hold->frame_build.finished == 1 && tight(hold->frame_build.progress, 1) &&
                         tight(hold->frame_build.health, hp_max) && team_ok(*hold) && near(hold->metal, hold_metal) &&
                         near(hold->energy, hold_energy);

    failure << "# lab\n\n";
    failure << "Scenario: build_tank\n\n";
    failure << "Entity: armfav built by armvp\n\n";
    failure << "Field: factory build progress and resources spent after income\n\n";
    if (fill != nullptr && fill->have_frame) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
        failure << "Progress " << fill->frame_build.progress << " expected " << fill_progress << " health "
                << fill->frame_build.health << " expected " << fill_health << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_frame) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
        failure << "Progress " << hold->frame_build.progress << " expected 1 health " << hold->frame_build.health
                << " expected " << hp_max << " finished " << hold->frame_build.finished << "\n\n";
        failure << "Pad " << hold->frame_build.x << " " << hold->frame_build.z << " expected " << pad_x << " " << pad_z
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Build power " << scaled_worker << " per second over " << build_time << ". Metal cost " << cost_metal
            << " energy cost " << cost_energy
            << ". The yard script is not hosted, so production starts on the order frame. The finished unit stays on "
               "the pad.\n\n";

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4lab FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "progress " << hold->frame_build.progress << " expected 1 health " << hold->frame_build.health
              << " expected " << hp_max << "\n";
    if (!fill_ok) {
        std::cout << "m4lab FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4lab FAIL hold\n";
        return 1;
    }
    std::cout << "m4lab PASS\n";
    failure << "The lab self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace. The M4 milestone stays red until reclaim is gated.\n";
    return 0;
}

struct ReclaimSample {
    int frame = -1;
    double metal = 0;
    double energy = 0;
    double metal_storage = 0;
    double energy_storage = 0;
    double metal_income = 0;
    double energy_income = 0;
    double mex_income = 0;
    double energy_upkeep = 0;
    double solar_income = 0;
    double wind_income = 0;
    std::vector<NanoUnit> units;
    std::string def;
    double x = 0;
    double z = 0;
    double left = 0;
    double wreck_metal = 0;
    double wreck_energy = 0;
    double reclaim_time = 0;
    double reclaim_speed = 0;
    double build_distance = 0;
    double power_scale = 0;
    double reclaim_method = 0;
    double energy_factor = 0;
    double multi = 0;
    double gone = 0;
    bool have_wreck = false;
};

bool read_reclaim_samples(const std::string& path, std::vector<ReclaimSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> teams;
        std::vector<std::string> wrecks;
        if (!json_objects(line, "units", units) || !json_objects(line, "teams", teams) ||
            !json_objects(line, "rk", wrecks)) {
            continue;
        }
        ReclaimSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_number_field(teams[0], "m", sample.metal) ||
            !json_number_field(teams[0], "e", sample.energy) || !json_number_field(teams[0], "ms", sample.metal_storage) ||
            !json_number_field(teams[0], "es", sample.energy_storage) ||
            !json_number_field(teams[0], "mi", sample.metal_income) ||
            !json_number_field(teams[0], "ei", sample.energy_income) ||
            !json_number_field(teams[0], "mx", sample.mex_income) ||
            !json_number_field(teams[0], "eu", sample.energy_upkeep) ||
            !json_number_field(teams[0], "so", sample.solar_income) ||
            !json_number_field(teams[0], "wi", sample.wind_income)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        bool units_ok = true;
        for (const std::string& object : units) {
            NanoUnit unit;
            double x = 0;
            double z = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "x", x) ||
                !json_number_field(object, "z", z)) {
                units_ok = false;
                break;
            }
            unit.x = x;
            unit.z = z;
            sample.units.push_back(unit);
        }
        const std::string& body = wrecks[0];
        double x = 0;
        double z = 0;
        if (!json_string_field(body, "def", sample.def) || !json_number_field(body, "x", x) ||
            !json_number_field(body, "z", z) || !json_number_field(body, "left", sample.left) ||
            !json_number_field(body, "mt", sample.wreck_metal) || !json_number_field(body, "en", sample.wreck_energy) ||
            !json_number_field(body, "rt", sample.reclaim_time) || !json_number_field(body, "rs", sample.reclaim_speed) ||
            !json_number_field(body, "bd", sample.build_distance) || !json_number_field(body, "sc", sample.power_scale) ||
            !json_number_field(body, "rm", sample.reclaim_method) ||
            !json_number_field(body, "ef", sample.energy_factor) || !json_number_field(body, "mr", sample.multi) ||
            !json_number_field(body, "gone", sample.gone)) {
            units_ok = false;
        }
        sample.x = x;
        sample.z = z;
        sample.have_wreck = units_ok;
        if (!units_ok) {
            continue;
        }
        out.push_back(sample);
    }
    return !out.empty();
}

const ReclaimSample* reclaim_at(const std::vector<ReclaimSample>& samples, int frame) {
    for (const ReclaimSample& sample : samples) {
        if (sample.frame == frame) {
            return &sample;
        }
    }
    return nullptr;
}

int gate_m4reclaim(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double hold_frame = 0;
    double start_metal = 0;
    double start_energy = 0;
    double commander_metal = 0;
    double commander_energy = 0;
    double commander_x = 0;
    double commander_z = 0;
    double store_metal = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double slop = 0;
    double const_slop = 0;
    double pos_slop = 0;
    double worker = 0;
    double power_scale = 0;
    double reach = 0;
    double unit_metal = 0;
    double wreck_ratio = 0;
    double feature_energy = 0;
    double reclaim_method = 0;
    double energy_factor = 0;
    double multi = 0;
    double click_x = 0;
    double click_z = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "rk_fill_frame", fill_frame) ||
        !toml_number(tolerance_text, "rk_hold_frame", hold_frame) ||
        !toml_number(tolerance_text, "rk_start_metal", start_metal) ||
        !toml_number(tolerance_text, "rk_start_energy", start_energy) ||
        !toml_number(tolerance_text, "rk_commander_metal", commander_metal) ||
        !toml_number(tolerance_text, "rk_commander_energy", commander_energy) ||
        !toml_number(tolerance_text, "rk_commander_x", commander_x) ||
        !toml_number(tolerance_text, "rk_commander_z", commander_z) ||
        !toml_number(tolerance_text, "rk_store_metal", store_metal) ||
        !toml_number(tolerance_text, "rk_store_energy", store_energy) ||
        !toml_number(tolerance_text, "rk_sim_hz", sim_hz) || !toml_number(tolerance_text, "rk_slop", slop) ||
        !toml_number(tolerance_text, "rk_const_slop", const_slop) ||
        !toml_number(tolerance_text, "rk_pos_slop", pos_slop) || !toml_number(tolerance_text, "rk_worker", worker) ||
        !toml_number(tolerance_text, "rk_power_scale", power_scale) || !toml_number(tolerance_text, "rk_reach", reach) ||
        !toml_number(tolerance_text, "rk_unit_metal", unit_metal) ||
        !toml_number(tolerance_text, "rk_wreck_ratio", wreck_ratio) ||
        !toml_number(tolerance_text, "rk_feature_energy", feature_energy) ||
        !toml_number(tolerance_text, "rk_reclaim_method", reclaim_method) ||
        !toml_number(tolerance_text, "rk_energy_factor", energy_factor) ||
        !toml_number(tolerance_text, "rk_multi", multi) || !toml_number(tolerance_text, "rk_click_x", click_x) ||
        !toml_number(tolerance_text, "rk_click_z", click_z) || !(sim_hz > 0) || !(wreck_ratio > 0)) {
        std::cerr << "gate: m4reclaim tolerances are missing\n";
        return 2;
    }

    std::ofstream failure(options.oracle + "/last_failure.md");
    if (!failure) {
        std::cerr << "gate: could not write " << options.oracle << "/last_failure.md\n";
        return 2;
    }

    const double wreck_metal = std::floor(unit_metal * wreck_ratio);
    const double reclaim_time = std::max(1.0, (wreck_metal + feature_energy) * 6.0);
    const double scaled_worker = worker * power_scale;
    const double filled = fill_frame * (scaled_worker / sim_hz) / reclaim_time;
    const double fill_left = 1.0 - filled;
    const double fill_metal = start_metal + commander_metal * fill_frame / sim_hz + wreck_metal * filled;
    const double fill_energy = start_energy + commander_energy * fill_frame / sim_hz;
    const double hold_metal = start_metal + commander_metal * hold_frame / sim_hz + wreck_metal;
    const double hold_energy = start_energy + commander_energy * hold_frame / sim_hz;
    const std::string trace_path = options.actual + "/m4reclaim/reclaim.jsonl";
    std::vector<ReclaimSample> samples;
    const bool trace_ok = read_reclaim_samples(trace_path, samples);
    const ReclaimSample* fill = reclaim_at(samples, static_cast<int>(fill_frame));
    const ReclaimSample* hold = reclaim_at(samples, static_cast<int>(hold_frame));
    auto near = [&](double got, double want) { return std::fabs(got - want) <= slop; };
    auto tight = [&](double got, double want) { return std::fabs(got - want) <= const_slop; };
    auto identity = [&](const ReclaimSample& sample) {
        bool extra = false;
        for (const NanoUnit& unit : sample.units) {
            if (unit.def != "armcom") {
                extra = true;
            }
        }
        return sample.have_wreck && sample.units.size() == 1 && !extra && sample.def == "armstump" &&
               nano_unit(NanoSample{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, sample.units, {}, false}, "armcom", commander_x,
                         commander_z, pos_slop) != nullptr &&
               std::fabs(sample.x - click_x) <= pos_slop && std::fabs(sample.z - click_z) <= pos_slop &&
               tight(sample.wreck_metal, wreck_metal) && tight(sample.wreck_energy, feature_energy) &&
               tight(sample.reclaim_time, reclaim_time) && tight(sample.reclaim_speed, scaled_worker) &&
               tight(sample.build_distance, reach) && tight(sample.power_scale, power_scale) &&
               tight(sample.reclaim_method, reclaim_method) && tight(sample.energy_factor, energy_factor) &&
               tight(sample.multi, multi) && near(sample.metal_income, commander_metal) &&
               near(sample.energy_income, commander_energy) && near(sample.mex_income, 0) &&
               near(sample.energy_upkeep, 0) && near(sample.solar_income, 0) && near(sample.wind_income, 0) &&
               near(sample.metal_storage, store_metal) && near(sample.energy_storage, store_energy);
    };
    const bool fill_ok = fill != nullptr && identity(*fill) && fill->gone == 0 && fill->left > 0 && fill->left < 1 &&
                         tight(fill->left, fill_left) && near(fill->metal, fill_metal) && near(fill->energy, fill_energy);
    const bool hold_ok = hold != nullptr && identity(*hold) && hold->gone == 1 && tight(hold->left, 0) &&
                         near(hold->metal, hold_metal) && near(hold->energy, hold_energy);

    failure << "# reclaim\n\n";
    failure << "Scenario: reclaim\n\n";
    failure << "Entity: armstump wreck\n\n";
    failure << "Field: metal returned while the wreck is reclaimed\n\n";
    if (fill != nullptr && fill->have_wreck) {
        failure << "Frame " << fill->frame << " metal " << fill->metal << " energy " << fill->energy << " expected "
                << fill_metal << " " << fill_energy << "\n\n";
        failure << "Left " << fill->left << " expected " << fill_left << " gone " << fill->gone << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_wreck) {
        failure << "Frame " << hold->frame << " metal " << hold->metal << " energy " << hold->energy << " expected "
                << hold_metal << " " << hold_energy << "\n\n";
        failure << "Left " << hold->left << " expected 0 gone " << hold->gone << "\n\n";
        failure << "Wreck " << hold->x << " " << hold->z << " metal " << hold->wreck_metal << " time "
                << hold->reclaim_time << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Wreck metal " << wreck_metal << " over reclaim time " << reclaim_time << ". Reclaim power "
            << scaled_worker << " per second. The build stance is not hosted, so reclaim starts on the order frame.\n\n";

    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "m4reclaim FAIL trace\n";
        return 1;
    }
    std::cout << "fill metal " << fill->metal << " energy " << fill->energy << " expected " << fill_metal << " "
              << fill_energy << "\n";
    std::cout << "hold metal " << hold->metal << " energy " << hold->energy << " expected " << hold_metal << " "
              << hold_energy << "\n";
    std::cout << "left " << fill->left << " expected " << fill_left << " gone " << hold->gone << "\n";
    if (!fill_ok) {
        std::cout << "m4reclaim FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m4reclaim FAIL hold\n";
        return 1;
    }
    std::cout << "m4reclaim PASS\n";
    failure << "The reclaim self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace. The M4 milestone stays red until a Recoil trace matches eco_t1, build_tank, and reclaim at "
               "2%.\n";
    return 0;
}

struct LaserUnit {
    std::string def;
    int team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct LaserSample {
    int frame = -1;
    std::vector<LaserUnit> units;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double speed = 0;
    double range = 0;
    double reload_frames = 0;
    bool have_laser = false;
};

bool read_laser_samples(const std::string& path, std::vector<LaserSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> lasers;
        if (!json_objects(line, "units", units) || !json_objects(line, "ls", lasers)) {
            continue;
        }
        LaserSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(lasers[0], "def", sample.def) ||
            !json_string_field(lasers[0], "td", sample.target_def) || !json_number_field(lasers[0], "hp", sample.health) ||
            !json_number_field(lasers[0], "die", sample.die) || !json_number_field(lasers[0], "hits", sample.hits) ||
            !json_number_field(lasers[0], "shots", sample.shots) || !json_number_field(lasers[0], "gnd", sample.ground) ||
            !json_number_field(lasers[0], "dmg", sample.damage) || !json_number_field(lasers[0], "spd", sample.speed) ||
            !json_number_field(lasers[0], "rng", sample.range) || !json_number_field(lasers[0], "rel", sample.reload_frames)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_laser = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            LaserUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct LaserBoxGate {
    double ox = 0;
    double oy = 0;
    double oz = 0;
    double hx = 0;
    double hy = 0;
    double hz = 0;
};

bool laser_slab(double origin, double direction, double min_bound, double max_bound, double& tmin, double& tmax) {
    if (std::fabs(direction) < 1.0e-8) {
        return origin >= min_bound && origin <= max_bound;
    }
    double enter = (min_bound - origin) / direction;
    double leave = (max_bound - origin) / direction;
    if (enter > leave) {
        const double swap = enter;
        enter = leave;
        leave = swap;
    }
    tmin = tmin > enter ? tmin : enter;
    tmax = tmax < leave ? tmax : leave;
    return tmin <= tmax;
}

bool laser_hit(double x, double y, double z, double dx, double dy, double dz, double speed, double unit_x, double unit_y,
               double unit_z, const LaserBoxGate& box, double& travel) {
    const double min_x = unit_x + box.ox - box.hx;
    const double max_x = unit_x + box.ox + box.hx;
    const double min_y = unit_y + box.oy - box.hy;
    const double max_y = unit_y + box.oy + box.hy;
    const double min_z = unit_z + box.oz - box.hz;
    const double max_z = unit_z + box.oz + box.hz;
    double tmin = 0;
    double tmax = 1.0e9;
    if (!laser_slab(x, dx, min_x, max_x, tmin, tmax) || !laser_slab(y, dy, min_y, max_y, tmin, tmax) ||
        !laser_slab(z, dz, min_z, max_z, tmin, tmax) || tmax < 0) {
        return false;
    }
    const double enter = tmin < 0 ? 0 : tmin;
    if (enter > speed) {
        return false;
    }
    travel = enter;
    return true;
}

struct LaserExpect {
    double health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
};

LaserExpect expect_laser(int sample_frame, double shooter_x, double shooter_y, double shooter_z, double target_x,
                         double target_y, double target_z, const LaserBoxGate& shooter, const LaserBoxGate& target,
                         double speed, double range, int reload_frames, double damage, double health, bool burnblow) {
    struct Bolt {
        double x = 0;
        double y = 0;
        double z = 0;
        double dx = 0;
        double dy = 0;
        double dz = 0;
        int ttl = 0;
        bool alive = false;
        bool check = false;
    };
    const double mx = shooter_x + shooter.ox;
    const double my = shooter_y + shooter.oy + shooter.hy;
    const double mz = shooter_z + shooter.oz;
    LaserExpect expect;
    expect.health = health;
    int reload_ready = 0;
    std::vector<Bolt> bolts;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die == 0 && frame >= reload_ready) {
            const double dx = target_x - mx;
            const double dy = target_y - my;
            const double dz = target_z - mz;
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 0 && dist <= range) {
                Bolt bolt;
                bolt.alive = true;
                bolt.check = true;
                bolt.x = mx;
                bolt.y = my;
                bolt.z = mz;
                bolt.dx = dx / dist;
                bolt.dy = dy / dist;
                bolt.dz = dz / dist;
                const int ttlreq = static_cast<int>(std::ceil(dist / speed));
                const int ttlmax = static_cast<int>(std::floor(range / speed)) - 1;
                bolt.ttl = burnblow ? (ttlreq < ttlmax ? ttlreq : ttlmax) : (ttlreq > ttlmax ? ttlreq : ttlmax);
                bolts.push_back(bolt);
                ++expect.shots;
                reload_ready = frame + reload_frames;
            }
        }
        for (Bolt& bolt : bolts) {
            if (!bolt.alive || !bolt.check) {
                continue;
            }
            double travel = 0;
            if (expect.die == 0 && laser_hit(bolt.x, bolt.y, bolt.z, bolt.dx, bolt.dy, bolt.dz, speed, target_x, target_y,
                                            target_z, target, travel)) {
                expect.health = expect.health - damage;
                if (!(expect.health > 0)) {
                    expect.health = 0;
                    expect.die = frame;
                }
                ++expect.hits;
                bolt.alive = false;
                bolt.check = false;
                continue;
            }
            bolt.x += bolt.dx * speed;
            bolt.y += bolt.dy * speed;
            bolt.z += bolt.dz * speed;
            bolt.check = (--bolt.ttl) >= 0;
            if (!bolt.check) {
                bolt.alive = false;
            }
        }
    }
    return expect;
}

int gate_m5laser(const Options& options) {
    const std::string trace_path = options.actual + "/m5laser/duel_laser.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double velocity = 0;
    double sim_hz = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "dl_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "dl_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "dl_commander_x", commander_x) &&
        toml_number(tolerance_text, "dl_commander_z", commander_z) &&
        toml_number(tolerance_text, "dl_shooter_x", shooter_x) && toml_number(tolerance_text, "dl_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "dl_target_x", target_x) && toml_number(tolerance_text, "dl_target_z", target_z) &&
        toml_number(tolerance_text, "dl_pos_slop", pos_slop) && toml_number(tolerance_text, "dl_health", health) &&
        toml_number(tolerance_text, "dl_damage", damage) && toml_number(tolerance_text, "dl_file_range", file_range) &&
        toml_number(tolerance_text, "dl_reload", reload) && toml_number(tolerance_text, "dl_velocity", velocity) &&
        toml_number(tolerance_text, "dl_sim_hz", sim_hz) && toml_number(tolerance_text, "dl_hp_slop", hp_slop) &&
        toml_number(tolerance_text, "dl_shooter_ox", shooter_ox) && toml_number(tolerance_text, "dl_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "dl_shooter_oz", shooter_oz) && toml_number(tolerance_text, "dl_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "dl_shooter_sy", shooter_sy) && toml_number(tolerance_text, "dl_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "dl_target_ox", target_ox) && toml_number(tolerance_text, "dl_target_oy", target_oy) &&
        toml_number(tolerance_text, "dl_target_oz", target_oz) && toml_number(tolerance_text, "dl_target_sx", target_sx) &&
        toml_number(tolerance_text, "dl_target_sy", target_sy) && toml_number(tolerance_text, "dl_target_sz", target_sz);
    std::vector<LaserSample> samples;
    const bool trace_ok = tolerances_ok && read_laser_samples(trace_path, samples);
    const LaserSample* fill = nullptr;
    const LaserSample* hold = nullptr;
    for (const LaserSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const double per_frame = velocity / sim_hz;
    const double rounded = std::max(1.0, std::floor(file_range / per_frame)) * per_frame;
    const int reload_frames = std::max(1, static_cast<int>(reload * sim_hz));
    LaserBoxGate shooter_box{shooter_ox, shooter_oy, shooter_oz, shooter_sx * 0.5, shooter_sy * 0.5, shooter_sz * 0.5};
    LaserBoxGate target_box{target_ox, target_oy, target_oz, target_sx * 0.5, target_sy * 0.5, target_sz * 0.5};
    const LaserUnit* commander = nullptr;
    const LaserUnit* shooter = nullptr;
    const LaserUnit* tank = nullptr;
    if (hold != nullptr) {
        for (const LaserUnit& unit : hold->units) {
            if (unit.def == "armcom") {
                commander = &unit;
            } else if (unit.def == "cormando") {
                shooter = &unit;
            } else if (unit.def == "armstump") {
                tank = &unit;
            }
        }
    }
    LaserExpect fill_expect;
    LaserExpect hold_expect;
    if (shooter != nullptr && tank != nullptr) {
        fill_expect = expect_laser(static_cast<int>(fill_frame), shooter->x, shooter->y, shooter->z, tank->x, tank->y,
                                   tank->z, shooter_box, target_box, per_frame, rounded, reload_frames, damage, health,
                                   true);
        hold_expect = expect_laser(static_cast<int>(hold_frame), shooter->x, shooter->y, shooter->z, tank->x, tank->y,
                                   tank->z, shooter_box, target_box, per_frame, rounded, reload_frames, damage, health,
                                   true);
    }
    const bool placed = commander != nullptr && shooter != nullptr && tank != nullptr && hold->units.size() == 3 &&
                        commander->team == 0 && shooter->team == 0 && tank->team == 1 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(tank->x - target_x) <= pos_slop && std::fabs(tank->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->speed - per_frame) <= hp_slop && std::fabs(fill->range - rounded) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames && fill->ground == 0 && hold->ground == 0 &&
                         fill->def == "cormando" && fill->target_def == "armstump";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.hits > 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0 && static_cast<int>(hold->health) == 0;

    failure << "# duel_laser\n\n";
    failure << "Scenario: duel_laser\n\n";
    failure << "Entity: cormando laser against armstump\n\n";
    failure << "Field: health while a direct laser is firing\n\n";
    if (fill != nullptr && fill->have_laser) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " expected " << fill_expect.health << " " << fill_expect.hits
                << " " << fill_expect.shots << " " << fill_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_laser) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " expected " << hold_expect.health << " " << hold_expect.hits
                << " " << hold_expect.shots << " " << hold_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Laser damage " << damage << " speed " << per_frame << " range " << rounded << " reload "
            << reload_frames << ". The aim script is not hosted. The muzzle is the top of the attacker box.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || tank == nullptr) {
        std::cout << "m5laser FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " expected " << fill_expect.health << " "
              << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " expected " << hold_expect.health << " "
              << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5laser FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5laser FAIL hold\n";
        return 1;
    }
    std::cout << "m5laser PASS\n";
    failure << "The laser self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace. The M4 milestone stays red until a Recoil trace matches eco_t1, build_tank, and reclaim at "
               "2%. The M5 milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct CannonUnit {
    std::string def;
    int team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct CannonSample {
    int frame = -1;
    std::vector<CannonUnit> units;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double speed = 0;
    double range = 0;
    double reload_frames = 0;
    double gravity = 0;
    double height_mod = 0;
    bool have_cannon = false;
};

bool read_cannon_samples(const std::string& path, std::vector<CannonSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> cannons;
        if (!json_objects(line, "units", units) || !json_objects(line, "cn", cannons)) {
            continue;
        }
        CannonSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(cannons[0], "def", sample.def) ||
            !json_string_field(cannons[0], "td", sample.target_def) ||
            !json_number_field(cannons[0], "hp", sample.health) || !json_number_field(cannons[0], "die", sample.die) ||
            !json_number_field(cannons[0], "hits", sample.hits) || !json_number_field(cannons[0], "shots", sample.shots) ||
            !json_number_field(cannons[0], "gnd", sample.ground) || !json_number_field(cannons[0], "dmg", sample.damage) ||
            !json_number_field(cannons[0], "spd", sample.speed) || !json_number_field(cannons[0], "rng", sample.range) ||
            !json_number_field(cannons[0], "rel", sample.reload_frames) ||
            !json_number_field(cannons[0], "grv", sample.gravity) ||
            !json_number_field(cannons[0], "hmod", sample.height_mod)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_cannon = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            CannonUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct CannonExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
};

bool cannon_slab(float origin, float direction, float min_bound, float max_bound, float& tmin, float& tmax) {
    if (std::fabs(direction) < 1.0e-8f) {
        return origin >= min_bound && origin <= max_bound;
    }
    float enter = (min_bound - origin) / direction;
    float leave = (max_bound - origin) / direction;
    if (enter > leave) {
        const float swap = enter;
        enter = leave;
        leave = swap;
    }
    tmin = tmin > enter ? tmin : enter;
    tmax = tmax < leave ? tmax : leave;
    return tmin <= tmax;
}

bool cannon_hit(float x, float y, float z, float vx, float vy, float vz, float unit_x, float unit_y, float unit_z,
                float ox, float oy, float oz, float hx, float hy, float hz) {
    const float speed_sq = vx * vx + vy * vy + vz * vz;
    if (!(speed_sq > 0.f)) {
        return false;
    }
    const float speed = std::sqrt(speed_sq);
    const float dx = vx / speed;
    const float dy = vy / speed;
    const float dz = vz / speed;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!cannon_slab(x, dx, unit_x + ox - hx, unit_x + ox + hx, tmin, tmax) ||
        !cannon_slab(y, dy, unit_y + oy - hy, unit_y + oy + hy, tmin, tmax) ||
        !cannon_slab(z, dz, unit_z + oz - hz, unit_z + oz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    const float enter = tmin < 0.f ? 0.f : tmin;
    return enter <= speed;
}

bool cannon_launch(float mx, float my, float mz, float ax, float ay, float az, float speed, float gravity, float& vx,
                   float& vy, float& vz, int& ttl) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dsq = dx * dx + dy * dy + dz * dz;
    const float dfsq = dx * dx + dz * dz;
    if (!(dsq > 0.f) || !(dfsq > 0.f) || !(speed > 0.f) || !(gravity < 0.f)) {
        return false;
    }
    const float dxz = std::sqrt(dfsq);
    const float vsq = speed * speed;
    const float root1 = vsq * vsq + 2.f * vsq * gravity * dy - gravity * gravity * dfsq;
    if (!(root1 >= 0.f)) {
        return false;
    }
    const float root2 = 2.f * dfsq * dsq * (vsq + gravity * dy + std::sqrt(root1));
    if (!(root2 >= 0.f)) {
        return false;
    }
    const float vxz = std::sqrt(root2) / (2.f * dsq);
    const float want_y = (dxz == 0.f || vxz == 0.f) ? speed : (vxz * dy / dxz - dxz * gravity / (2.f * vxz));
    float dir_x = dx / dxz;
    float dir_z = dz / dxz;
    dir_x *= vxz;
    dir_z *= vxz;
    const float dir_len = std::sqrt(dir_x * dir_x + want_y * want_y + dir_z * dir_z);
    if (!(dir_len > 0.f)) {
        return false;
    }
    dir_x /= dir_len;
    const float dir_y = want_y / dir_len;
    dir_z /= dir_len;
    vx = dir_x * speed;
    vy = dir_y * speed;
    vz = dir_z * speed;
    const float sq_speed_2d = (dir_x * dir_x + dir_z * dir_z) * speed * speed;
    if (!(sq_speed_2d > 0.f)) {
        return false;
    }
    ttl = static_cast<int>(std::ceil(std::sqrt(dfsq / sq_speed_2d))) * 2;
    return ttl > 0;
}

CannonExpect expect_cannon(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                           float target_y, float target_z, float sox, float soy, float soz, float shx, float shy,
                           float shz, float tox, float toy, float toz, float thx, float thy, float thz, float speed,
                           float range, int reload_frames, float damage, float health, float gravity, float height_mod) {
    struct Shell {
        float x = 0;
        float y = 0;
        float z = 0;
        float vx = 0;
        float vy = 0;
        float vz = 0;
        int ttl = 0;
        bool alive = false;
    };
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float ydiff = (ay - my) * height_mod;
    const float root = range * range - ydiff * ydiff;
    const float weapon_range = root > 0.f ? std::sqrt(root) : 0.f;
    const float dist = std::sqrt((ax - mx) * (ax - mx) + (az - mz) * (az - mz));
    CannonExpect expect;
    expect.health = health;
    int reload_ready = 0;
    std::vector<Shell> shells;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die == 0 && frame >= reload_ready && dist > 0.f && dist <= weapon_range) {
            Shell shell;
            if (cannon_launch(mx, my, mz, ax, ay, az, speed, gravity, shell.vx, shell.vy, shell.vz, shell.ttl)) {
                shell.alive = true;
                shell.x = mx;
                shell.y = my;
                shell.z = mz;
                shells.push_back(shell);
                ++expect.shots;
                reload_ready = frame + reload_frames;
            }
        }
        for (Shell& shell : shells) {
            if (!shell.alive) {
                continue;
            }
            if (expect.die == 0 &&
                cannon_hit(shell.x, shell.y, shell.z, shell.vx, shell.vy, shell.vz, target_x, target_y, target_z, tox,
                           toy, toz, thx, thy, thz)) {
                expect.health = std::max(0.f, expect.health - damage);
                ++expect.hits;
                if (!(expect.health > 0.f)) {
                    expect.health = 0.f;
                    expect.die = frame;
                }
                shell.alive = false;
                continue;
            }
            shell.vy += gravity;
            shell.x += shell.vx;
            shell.y += shell.vy;
            shell.z += shell.vz;
            if (--shell.ttl == 0) {
                shell.alive = false;
            }
        }
    }
    return expect;
}

int gate_m5cannon(const Options& options) {
    const std::string trace_path = options.actual + "/m5cannon/duel_cannon.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double velocity = 0;
    double sim_hz = 0;
    double gravity_tag = 0;
    double height_mod = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "cn_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "cn_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "cn_commander_x", commander_x) &&
        toml_number(tolerance_text, "cn_commander_z", commander_z) &&
        toml_number(tolerance_text, "cn_shooter_x", shooter_x) && toml_number(tolerance_text, "cn_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "cn_target_x", target_x) && toml_number(tolerance_text, "cn_target_z", target_z) &&
        toml_number(tolerance_text, "cn_pos_slop", pos_slop) && toml_number(tolerance_text, "cn_health", health) &&
        toml_number(tolerance_text, "cn_damage", damage) && toml_number(tolerance_text, "cn_file_range", file_range) &&
        toml_number(tolerance_text, "cn_reload", reload) && toml_number(tolerance_text, "cn_velocity", velocity) &&
        toml_number(tolerance_text, "cn_sim_hz", sim_hz) && toml_number(tolerance_text, "cn_gravity", gravity_tag) &&
        toml_number(tolerance_text, "cn_heightmod", height_mod) && toml_number(tolerance_text, "cn_hp_slop", hp_slop) &&
        toml_number(tolerance_text, "cn_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "cn_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "cn_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "cn_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "cn_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "cn_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "cn_target_ox", target_ox) && toml_number(tolerance_text, "cn_target_oy", target_oy) &&
        toml_number(tolerance_text, "cn_target_oz", target_oz) && toml_number(tolerance_text, "cn_target_sx", target_sx) &&
        toml_number(tolerance_text, "cn_target_sy", target_sy) && toml_number(tolerance_text, "cn_target_sz", target_sz);
    std::vector<CannonSample> samples;
    const bool trace_ok = tolerances_ok && read_cannon_samples(trace_path, samples);
    const CannonSample* fill = nullptr;
    const CannonSample* hold = nullptr;
    for (const CannonSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float per_frame = static_cast<float>(static_cast<double>(velocity) / sim_hz);
    const int reload_frames = std::max(1, static_cast<int>(reload * sim_hz));
    const float grav_f = -std::max(0.001f, static_cast<float>(gravity_tag)) /
                         (static_cast<float>(sim_hz) * static_cast<float>(sim_hz));
    const CannonUnit* commander = nullptr;
    const CannonUnit* shooter = nullptr;
    const CannonUnit* tank = nullptr;
    if (hold != nullptr) {
        for (const CannonUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "armstump" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armstump" && unit.team == 1) {
                tank = &unit;
            }
        }
    }
    CannonExpect fill_expect;
    CannonExpect hold_expect;
    if (shooter != nullptr && tank != nullptr) {
        fill_expect = expect_cannon(static_cast<int>(fill_frame), static_cast<float>(shooter->x),
                                    static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                    static_cast<float>(tank->x), static_cast<float>(tank->y),
                                    static_cast<float>(tank->z), static_cast<float>(shooter_ox),
                                    static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                    static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                    static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                    static_cast<float>(target_oy), static_cast<float>(target_oz),
                                    static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                    static_cast<float>(target_sz * 0.5), per_frame, static_cast<float>(file_range),
                                    reload_frames, static_cast<float>(damage), static_cast<float>(health), grav_f,
                                    static_cast<float>(height_mod));
        hold_expect = expect_cannon(static_cast<int>(hold_frame), static_cast<float>(shooter->x),
                                    static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                    static_cast<float>(tank->x), static_cast<float>(tank->y),
                                    static_cast<float>(tank->z), static_cast<float>(shooter_ox),
                                    static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                    static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                    static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                    static_cast<float>(target_oy), static_cast<float>(target_oz),
                                    static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                    static_cast<float>(target_sz * 0.5), per_frame, static_cast<float>(file_range),
                                    reload_frames, static_cast<float>(damage), static_cast<float>(health), grav_f,
                                    static_cast<float>(height_mod));
    }
    const bool placed = commander != nullptr && shooter != nullptr && tank != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(tank->x - target_x) <= pos_slop && std::fabs(tank->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->speed - per_frame) <= hp_slop && std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         std::fabs(fill->gravity - grav_f) <= hp_slop &&
                         std::fabs(fill->height_mod - height_mod) <= hp_slop && fill->ground == 0 && hold->ground == 0 &&
                         fill->def == "armstump" && fill->target_def == "armstump";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.hits > 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0;

    failure << "# duel_cannon\n\n";
    failure << "Scenario: duel_cannon\n\n";
    failure << "Entity: armstump cannon against armstump\n\n";
    failure << "Field: health while a direct cannon is firing\n\n";
    if (fill != nullptr && fill->have_cannon) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " expected " << fill_expect.health << " " << fill_expect.hits
                << " " << fill_expect.shots << " " << fill_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_cannon) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " expected " << hold_expect.health << " " << hold_expect.hits
                << " " << hold_expect.shots << " " << hold_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Cannon damage " << damage << " speed " << per_frame << " range " << file_range << " reload "
            << reload_frames << " gravity " << grav_f
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || tank == nullptr) {
        std::cout << "m5cannon FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " expected " << fill_expect.health << " "
              << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " expected " << hold_expect.health << " "
              << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5cannon FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5cannon FAIL hold\n";
        return 1;
    }
    std::cout << "m5cannon PASS\n";
    failure << "The cannon self-check passed on engine output. Suites m0, m1, m2, and m3 stay red without a Recoil "
               "trace. The M4 milestone stays red until a Recoil trace matches eco_t1, build_tank, and reclaim at "
               "2%. The M5 milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct BeamUnit {
    std::string def;
    int team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct BeamSample {
    int frame = -1;
    std::vector<BeamUnit> units;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double beamtime = 0;
    double salvo = 0;
    double intensity = 0;
    bool have_beam = false;
};

bool read_beam_samples(const std::string& path, std::vector<BeamSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> beams;
        if (!json_objects(line, "units", units) || !json_objects(line, "bm", beams)) {
            continue;
        }
        BeamSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(beams[0], "def", sample.def) ||
            !json_string_field(beams[0], "td", sample.target_def) ||
            !json_number_field(beams[0], "hp", sample.health) || !json_number_field(beams[0], "die", sample.die) ||
            !json_number_field(beams[0], "hits", sample.hits) || !json_number_field(beams[0], "shots", sample.shots) ||
            !json_number_field(beams[0], "gnd", sample.ground) || !json_number_field(beams[0], "dmg", sample.damage) ||
            !json_number_field(beams[0], "rng", sample.range) || !json_number_field(beams[0], "rel", sample.reload_frames) ||
            !json_number_field(beams[0], "btm", sample.beamtime) || !json_number_field(beams[0], "sal", sample.salvo) ||
            !json_number_field(beams[0], "inten", sample.intensity)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_beam = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            BeamUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct BeamExpect {
    float health = 0;
    float intensity = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    int salvo = 0;
    int reload_frames = 0;
};

bool beam_slab(float origin, float direction, float min_bound, float max_bound, float& tmin, float& tmax) {
    if (std::fabs(direction) < 1.0e-8f) {
        return origin >= min_bound && origin <= max_bound;
    }
    float enter = (min_bound - origin) / direction;
    float leave = (max_bound - origin) / direction;
    if (enter > leave) {
        const float swap = enter;
        enter = leave;
        leave = swap;
    }
    tmin = tmin > enter ? tmin : enter;
    tmax = tmax < leave ? tmax : leave;
    return tmin <= tmax;
}

bool beam_entry(float mx, float my, float mz, float ax, float ay, float az, float cx, float cy, float cz, float hx,
                float hy, float hz, float& length) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.f)) {
        return false;
    }
    const float dir_x = dx / dist;
    const float dir_y = dy / dist;
    const float dir_z = dz / dist;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!beam_slab(mx, dir_x, cx - hx, cx + hx, tmin, tmax) ||
        !beam_slab(my, dir_y, cy - hy, cy + hy, tmin, tmax) ||
        !beam_slab(mz, dir_z, cz - hz, cz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    length = tmin < 0.f ? 0.f : tmin;
    return true;
}

BeamExpect expect_beam(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                       float target_y, float target_z, float sox, float soy, float soz, float shx, float shy, float shz,
                       float tox, float toy, float toz, float thx, float thy, float thz, float range, int reload_frames,
                       int salvo, float damage, float health) {
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = dist > 0.f && dist <= range;
    const float cx = target_x + tox;
    const float cy = target_y + toy;
    const float cz = target_z + toz;
    float length = 0;
    const bool hit = beam_entry(mx, my, mz, ax, ay, az, cx, cy, cz, thx, thy, thz, length) && length < range;
    const float intensity = hit ? std::max(0.f, 1.f - length / (range * 2.f)) : 0.f;
    const float scale = 1.f / static_cast<float>(salvo);
    const float tick = damage * (intensity * scale);
    BeamExpect expect;
    expect.health = health;
    expect.intensity = intensity;
    expect.salvo = salvo;
    expect.reload_frames = reload_frames;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die != 0) {
            break;
        }
        if (salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            reload_status = frame + reload_frames;
            salvo_left = salvo;
            next_salvo = frame;
        }
        if (salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame;
            ++expect.shots;
            if (hit) {
                expect.health = std::max(0.f, expect.health - tick);
                ++expect.hits;
                if (!(expect.health > 0.f)) {
                    expect.health = 0.f;
                    expect.die = frame;
                }
            } else {
                ++expect.ground;
            }
        }
    }
    return expect;
}

int gate_m5beam(const Options& options) {
    const std::string trace_path = options.actual + "/m5beam/duel_beam.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double beamtime = 0;
    double sim_hz = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "bm_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "bm_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "bm_commander_x", commander_x) &&
        toml_number(tolerance_text, "bm_commander_z", commander_z) &&
        toml_number(tolerance_text, "bm_shooter_x", shooter_x) && toml_number(tolerance_text, "bm_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "bm_target_x", target_x) && toml_number(tolerance_text, "bm_target_z", target_z) &&
        toml_number(tolerance_text, "bm_pos_slop", pos_slop) && toml_number(tolerance_text, "bm_health", health) &&
        toml_number(tolerance_text, "bm_damage", damage) && toml_number(tolerance_text, "bm_file_range", file_range) &&
        toml_number(tolerance_text, "bm_reload", reload) && toml_number(tolerance_text, "bm_beamtime", beamtime) &&
        toml_number(tolerance_text, "bm_sim_hz", sim_hz) && toml_number(tolerance_text, "bm_hp_slop", hp_slop) &&
        toml_number(tolerance_text, "bm_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "bm_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "bm_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "bm_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "bm_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "bm_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "bm_target_ox", target_ox) && toml_number(tolerance_text, "bm_target_oy", target_oy) &&
        toml_number(tolerance_text, "bm_target_oz", target_oz) && toml_number(tolerance_text, "bm_target_sx", target_sx) &&
        toml_number(tolerance_text, "bm_target_sy", target_sy) && toml_number(tolerance_text, "bm_target_sz", target_sz);
    std::vector<BeamSample> samples;
    const bool trace_ok = tolerances_ok && read_beam_samples(trace_path, samples);
    const BeamSample* fill = nullptr;
    const BeamSample* hold = nullptr;
    for (const BeamSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float beamtime_f = static_cast<float>(beamtime);
    const int salvo = std::max(1, static_cast<int>(beamtime_f * static_cast<float>(sim_hz)));
    const int reload_frames = std::max(1, static_cast<int>(reload * sim_hz));
    const BeamUnit* commander = nullptr;
    const BeamUnit* shooter = nullptr;
    const BeamUnit* scout = nullptr;
    if (hold != nullptr) {
        for (const BeamUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "armfav" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                scout = &unit;
            }
        }
    }
    BeamExpect fill_expect;
    BeamExpect hold_expect;
    if (shooter != nullptr && scout != nullptr) {
        fill_expect = expect_beam(static_cast<int>(fill_frame), static_cast<float>(shooter->x),
                                  static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                  static_cast<float>(scout->x), static_cast<float>(scout->y),
                                  static_cast<float>(scout->z), static_cast<float>(shooter_ox),
                                  static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                  static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                  static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                  static_cast<float>(target_oy), static_cast<float>(target_oz),
                                  static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                  static_cast<float>(target_sz * 0.5), static_cast<float>(file_range), reload_frames,
                                  salvo, static_cast<float>(damage), static_cast<float>(health));
        hold_expect = expect_beam(static_cast<int>(hold_frame), static_cast<float>(shooter->x),
                                  static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                  static_cast<float>(scout->x), static_cast<float>(scout->y),
                                  static_cast<float>(scout->z), static_cast<float>(shooter_ox),
                                  static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                  static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                  static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                  static_cast<float>(target_oy), static_cast<float>(target_oz),
                                  static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                  static_cast<float>(target_sz * 0.5), static_cast<float>(file_range), reload_frames,
                                  salvo, static_cast<float>(damage), static_cast<float>(health));
    }
    const bool placed = commander != nullptr && shooter != nullptr && scout != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(scout->x - target_x) <= pos_slop && std::fabs(scout->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         std::fabs(fill->beamtime - beamtime_f) <= hp_slop && static_cast<int>(fill->salvo) == salvo &&
                         std::fabs(fill->intensity - fill_expect.intensity) <= hp_slop &&
                         std::fabs(hold->intensity - hold_expect.intensity) <= hp_slop && fill->ground == 0 &&
                         hold->ground == 0 && fill_expect.ground == 0 && hold_expect.ground == 0 &&
                         fill->def == "armfav" && fill->target_def == "armfav";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.hits > 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0;

    failure << "# duel_beam\n\n";
    failure << "Scenario: duel_beam\n\n";
    failure << "Entity: armfav beam against armfav\n\n";
    failure << "Field: health while a beam laser is firing\n\n";
    if (fill != nullptr && fill->have_beam) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " expected " << fill_expect.health << " " << fill_expect.hits
                << " " << fill_expect.shots << " " << fill_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_beam) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " expected " << hold_expect.health << " " << hold_expect.hits
                << " " << hold_expect.shots << " " << hold_expect.die << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Beam damage " << damage << " range " << file_range << " reload " << reload_frames << " beamtime "
            << beamtime_f << " salvo " << salvo
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box. Intensity is the ray entry falloff.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || scout == nullptr) {
        std::cout << "m5beam FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " expected " << fill_expect.health << " "
              << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " expected " << hold_expect.health << " "
              << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5beam FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5beam FAIL hold\n";
        return 1;
    }
    std::cout << "m5beam PASS\n";
    failure << "The beam self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct MissileUnit {
    std::string def;
    int team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct MissileSample {
    int frame = -1;
    std::vector<MissileUnit> units;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double speed = 0;
    double max_speed = 0;
    double accel = 0;
    double range = 0;
    double reload_frames = 0;
    double height_mod = 0;
    bool have_missile = false;
};

bool read_missile_samples(const std::string& path, std::vector<MissileSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> missiles;
        if (!json_objects(line, "units", units) || !json_objects(line, "ms", missiles)) {
            continue;
        }
        MissileSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(missiles[0], "def", sample.def) ||
            !json_string_field(missiles[0], "td", sample.target_def) ||
            !json_number_field(missiles[0], "hp", sample.health) || !json_number_field(missiles[0], "die", sample.die) ||
            !json_number_field(missiles[0], "hits", sample.hits) || !json_number_field(missiles[0], "shots", sample.shots) ||
            !json_number_field(missiles[0], "gnd", sample.ground) || !json_number_field(missiles[0], "dmg", sample.damage) ||
            !json_number_field(missiles[0], "spd", sample.speed) || !json_number_field(missiles[0], "mxv", sample.max_speed) ||
            !json_number_field(missiles[0], "acc", sample.accel) || !json_number_field(missiles[0], "rng", sample.range) ||
            !json_number_field(missiles[0], "rel", sample.reload_frames) ||
            !json_number_field(missiles[0], "hmod", sample.height_mod)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_missile = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            MissileUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct MissileExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
};

MissileExpect expect_missile(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                             float target_y, float target_z, float sox, float soy, float soz, float shx, float shy,
                             float shz, float tox, float toy, float toz, float thx, float thy, float thz, float speed,
                             float max_speed, float accel, float range, int reload_frames, float damage, float health,
                             float height_mod) {
    struct Rocket {
        float x = 0;
        float y = 0;
        float z = 0;
        float vx = 0;
        float vy = 0;
        float vz = 0;
        float speed = 0;
        int ttl = 0;
        bool alive = false;
    };
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float ydiff = (ay - my) * height_mod;
    const float root = range * range - ydiff * ydiff;
    const float weapon_range = root > 0.f ? std::sqrt(root) : 0.f;
    const float horiz = std::sqrt((ax - mx) * (ax - mx) + (az - mz) * (az - mz));
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = horiz > 0.f && horiz <= weapon_range && dist > 0.f && max_speed > 0.f;
    const float span = dist > range ? dist : range;
    const int ttl = ready ? static_cast<int>(std::ceil(span / max_speed)) : 0;
    MissileExpect expect;
    expect.health = health;
    int reload_ready = 0;
    std::vector<Rocket> rockets;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die == 0 && frame >= reload_ready && ready && ttl > 0) {
            Rocket rocket;
            rocket.alive = true;
            rocket.ttl = ttl;
            rocket.x = mx;
            rocket.y = my;
            rocket.z = mz;
            rocket.speed = speed;
            rocket.vx = dx / dist * speed;
            rocket.vy = dy / dist * speed;
            rocket.vz = dz / dist * speed;
            rockets.push_back(rocket);
            ++expect.shots;
            reload_ready = frame + reload_frames;
        }
        for (Rocket& rocket : rockets) {
            if (!rocket.alive) {
                continue;
            }
            if (expect.die == 0 &&
                cannon_hit(rocket.x, rocket.y, rocket.z, rocket.vx, rocket.vy, rocket.vz, target_x, target_y, target_z,
                           tox, toy, toz, thx, thy, thz)) {
                expect.health = std::max(0.f, expect.health - damage);
                ++expect.hits;
                if (!(expect.health > 0.f)) {
                    expect.health = 0.f;
                    expect.die = frame;
                }
                rocket.alive = false;
                continue;
            }
            if (rocket.ttl - 1 > 0 && rocket.speed < max_speed) {
                const float faster = rocket.speed + accel;
                if (rocket.speed > 0.f) {
                    const float scale = faster / rocket.speed;
                    rocket.vx *= scale;
                    rocket.vy *= scale;
                    rocket.vz *= scale;
                }
                rocket.speed = faster;
            }
            rocket.x += rocket.vx;
            rocket.y += rocket.vy;
            rocket.z += rocket.vz;
            if (--rocket.ttl == 0) {
                rocket.alive = false;
            }
        }
    }
    return expect;
}

int gate_m5missile(const Options& options) {
    const std::string trace_path = options.actual + "/m5missile/duel_missile.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double start_velocity = 0;
    double velocity = 0;
    double acceleration = 0;
    double sim_hz = 0;
    double height_mod = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "ms_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "ms_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "ms_commander_x", commander_x) &&
        toml_number(tolerance_text, "ms_commander_z", commander_z) &&
        toml_number(tolerance_text, "ms_shooter_x", shooter_x) && toml_number(tolerance_text, "ms_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "ms_target_x", target_x) && toml_number(tolerance_text, "ms_target_z", target_z) &&
        toml_number(tolerance_text, "ms_pos_slop", pos_slop) && toml_number(tolerance_text, "ms_health", health) &&
        toml_number(tolerance_text, "ms_damage", damage) && toml_number(tolerance_text, "ms_file_range", file_range) &&
        toml_number(tolerance_text, "ms_reload", reload) &&
        toml_number(tolerance_text, "ms_start_velocity", start_velocity) &&
        toml_number(tolerance_text, "ms_velocity", velocity) &&
        toml_number(tolerance_text, "ms_acceleration", acceleration) &&
        toml_number(tolerance_text, "ms_sim_hz", sim_hz) && toml_number(tolerance_text, "ms_heightmod", height_mod) &&
        toml_number(tolerance_text, "ms_hp_slop", hp_slop) && toml_number(tolerance_text, "ms_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "ms_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "ms_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "ms_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "ms_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "ms_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "ms_target_ox", target_ox) && toml_number(tolerance_text, "ms_target_oy", target_oy) &&
        toml_number(tolerance_text, "ms_target_oz", target_oz) && toml_number(tolerance_text, "ms_target_sx", target_sx) &&
        toml_number(tolerance_text, "ms_target_sy", target_sy) && toml_number(tolerance_text, "ms_target_sz", target_sz);
    std::vector<MissileSample> samples;
    const bool trace_ok = tolerances_ok && read_missile_samples(trace_path, samples);
    const MissileSample* fill = nullptr;
    const MissileSample* hold = nullptr;
    for (const MissileSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float start_speed = static_cast<float>(static_cast<double>(start_velocity) / sim_hz);
    const float max_speed = static_cast<float>(static_cast<double>(velocity) / sim_hz);
    const float accel = static_cast<float>(static_cast<double>(acceleration) / (sim_hz * sim_hz));
    const int reload_frames = std::max(1, static_cast<int>(reload * sim_hz));
    const MissileUnit* commander = nullptr;
    const MissileUnit* shooter = nullptr;
    const MissileUnit* bot = nullptr;
    if (hold != nullptr) {
        for (const MissileUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "legbal" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "legbal" && unit.team == 1) {
                bot = &unit;
            }
        }
    }
    MissileExpect fill_expect;
    MissileExpect hold_expect;
    if (shooter != nullptr && bot != nullptr) {
        fill_expect = expect_missile(static_cast<int>(fill_frame), static_cast<float>(shooter->x),
                                     static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                     static_cast<float>(bot->x), static_cast<float>(bot->y),
                                     static_cast<float>(bot->z), static_cast<float>(shooter_ox),
                                     static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                     static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                     static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                     static_cast<float>(target_oy), static_cast<float>(target_oz),
                                     static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                     static_cast<float>(target_sz * 0.5), start_speed, max_speed, accel,
                                     static_cast<float>(file_range), reload_frames, static_cast<float>(damage),
                                     static_cast<float>(health), static_cast<float>(height_mod));
        hold_expect = expect_missile(static_cast<int>(hold_frame), static_cast<float>(shooter->x),
                                     static_cast<float>(shooter->y), static_cast<float>(shooter->z),
                                     static_cast<float>(bot->x), static_cast<float>(bot->y),
                                     static_cast<float>(bot->z), static_cast<float>(shooter_ox),
                                     static_cast<float>(shooter_oy), static_cast<float>(shooter_oz),
                                     static_cast<float>(shooter_sx * 0.5), static_cast<float>(shooter_sy * 0.5),
                                     static_cast<float>(shooter_sz * 0.5), static_cast<float>(target_ox),
                                     static_cast<float>(target_oy), static_cast<float>(target_oz),
                                     static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                                     static_cast<float>(target_sz * 0.5), start_speed, max_speed, accel,
                                     static_cast<float>(file_range), reload_frames, static_cast<float>(damage),
                                     static_cast<float>(health), static_cast<float>(height_mod));
    }
    const bool placed = commander != nullptr && shooter != nullptr && bot != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(bot->x - target_x) <= pos_slop && std::fabs(bot->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->speed - start_speed) <= hp_slop &&
                         std::fabs(fill->max_speed - max_speed) <= hp_slop && std::fabs(fill->accel - accel) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         std::fabs(fill->height_mod - height_mod) <= hp_slop && fill->ground == 0 && hold->ground == 0 &&
                         fill->def == "legbal" && fill->target_def == "legbal";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.hits > 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0;

    failure << "# duel_missile\n\n";
    failure << "Scenario: duel_missile\n\n";
    failure << "Entity: legbal missile against legbal\n\n";
    failure << "Field: health while an unguided missile is firing\n\n";
    if (fill != nullptr && fill->have_missile) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " expected "
                << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_missile) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " expected "
                << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Missile damage " << damage << " speed " << start_speed << " max " << max_speed << " accel " << accel
            << " range " << file_range << " reload " << reload_frames << " heightmod " << height_mod
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box. The rocket flies straight.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || bot == nullptr) {
        std::cout << "m5missile FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " ground " << fill->ground << " expected "
              << fill_expect.health << " " << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " ground " << hold->ground << " expected "
              << hold_expect.health << " " << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5missile FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5missile FAIL hold\n";
        return 1;
    }
    std::cout << "m5missile PASS\n";
    failure << "The missile self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct LightningUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct LightningSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double burst = 0;
    double gap = 0;
    double cost = 0;
    double energy = 0;
    double water = 0;
    std::vector<LightningUnit> units;
    bool have_lightning = false;
};

bool read_lightning_samples(const std::string& path, std::vector<LightningSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> bolts;
        if (!json_objects(line, "units", units) || !json_objects(line, "ln", bolts)) {
            continue;
        }
        LightningSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(bolts[0], "def", sample.def) ||
            !json_string_field(bolts[0], "td", sample.target_def) ||
            !json_number_field(bolts[0], "hp", sample.health) || !json_number_field(bolts[0], "die", sample.die) ||
            !json_number_field(bolts[0], "hits", sample.hits) || !json_number_field(bolts[0], "shots", sample.shots) ||
            !json_number_field(bolts[0], "gnd", sample.ground) || !json_number_field(bolts[0], "dmg", sample.damage) ||
            !json_number_field(bolts[0], "rng", sample.range) || !json_number_field(bolts[0], "rel", sample.reload_frames) ||
            !json_number_field(bolts[0], "brst", sample.burst) || !json_number_field(bolts[0], "gap", sample.gap) ||
            !json_number_field(bolts[0], "cost", sample.cost) || !json_number_field(bolts[0], "nrg", sample.energy) ||
            !json_number_field(bolts[0], "wpn", sample.water)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_lightning = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            LightningUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

bool lightning_bolt(float mx, float my, float mz, float ax, float ay, float az, float cx, float cy, float cz, float hx,
                    float hy, float hz, float range, bool waterweapon) {
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(dist > 0.f)) {
        return false;
    }
    const float dir_x = dx / dist;
    const float dir_y = dy / dist;
    const float dir_z = dz / dist;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!beam_slab(mx, dir_x, cx - hx, cx + hx, tmin, tmax) || !beam_slab(my, dir_y, cy - hy, cy + hy, tmin, tmax) ||
        !beam_slab(mz, dir_z, cz - hz, cz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    const float length = tmin < 0.f ? 0.f : tmin;
    if (length > range) {
        return false;
    }
    if (!waterweapon && dir_y < 0.f) {
        const float water_t = my / -dir_y;
        if (water_t < length) {
            return false;
        }
    }
    return true;
}

struct LightningExpect {
    float health = 0;
    float energy = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
};

LightningExpect expect_lightning(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                                 float target_y, float target_z, float sox, float soy, float soz, float shx, float shy,
                                 float shz, float tox, float toy, float toz, float thx, float thy, float thz,
                                 float range, int reload_frames, int burst, int delay, float damage, float health,
                                 float cost, float open_energy, float make, float store, bool waterweapon) {
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = dist > 0.f && dist <= range;
    const bool hit = lightning_bolt(mx, my, mz, ax, ay, az, target_x + tox, target_y + toy, target_z + toz, thx, thy,
                                    thz, range, waterweapon);
    LightningExpect expect;
    expect.health = health;
    expect.energy = open_energy;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        expect.energy = std::clamp(expect.energy + make, 0.f, store);
        if (expect.die == 0 && salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            const bool unaffordable = cost > 0.f && expect.energy < cost;
            if (!unaffordable) {
                if (cost > 0.f) {
                    expect.energy -= cost;
                }
                reload_status = frame + reload_frames;
                salvo_left = burst;
                next_salvo = frame;
            }
        }
        if (expect.die == 0 && salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame + delay;
            ++expect.shots;
            if (hit) {
                expect.health = std::max(0.f, expect.health - damage);
                ++expect.hits;
                if (!(expect.health > 0.f)) {
                    expect.health = 0.f;
                    expect.die = frame;
                }
            } else {
                ++expect.ground;
            }
        }
    }
    return expect;
}

int gate_m5lightning(const Options& options) {
    const std::string trace_path = options.actual + "/m5lightning/duel_lightning.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double burst = 0;
    double burstrate = 0;
    double cost = 0;
    double open_energy = 0;
    double make_energy = 0;
    double store_energy = 0;
    double sim_hz = 0;
    double hp_slop = 0;
    double water = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "ln_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "ln_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "ln_commander_x", commander_x) &&
        toml_number(tolerance_text, "ln_commander_z", commander_z) &&
        toml_number(tolerance_text, "ln_shooter_x", shooter_x) && toml_number(tolerance_text, "ln_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "ln_target_x", target_x) && toml_number(tolerance_text, "ln_target_z", target_z) &&
        toml_number(tolerance_text, "ln_pos_slop", pos_slop) && toml_number(tolerance_text, "ln_health", health) &&
        toml_number(tolerance_text, "ln_damage", damage) && toml_number(tolerance_text, "ln_file_range", file_range) &&
        toml_number(tolerance_text, "ln_reload", reload) && toml_number(tolerance_text, "ln_burst", burst) &&
        toml_number(tolerance_text, "ln_burstrate", burstrate) && toml_number(tolerance_text, "ln_cost", cost) &&
        toml_number(tolerance_text, "ln_open_energy", open_energy) &&
        toml_number(tolerance_text, "ln_make_energy", make_energy) &&
        toml_number(tolerance_text, "ln_store_energy", store_energy) && toml_number(tolerance_text, "ln_sim_hz", sim_hz) &&
        toml_number(tolerance_text, "ln_hp_slop", hp_slop) && toml_number(tolerance_text, "ln_water", water) &&
        toml_number(tolerance_text, "ln_shooter_ox", shooter_ox) && toml_number(tolerance_text, "ln_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "ln_shooter_oz", shooter_oz) && toml_number(tolerance_text, "ln_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "ln_shooter_sy", shooter_sy) && toml_number(tolerance_text, "ln_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "ln_target_ox", target_ox) && toml_number(tolerance_text, "ln_target_oy", target_oy) &&
        toml_number(tolerance_text, "ln_target_oz", target_oz) && toml_number(tolerance_text, "ln_target_sx", target_sx) &&
        toml_number(tolerance_text, "ln_target_sy", target_sy) && toml_number(tolerance_text, "ln_target_sz", target_sz);
    std::vector<LightningSample> samples;
    const bool trace_ok = tolerances_ok && read_lightning_samples(trace_path, samples);
    const LightningSample* fill = nullptr;
    const LightningSample* hold = nullptr;
    for (const LightningSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float reload_f = static_cast<float>(reload);
    const float rate_f = static_cast<float>(burstrate);
    const float hz = static_cast<float>(sim_hz);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const int delay = static_cast<int>(rate_f * hz);
    const int burst_n = static_cast<int>(burst);
    const float make = static_cast<float>(make_energy) / hz;
    const LightningUnit* commander = nullptr;
    const LightningUnit* shooter = nullptr;
    const LightningUnit* tank = nullptr;
    if (hold != nullptr) {
        for (const LightningUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "armlatnk" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armlatnk" && unit.team == 1) {
                tank = &unit;
            }
        }
    }
    LightningExpect fill_expect;
    LightningExpect hold_expect;
    if (shooter != nullptr && tank != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_lightning(frame, static_cast<float>(shooter->x), static_cast<float>(shooter->y),
                                    static_cast<float>(shooter->z), static_cast<float>(tank->x),
                                    static_cast<float>(tank->y), static_cast<float>(tank->z),
                                    static_cast<float>(shooter_ox), static_cast<float>(shooter_oy),
                                    static_cast<float>(shooter_oz), static_cast<float>(shooter_sx * 0.5),
                                    static_cast<float>(shooter_sy * 0.5), static_cast<float>(shooter_sz * 0.5),
                                    static_cast<float>(target_ox), static_cast<float>(target_oy),
                                    static_cast<float>(target_oz), static_cast<float>(target_sx * 0.5),
                                    static_cast<float>(target_sy * 0.5), static_cast<float>(target_sz * 0.5),
                                    static_cast<float>(file_range), reload_frames, burst_n, delay,
                                    static_cast<float>(damage), static_cast<float>(health), static_cast<float>(cost),
                                    static_cast<float>(open_energy), make, static_cast<float>(store_energy),
                                    static_cast<int>(water) != 0);
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const bool placed = commander != nullptr && shooter != nullptr && tank != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(tank->x - target_x) <= pos_slop && std::fabs(tank->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         static_cast<int>(fill->burst) == burst_n && static_cast<int>(fill->gap) == delay &&
                         std::fabs(fill->cost - cost) <= hp_slop && static_cast<int>(fill->water) == static_cast<int>(water) &&
                         fill->ground == 0 && hold->ground == 0 && fill->def == "armlatnk" && fill->target_def == "armlatnk";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots &&
                         std::fabs(fill->energy - fill_expect.energy) <= hp_slop && fill_expect.die == 0 &&
                         fill_expect.hits > 0 && fill_expect.ground == 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots &&
                         std::fabs(hold->energy - hold_expect.energy) <= hp_slop && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0 && hold_expect.ground == 0;

    failure << "# duel_lightning\n\n";
    failure << "Scenario: duel_lightning\n\n";
    failure << "Entity: armlatnk lightning against armlatnk\n\n";
    failure << "Field: health while a lightning burst is firing\n\n";
    if (fill != nullptr && fill->have_lightning) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " energy " << fill->energy
                << " expected " << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " "
                << fill_expect.die << " " << fill_expect.energy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_lightning) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " energy " << hold->energy
                << " expected " << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " "
                << hold_expect.die << " " << hold_expect.energy << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Lightning damage " << damage << " range " << file_range << " reload " << reload_frames << " burst "
            << burst_n << " delay " << delay << " cost " << cost
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box. The bolt is a straight hitscan.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || tank == nullptr) {
        std::cout << "m5lightning FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " energy " << fill->energy << " ground "
              << fill->ground << " expected " << fill_expect.health << " " << fill_expect.hits << " "
              << fill_expect.energy << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " energy " << hold->energy << " ground "
              << hold->ground << " expected " << hold_expect.health << " " << hold_expect.die << " "
              << hold_expect.energy << "\n";
    if (!fill_ok) {
        std::cout << "m5lightning FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5lightning FAIL hold\n";
        return 1;
    }
    std::cout << "m5lightning PASS\n";
    failure << "The lightning self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct FlameUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct FlameSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double burst = 0;
    double gap = 0;
    double speed = 0;
    double ttl = 0;
    double water = 0;
    std::vector<FlameUnit> units;
    bool have_flame = false;
};

bool read_flame_samples(const std::string& path, std::vector<FlameSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> flames;
        if (!json_objects(line, "units", units) || !json_objects(line, "fl", flames)) {
            continue;
        }
        FlameSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(flames[0], "def", sample.def) ||
            !json_string_field(flames[0], "td", sample.target_def) ||
            !json_number_field(flames[0], "hp", sample.health) || !json_number_field(flames[0], "die", sample.die) ||
            !json_number_field(flames[0], "hits", sample.hits) || !json_number_field(flames[0], "shots", sample.shots) ||
            !json_number_field(flames[0], "gnd", sample.ground) || !json_number_field(flames[0], "dmg", sample.damage) ||
            !json_number_field(flames[0], "rng", sample.range) || !json_number_field(flames[0], "rel", sample.reload_frames) ||
            !json_number_field(flames[0], "brst", sample.burst) || !json_number_field(flames[0], "gap", sample.gap) ||
            !json_number_field(flames[0], "spd", sample.speed) || !json_number_field(flames[0], "ttl", sample.ttl) ||
            !json_number_field(flames[0], "wpn", sample.water)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_flame = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            FlameUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct FlameBit {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float dx = 0;
    float dy = 0;
    float dz = 0;
    int life = 0;
    int ttl = 0;
};

struct FlameExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    int ttl = 0;
};

int flame_ttl(float dist, float range, float speed, float duration) {
    const float life = std::max(dist, range) / speed * duration;
    const int ttl = static_cast<int>(std::ceil(life));
    return ttl < 1 ? 1 : ttl;
}

bool flame_segment(float px, float py, float pz, float vx, float vy, float vz, float cx, float cy, float cz, float hx,
                   float hy, float hz) {
    float tmin = 0.f;
    float tmax = 1.f;
    if (!beam_slab(px, vx, cx - hx, cx + hx, tmin, tmax) || !beam_slab(py, vy, cy - hy, cy + hy, tmin, tmax) ||
        !beam_slab(pz, vz, cz - hz, cz + hz, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    return true;
}

FlameExpect expect_flame(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                         float target_y, float target_z, float sox, float soy, float soz, float shx, float shy,
                         float shz, float tox, float toy, float toz, float thx, float thy, float thz, float range,
                         int reload_frames, int burst, int delay, float damage, float health, float speed,
                         float duration) {
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = dist > 0.f && dist <= range;
    const float inv = dist > 0.f ? 1.f / dist : 0.f;
    const float dir_x = dx * inv;
    const float dir_y = dy * inv;
    const float dir_z = dz * inv;
    const float cx = target_x + tox;
    const float cy = target_y + toy;
    const float cz = target_z + toz;
    const int ttl = dist > 0.f && speed > 0.f ? flame_ttl(dist, range, speed, duration) : 0;
    FlameExpect expect;
    expect.health = health;
    expect.ttl = ttl;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    std::vector<FlameBit> particles;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die != 0) {
            break;
        }
        if (salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            reload_status = frame + reload_frames;
            salvo_left = burst;
            next_salvo = frame;
        }
        if (salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame + delay;
            ++expect.shots;
            FlameBit particle;
            particle.x = mx;
            particle.y = my;
            particle.z = mz;
            particle.dx = dir_x;
            particle.dy = dir_y;
            particle.dz = dir_z;
            particle.vx = dir_x * speed;
            particle.vy = dir_y * speed;
            particle.vz = dir_z * speed;
            particle.life = 0;
            particle.ttl = ttl;
            particles.push_back(particle);
        }
        for (size_t i = 0; i < particles.size();) {
            FlameBit& particle = particles[i];
            if (flame_segment(particle.x, particle.y, particle.z, particle.vx, particle.vy, particle.vz, cx, cy, cz,
                              thx, thy, thz)) {
                if (expect.die == 0) {
                    expect.health = std::max(0.f, expect.health - damage);
                    ++expect.hits;
                    if (!(expect.health > 0.f)) {
                        expect.health = 0.f;
                        expect.die = frame;
                    }
                }
                particles.erase(particles.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            particle.x += particle.vx;
            particle.y += particle.vy;
            particle.z += particle.vz;
            particle.vx += -particle.dx * 0.001f;
            particle.vy += -particle.dy * 0.001f;
            particle.vz += -particle.dz * 0.001f;
            ++particle.life;
            if (particle.life >= particle.ttl) {
                ++expect.ground;
                particles.erase(particles.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }
    return expect;
}

int gate_m5flame(const Options& options) {
    const std::string trace_path = options.actual + "/m5flame/duel_flame.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double burst = 0;
    double burstrate = 0;
    double velocity = 0;
    double duration = 0;
    double sim_hz = 0;
    double hp_slop = 0;
    double water = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "fl_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "fl_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "fl_commander_x", commander_x) &&
        toml_number(tolerance_text, "fl_commander_z", commander_z) &&
        toml_number(tolerance_text, "fl_shooter_x", shooter_x) && toml_number(tolerance_text, "fl_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "fl_target_x", target_x) && toml_number(tolerance_text, "fl_target_z", target_z) &&
        toml_number(tolerance_text, "fl_pos_slop", pos_slop) && toml_number(tolerance_text, "fl_health", health) &&
        toml_number(tolerance_text, "fl_damage", damage) && toml_number(tolerance_text, "fl_file_range", file_range) &&
        toml_number(tolerance_text, "fl_reload", reload) && toml_number(tolerance_text, "fl_burst", burst) &&
        toml_number(tolerance_text, "fl_burstrate", burstrate) && toml_number(tolerance_text, "fl_velocity", velocity) &&
        toml_number(tolerance_text, "fl_duration", duration) && toml_number(tolerance_text, "fl_sim_hz", sim_hz) &&
        toml_number(tolerance_text, "fl_hp_slop", hp_slop) && toml_number(tolerance_text, "fl_water", water) &&
        toml_number(tolerance_text, "fl_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "fl_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "fl_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "fl_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "fl_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "fl_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "fl_target_ox", target_ox) && toml_number(tolerance_text, "fl_target_oy", target_oy) &&
        toml_number(tolerance_text, "fl_target_oz", target_oz) && toml_number(tolerance_text, "fl_target_sx", target_sx) &&
        toml_number(tolerance_text, "fl_target_sy", target_sy) && toml_number(tolerance_text, "fl_target_sz", target_sz);
    std::vector<FlameSample> samples;
    const bool trace_ok = tolerances_ok && read_flame_samples(trace_path, samples);
    const FlameSample* fill = nullptr;
    const FlameSample* hold = nullptr;
    for (const FlameSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float reload_f = static_cast<float>(reload);
    const float rate_f = static_cast<float>(burstrate);
    const float hz = static_cast<float>(sim_hz);
    const float velocity_f = static_cast<float>(velocity);
    const float duration_f = static_cast<float>(duration);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const int delay = static_cast<int>(rate_f * hz);
    const int burst_n = static_cast<int>(burst);
    const float speed = velocity_f / hz;
    const FlameUnit* commander = nullptr;
    const FlameUnit* shooter = nullptr;
    const FlameUnit* dragon = nullptr;
    if (hold != nullptr) {
        for (const FlameUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "corcrwt4" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "corcrwt4" && unit.team == 1) {
                dragon = &unit;
            }
        }
    }
    FlameExpect fill_expect;
    FlameExpect hold_expect;
    if (shooter != nullptr && dragon != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_flame(frame, static_cast<float>(shooter->x), static_cast<float>(shooter->y),
                                static_cast<float>(shooter->z), static_cast<float>(dragon->x),
                                static_cast<float>(dragon->y), static_cast<float>(dragon->z),
                                static_cast<float>(shooter_ox), static_cast<float>(shooter_oy),
                                static_cast<float>(shooter_oz), static_cast<float>(shooter_sx * 0.5),
                                static_cast<float>(shooter_sy * 0.5), static_cast<float>(shooter_sz * 0.5),
                                static_cast<float>(target_ox), static_cast<float>(target_oy),
                                static_cast<float>(target_oz), static_cast<float>(target_sx * 0.5),
                                static_cast<float>(target_sy * 0.5), static_cast<float>(target_sz * 0.5),
                                static_cast<float>(file_range), reload_frames, burst_n, delay,
                                static_cast<float>(damage), static_cast<float>(health), speed, duration_f);
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const bool placed = commander != nullptr && shooter != nullptr && dragon != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(dragon->x - target_x) <= pos_slop && std::fabs(dragon->z - target_z) <= pos_slop;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         static_cast<int>(fill->burst) == burst_n && static_cast<int>(fill->gap) == delay &&
                         std::fabs(fill->speed - speed) <= hp_slop && static_cast<int>(fill->ttl) == fill_expect.ttl &&
                         static_cast<int>(hold->ttl) == hold_expect.ttl &&
                         static_cast<int>(fill->water) == static_cast<int>(water) && fill->ground == 0 &&
                         hold->ground == 0 && fill->def == "corcrwt4" && fill->target_def == "corcrwt4";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.hits > 0 && fill_expect.ground == 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0 && hold_expect.ground == 0;

    failure << "# duel_flame\n\n";
    failure << "Scenario: duel_flame\n\n";
    failure << "Entity: corcrwt4 flame against corcrwt4\n\n";
    failure << "Field: health while a flame burst is firing\n\n";
    if (fill != nullptr && fill->have_flame) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " expected "
                << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_flame) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " expected "
                << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Flame damage " << damage << " speed " << speed << " range " << file_range << " reload " << reload_frames
            << " burst " << burst_n << " delay " << delay << " ttl " << hold_expect.ttl
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box. The particle moves, then its velocity gains a fixed bias along the shot.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || dragon == nullptr) {
        std::cout << "m5flame FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " ground " << fill->ground << " expected "
              << fill_expect.health << " " << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " ground " << hold->ground << " expected "
              << hold_expect.health << " " << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5flame FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5flame FAIL hold\n";
        return 1;
    }
    std::cout << "m5flame PASS\n";
    failure << "The flame self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct TorpedoUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct TorpedoSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double speed = 0;
    double max_speed = 0;
    double ttl = 0;
    double gravity = 0;
    double water = 0;
    std::vector<TorpedoUnit> units;
    bool have_torpedo = false;
};

bool read_torpedo_samples(const std::string& path, std::vector<TorpedoSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> shots;
        if (!json_objects(line, "units", units) || !json_objects(line, "tp", shots)) {
            continue;
        }
        TorpedoSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(shots[0], "def", sample.def) ||
            !json_string_field(shots[0], "td", sample.target_def) ||
            !json_number_field(shots[0], "hp", sample.health) || !json_number_field(shots[0], "die", sample.die) ||
            !json_number_field(shots[0], "hits", sample.hits) || !json_number_field(shots[0], "shots", sample.shots) ||
            !json_number_field(shots[0], "gnd", sample.ground) || !json_number_field(shots[0], "dmg", sample.damage) ||
            !json_number_field(shots[0], "rng", sample.range) || !json_number_field(shots[0], "rel", sample.reload_frames) ||
            !json_number_field(shots[0], "spd", sample.speed) || !json_number_field(shots[0], "mxv", sample.max_speed) ||
            !json_number_field(shots[0], "ttl", sample.ttl) || !json_number_field(shots[0], "grv", sample.gravity) ||
            !json_number_field(shots[0], "wpn", sample.water)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_torpedo = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            TorpedoUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct TorpedoBit {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float dx = 0;
    float dy = 0;
    float dz = 0;
    float speed_w = 0;
    int ttl = 0;
};

struct TorpedoExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    int ttl = 0;
};

int torpedo_fuel(float dist, float range, float projectile_speed, int flight_frames) {
    if (flight_frames > 0) {
        return flight_frames;
    }
    if (!(projectile_speed > 0.f)) {
        return 1;
    }
    const float life = std::max(dist, range) / projectile_speed + 25.f;
    const int ttl = static_cast<int>(std::ceil(life));
    return ttl < 1 ? 1 : ttl;
}

void torpedo_set_speed(TorpedoBit& shot) {
    const float mag = std::sqrt(shot.vx * shot.vx + shot.vy * shot.vy + shot.vz * shot.vz);
    shot.speed_w = mag;
    if (!(mag > 0.f)) {
        return;
    }
    shot.dx = shot.vx / mag;
    shot.dy = shot.vy / mag;
    shot.dz = shot.vz / mag;
}

void torpedo_advance(TorpedoBit& shot, float gravity, float max_speed) {
    if (shot.y > 0.f) {
        shot.vy += gravity;
        torpedo_set_speed(shot);
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        return;
    }
    if (--shot.ttl > 0) {
        if (shot.speed_w < max_speed) {
            shot.speed_w += 0.2f;
        }
        const float len = std::sqrt(shot.dx * shot.dx + shot.dy * shot.dy + shot.dz * shot.dz);
        if (len > 0.f) {
            shot.dx /= len;
            shot.dy /= len;
            shot.dz /= len;
        }
        shot.vx = shot.dx * shot.speed_w;
        shot.vy = shot.dy * shot.speed_w;
        shot.vz = shot.dz * shot.speed_w;
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;
        return;
    }
    shot.vx *= 0.98f;
    shot.vy = shot.vy * 0.98f + gravity;
    shot.vz *= 0.98f;
    torpedo_set_speed(shot);
    shot.x += shot.vx;
    shot.y += shot.vy;
    shot.z += shot.vz;
}

TorpedoExpect expect_torpedo(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                             float target_y, float target_z, float sox, float soy, float soz, float shx, float shy,
                             float shz, float tox, float toy, float toz, float thx, float thy, float thz, float range,
                             int reload_frames, int burst, int delay, float damage, float health, float speed,
                             float max_speed, int flight_frames, float gravity) {
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = dist > 0.f && dist <= range && ay <= 0.f;
    const float inv = dist > 0.f ? 1.f / dist : 0.f;
    const float dir_x = dx * inv;
    const float dir_y = dy * inv;
    const float dir_z = dz * inv;
    const float cx = target_x + tox;
    const float cy = target_y + toy;
    const float cz = target_z + toz;
    const int ttl = dist > 0.f && speed > 0.f ? torpedo_fuel(dist, range, max_speed, flight_frames) : 0;
    TorpedoExpect expect;
    expect.health = health;
    expect.ttl = ttl;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    std::vector<TorpedoBit> live;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die != 0) {
            break;
        }
        if (salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            reload_status = frame + reload_frames;
            salvo_left = burst;
            next_salvo = frame;
        }
        if (salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame + delay;
            ++expect.shots;
            if (!(dist > 0.f) || !(speed > 0.f)) {
                ++expect.ground;
            } else {
                TorpedoBit shot;
                shot.x = mx;
                shot.y = my;
                shot.z = mz;
                shot.dx = dir_x;
                shot.dy = dir_y;
                shot.dz = dir_z;
                shot.speed_w = speed;
                shot.vx = dir_x * speed;
                shot.vy = dir_y * speed;
                shot.vz = dir_z * speed;
                shot.ttl = ttl;
                live.push_back(shot);
            }
        }
        for (size_t i = 0; i < live.size();) {
            TorpedoBit& shot = live[i];
            if (flame_segment(shot.x, shot.y, shot.z, shot.vx, shot.vy, shot.vz, cx, cy, cz, thx, thy, thz)) {
                if (expect.die == 0) {
                    expect.health = std::max(0.f, expect.health - damage);
                    ++expect.hits;
                    if (!(expect.health > 0.f)) {
                        expect.health = 0.f;
                        expect.die = frame;
                    }
                }
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            torpedo_advance(shot, gravity, max_speed);
            ++i;
        }
    }
    return expect;
}

int gate_m5torpedo(const Options& options) {
    const std::string trace_path = options.actual + "/m5torpedo/duel_torpedo.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double start_velocity = 0;
    double max_velocity = 0;
    double flight = 0;
    double sim_hz = 0;
    double gravity_tag = 0;
    double hp_slop = 0;
    double water = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "tp_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "tp_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "tp_commander_x", commander_x) &&
        toml_number(tolerance_text, "tp_commander_z", commander_z) &&
        toml_number(tolerance_text, "tp_shooter_x", shooter_x) && toml_number(tolerance_text, "tp_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "tp_target_x", target_x) && toml_number(tolerance_text, "tp_target_z", target_z) &&
        toml_number(tolerance_text, "tp_pos_slop", pos_slop) && toml_number(tolerance_text, "tp_health", health) &&
        toml_number(tolerance_text, "tp_damage", damage) && toml_number(tolerance_text, "tp_file_range", file_range) &&
        toml_number(tolerance_text, "tp_reload", reload) &&
        toml_number(tolerance_text, "tp_start_velocity", start_velocity) &&
        toml_number(tolerance_text, "tp_max_velocity", max_velocity) && toml_number(tolerance_text, "tp_flight", flight) &&
        toml_number(tolerance_text, "tp_sim_hz", sim_hz) && toml_number(tolerance_text, "tp_gravity", gravity_tag) &&
        toml_number(tolerance_text, "tp_hp_slop", hp_slop) && toml_number(tolerance_text, "tp_water", water) &&
        toml_number(tolerance_text, "tp_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "tp_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "tp_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "tp_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "tp_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "tp_shooter_sz", shooter_sz) &&
        toml_number(tolerance_text, "tp_target_ox", target_ox) && toml_number(tolerance_text, "tp_target_oy", target_oy) &&
        toml_number(tolerance_text, "tp_target_oz", target_oz) && toml_number(tolerance_text, "tp_target_sx", target_sx) &&
        toml_number(tolerance_text, "tp_target_sy", target_sy) && toml_number(tolerance_text, "tp_target_sz", target_sz);
    std::vector<TorpedoSample> samples;
    const bool trace_ok = tolerances_ok && read_torpedo_samples(trace_path, samples);
    const TorpedoSample* fill = nullptr;
    const TorpedoSample* hold = nullptr;
    for (const TorpedoSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float hz = static_cast<float>(sim_hz);
    const float reload_f = static_cast<float>(reload);
    const float flight_f = static_cast<float>(flight);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const int flight_frames = static_cast<int>(flight_f * hz);
    const float speed = static_cast<float>(start_velocity) / hz;
    const float max_speed = static_cast<float>(max_velocity) / hz;
    const float grav_f = -std::max(0.001f, static_cast<float>(gravity_tag)) / (hz * hz);
    const TorpedoUnit* commander = nullptr;
    const TorpedoUnit* shooter = nullptr;
    const TorpedoUnit* sub = nullptr;
    if (hold != nullptr) {
        for (const TorpedoUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "corsub" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "corsub" && unit.team == 1) {
                sub = &unit;
            }
        }
    }
    TorpedoExpect fill_expect;
    TorpedoExpect hold_expect;
    if (shooter != nullptr && sub != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_torpedo(frame, static_cast<float>(shooter->x), static_cast<float>(shooter->y),
                                  static_cast<float>(shooter->z), static_cast<float>(sub->x),
                                  static_cast<float>(sub->y), static_cast<float>(sub->z),
                                  static_cast<float>(shooter_ox), static_cast<float>(shooter_oy),
                                  static_cast<float>(shooter_oz), static_cast<float>(shooter_sx * 0.5),
                                  static_cast<float>(shooter_sy * 0.5), static_cast<float>(shooter_sz * 0.5),
                                  static_cast<float>(target_ox), static_cast<float>(target_oy),
                                  static_cast<float>(target_oz), static_cast<float>(target_sx * 0.5),
                                  static_cast<float>(target_sy * 0.5), static_cast<float>(target_sz * 0.5),
                                  static_cast<float>(file_range), reload_frames, 1, 0, static_cast<float>(damage),
                                  static_cast<float>(health), speed, max_speed, flight_frames, grav_f);
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const bool underwater = shooter != nullptr && sub != nullptr && shooter->y < 0.0 && sub->y < 0.0 &&
                            shooter->y + shooter_sy * 0.5 <= 0.0 && sub->y + target_sy * 0.5 <= 0.0;
    const bool placed = commander != nullptr && shooter != nullptr && sub != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(sub->x - target_x) <= pos_slop && std::fabs(sub->z - target_z) <= pos_slop &&
                        underwater;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         std::fabs(fill->speed - speed) <= hp_slop && std::fabs(fill->max_speed - max_speed) <= hp_slop &&
                         static_cast<int>(fill->ttl) == fill_expect.ttl && static_cast<int>(hold->ttl) == hold_expect.ttl &&
                         std::fabs(fill->gravity - grav_f) <= hp_slop &&
                         static_cast<int>(fill->water) == static_cast<int>(water) && fill->ground == 0 &&
                         hold->ground == 0 && fill->def == "corsub" && fill->target_def == "corsub";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && fill_expect.die == 0 &&
                         fill_expect.shots > 0 && fill_expect.ground == 0;
    const bool hold_ok = numbers && placed && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0 && hold_expect.ground == 0;

    failure << "# duel_torpedo\n\n";
    failure << "Scenario: duel_torpedo\n\n";
    failure << "Entity: corsub torpedo against corsub\n\n";
    failure << "Field: health while a torpedo is in flight\n\n";
    if (fill != nullptr && fill->have_torpedo) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " expected "
                << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_torpedo) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " expected "
                << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.die
                << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Torpedo damage " << damage << " speed " << speed << " max " << max_speed << " range " << file_range
            << " reload " << reload_frames << " ttl " << hold_expect.ttl << " gravity " << grav_f
            << ". The aim script is not hosted. The muzzle is the top of the attacker box. The aim point is the "
               "center of the target box. Tracking is off. A powered shot adds 0.2 before it moves.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || sub == nullptr) {
        std::cout << "m5torpedo FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " ground " << fill->ground << " expected "
              << fill_expect.health << " " << fill_expect.hits << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " ground " << hold->ground << " expected "
              << hold_expect.health << " " << hold_expect.die << "\n";
    if (!fill_ok) {
        std::cout << "m5torpedo FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5torpedo FAIL hold\n";
        return 1;
    }
    std::cout << "m5torpedo PASS\n";
    failure << "The torpedo self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

bool json_number_array(const std::string& object, const char* key, std::vector<double>& out) {
    const std::string pattern = std::string("\"") + key + "\":[";
    const auto at = object.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t cursor = at + pattern.size();
    while (cursor < object.size() && object[cursor] != ']') {
        while (cursor < object.size() && (object[cursor] == ' ' || object[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= object.size() || object[cursor] == ']') {
            break;
        }
        char* end = nullptr;
        const double value = std::strtod(object.c_str() + cursor, &end);
        if (end == object.c_str() + cursor) {
            return false;
        }
        out.push_back(value);
        cursor = static_cast<std::size_t>(end - object.c_str());
    }
    return !out.empty();
}

struct BombUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct BombSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double burst = 0;
    double gap = 0;
    double speed = 0;
    double accel = 0;
    double cruise = 0;
    double pitch = 0;
    double ttl = 0;
    double gravity = 0;
    double water = 0;
    double height_z0 = 0;
    double height_step = 0;
    std::vector<float> heights;
    std::vector<BombUnit> units;
    bool have_bomb = false;
};

bool read_bomb_samples(const std::string& path, std::vector<BombSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> shots;
        if (!json_objects(line, "units", units) || !json_objects(line, "bb", shots)) {
            continue;
        }
        BombSample sample;
        double frame = 0;
        std::vector<double> heights;
        if (!json_number_field(line, "f", frame) || !json_string_field(shots[0], "def", sample.def) ||
            !json_string_field(shots[0], "td", sample.target_def) ||
            !json_number_field(shots[0], "hp", sample.health) || !json_number_field(shots[0], "die", sample.die) ||
            !json_number_field(shots[0], "hits", sample.hits) || !json_number_field(shots[0], "shots", sample.shots) ||
            !json_number_field(shots[0], "gnd", sample.ground) || !json_number_field(shots[0], "dmg", sample.damage) ||
            !json_number_field(shots[0], "rng", sample.range) || !json_number_field(shots[0], "rel", sample.reload_frames) ||
            !json_number_field(shots[0], "brst", sample.burst) || !json_number_field(shots[0], "gap", sample.gap) ||
            !json_number_field(shots[0], "spd", sample.speed) || !json_number_field(shots[0], "acc", sample.accel) ||
            !json_number_field(shots[0], "cru", sample.cruise) || !json_number_field(shots[0], "pit", sample.pitch) ||
            !json_number_field(shots[0], "ttl", sample.ttl) || !json_number_field(shots[0], "grv", sample.gravity) ||
            !json_number_field(shots[0], "wpn", sample.water) || !json_number_field(shots[0], "gz", sample.height_z0) ||
            !json_number_field(shots[0], "gs", sample.height_step) || !json_number_array(shots[0], "gh", heights)) {
            continue;
        }
        sample.heights.reserve(heights.size());
        for (double height : heights) {
            sample.heights.push_back(static_cast<float>(height));
        }
        sample.frame = static_cast<int>(frame);
        sample.have_bomb = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            BombUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct BombBit {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    int ttl = 0;
};

struct BombExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
    float plane_x = 0;
    float plane_y = 0;
    float plane_z = 0;
    float target_y = 0;
    bool inside = false;
};

float bomb_height_at(const std::vector<float>& heights, float z0, float step, float z, bool& inside) {
    if (!(step > 0.f) || heights.size() < 2) {
        inside = false;
        return 0.f;
    }
    const float fz = (z - z0) / step;
    if (fz < 0.f || fz > static_cast<float>(heights.size() - 1)) {
        inside = false;
        return 0.f;
    }
    const int i0 = static_cast<int>(fz);
    const int i1 = std::min(i0 + 1, static_cast<int>(heights.size()) - 1);
    const float t = fz - static_cast<float>(i0);
    return heights[static_cast<size_t>(i0)] +
           (heights[static_cast<size_t>(i1)] - heights[static_cast<size_t>(i0)]) * t;
}

void bomb_normalize(float& x, float& y, float& z) {
    const float sq = x * x + y * y + z * z;
    if (!(sq > 1.0e-12f)) {
        return;
    }
    const float inv = 1.f / std::sqrt(sq);
    x *= inv;
    y *= inv;
    z *= inv;
}

float bomb_fall(float muzzle_y, float impact_y, float vy, float gravity) {
    if (muzzle_y <= impact_y || gravity == 0.f) {
        return 0.f;
    }
    const float d = impact_y - muzzle_y;
    const float tt = vy * vy + 2.f * d * gravity;
    if (!(tt >= 0.f)) {
        return 0.f;
    }
    return (-vy - std::sqrt(tt)) / gravity;
}

BombExpect expect_bomb(int sample_frame, float start_x, float start_z, float goal_x, float goal_z, float target_x,
                       float target_z, float target_ox, float target_oy, float target_oz, float target_hx,
                       float target_hy, float target_hz, int reload_frames, int burst, int delay, float damage,
                       float health, float max_speed, float accel, float cruise, float pitch, float gravity,
                       const std::vector<float>& heights, float height_z0, float height_step) {
    BombExpect expect;
    expect.health = health;
    bool inside = true;
    const float spawn_ground = bomb_height_at(heights, height_z0, height_step, start_z, inside);
    const float spawn_surface = spawn_ground < 0.f ? 0.f : spawn_ground;
    float plane_x = start_x;
    float plane_y = spawn_surface + cruise;
    float plane_z = start_z;
    float speed = 0.f;
    float vy = 0.f;
    bool chasing = true;
    const float target_ground = bomb_height_at(heights, height_z0, height_step, target_z, inside);
    const float target_y = target_ground;
    expect.target_y = target_y;
    const float ax = target_x + target_ox;
    const float ay = target_y + target_oy;
    const float az = target_z + target_oz;
    int salvo_left = 0;
    int next_salvo = 0;
    int reload_status = 0;
    std::vector<BombBit> live;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        speed = std::min(max_speed, speed + accel);
        plane_z += speed;
        const float ground = bomb_height_at(heights, height_z0, height_step, plane_z, inside);
        const float surface = ground < 0.f ? 0.f : ground;
        const float target_alt = surface + cruise;
        float climb = 1.0e6f;
        if (pitch > 0.f) {
            climb = std::max(speed, 0.01f) * std::tan(pitch);
        }
        const float dy = std::clamp(target_alt - plane_y, -climb, climb);
        plane_y += dy;
        vy = dy;
        if (chasing) {
            const float left = goal_x - plane_x;
            const float forward = goal_z - plane_z;
            if (left * left + forward * forward <= 64.f) {
                chasing = false;
            }
        }
        if (expect.die != 0) {
            continue;
        }
        const float vx = 0.f;
        const float vz = speed;
        bool ready = target_y >= 0.f && plane_y >= ay;
        if (ready) {
            const float fall = bomb_fall(plane_y, ay, vy, gravity);
            const float speed2d = std::sqrt(vx * vx + vz * vz);
            const float drop = static_cast<float>(std::max(1, burst - 1) * delay) * speed2d * 0.5f;
            const float px = plane_x + vx * fall;
            const float pz = plane_z + vz * fall;
            const float dx = ax - px;
            const float dz = az - pz;
            ready = dx * dx + dz * dz < drop * drop;
        }
        if (salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            reload_status = frame + reload_frames;
            salvo_left = burst;
            next_salvo = frame;
        }
        if (salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame + delay;
            ++expect.shots;
            float dif_x = ax - plane_x;
            float dif_y = 0.f;
            float dif_z = az - plane_z;
            float dir_x = vx;
            float dir_y = vy;
            float dir_z = vz;
            bomb_normalize(dir_x, dir_y, dir_z);
            dir_y = std::min(0.f, dir_y);
            bomb_normalize(dir_x, dir_y, dir_z);
            const float dot = dif_x * dir_x + dif_y * dir_y + dif_z * dir_z;
            dif_x -= dir_x * dot;
            dif_y -= dir_y * dot;
            dif_z -= dir_z * dot;
            const float predict = bomb_fall(plane_y, ay, vy, gravity);
            const float scale = std::max(0.01f, predict);
            dif_x /= scale;
            dif_y /= scale;
            dif_z /= scale;
            const float sq = dif_x * dif_x + dif_y * dif_y + dif_z * dif_z;
            if (sq > 1.f) {
                const float inv = 1.f / std::sqrt(sq);
                dif_x *= inv;
                dif_y *= inv;
                dif_z *= inv;
            }
            BombBit shot;
            shot.x = plane_x;
            shot.y = plane_y;
            shot.z = plane_z;
            shot.vx = vx + dif_x;
            shot.vy = vy + dif_y;
            shot.vz = vz + dif_z;
            shot.ttl = 1000;
            live.push_back(shot);
        }
        for (size_t i = 0; i < live.size();) {
            BombBit& shot = live[i];
            if (flame_segment(shot.x, shot.y, shot.z, shot.vx, shot.vy, shot.vz, ax, ay, az, target_hx, target_hy,
                              target_hz)) {
                if (expect.die == 0) {
                    expect.health = std::max(0.f, expect.health - damage);
                    ++expect.hits;
                    if (!(expect.health > 0.f)) {
                        expect.health = 0.f;
                        expect.die = frame;
                    }
                }
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            const float below = bomb_height_at(heights, height_z0, height_step, shot.z, inside);
            if (shot.y < below || shot.y < 0.f) {
                ++expect.ground;
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            shot.vy += gravity;
            shot.x += shot.vx;
            shot.y += shot.vy;
            shot.z += shot.vz;
            if (--shot.ttl == 0) {
                ++expect.ground;
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }
    expect.plane_x = plane_x;
    expect.plane_y = plane_y;
    expect.plane_z = plane_z;
    expect.inside = inside;
    return expect;
}

int gate_m5bomb(const Options& options) {
    const std::string trace_path = options.actual + "/m5bomb/duel_bomb.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double goal_x = 0;
    double goal_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double burst = 0;
    double burstrate = 0;
    double file_speed = 0;
    double file_acc = 0;
    double cruise = 0;
    double pitch = 0;
    double sim_hz = 0;
    double gravity_tag = 0;
    double hp_slop = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "bb_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "bb_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "bb_commander_x", commander_x) &&
        toml_number(tolerance_text, "bb_commander_z", commander_z) &&
        toml_number(tolerance_text, "bb_shooter_x", shooter_x) && toml_number(tolerance_text, "bb_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "bb_goal_x", goal_x) && toml_number(tolerance_text, "bb_goal_z", goal_z) &&
        toml_number(tolerance_text, "bb_target_x", target_x) && toml_number(tolerance_text, "bb_target_z", target_z) &&
        toml_number(tolerance_text, "bb_pos_slop", pos_slop) && toml_number(tolerance_text, "bb_health", health) &&
        toml_number(tolerance_text, "bb_damage", damage) && toml_number(tolerance_text, "bb_file_range", file_range) &&
        toml_number(tolerance_text, "bb_reload", reload) && toml_number(tolerance_text, "bb_burst", burst) &&
        toml_number(tolerance_text, "bb_burstrate", burstrate) && toml_number(tolerance_text, "bb_speed", file_speed) &&
        toml_number(tolerance_text, "bb_acc", file_acc) && toml_number(tolerance_text, "bb_cruise", cruise) &&
        toml_number(tolerance_text, "bb_pitch", pitch) && toml_number(tolerance_text, "bb_sim_hz", sim_hz) &&
        toml_number(tolerance_text, "bb_gravity", gravity_tag) && toml_number(tolerance_text, "bb_hp_slop", hp_slop) &&
        toml_number(tolerance_text, "bb_target_ox", target_ox) && toml_number(tolerance_text, "bb_target_oy", target_oy) &&
        toml_number(tolerance_text, "bb_target_oz", target_oz) && toml_number(tolerance_text, "bb_target_sx", target_sx) &&
        toml_number(tolerance_text, "bb_target_sy", target_sy) && toml_number(tolerance_text, "bb_target_sz", target_sz);
    std::vector<BombSample> samples;
    const bool trace_ok = tolerances_ok && read_bomb_samples(trace_path, samples);
    const BombSample* fill = nullptr;
    const BombSample* hold = nullptr;
    for (const BombSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float hz = static_cast<float>(sim_hz);
    const float rate = static_cast<float>(burstrate);
    const float reload_f = static_cast<float>(reload);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const int burst_n = static_cast<int>(static_cast<float>(burst));
    const int delay = static_cast<int>(rate * hz);
    const float max_speed = static_cast<float>(file_speed) / hz;
    const float accel = static_cast<float>(file_acc);
    const float cruise_f = static_cast<float>(cruise);
    const float pitch_f = static_cast<float>(pitch);
    const float grav_f = -std::max(0.001f, static_cast<float>(gravity_tag)) / (hz * hz);
    const BombUnit* commander = nullptr;
    const BombUnit* shooter = nullptr;
    const BombUnit* target = nullptr;
    if (hold != nullptr) {
        for (const BombUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "corshad" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                target = &unit;
            }
        }
    }
    BombExpect fill_expect;
    BombExpect hold_expect;
    const bool strip = fill != nullptr && hold != nullptr && fill->heights.size() > 2 &&
                       fill->heights.size() == hold->heights.size() &&
                       std::fabs(fill->height_z0 - hold->height_z0) <= hp_slop &&
                       std::fabs(fill->height_step - hold->height_step) <= hp_slop;
    if (strip && shooter != nullptr && target != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_bomb(frame, static_cast<float>(shooter_x), static_cast<float>(shooter_z),
                               static_cast<float>(goal_x), static_cast<float>(goal_z), static_cast<float>(target_x),
                               static_cast<float>(target_z), static_cast<float>(target_ox),
                               static_cast<float>(target_oy), static_cast<float>(target_oz),
                               static_cast<float>(target_sx * 0.5), static_cast<float>(target_sy * 0.5),
                               static_cast<float>(target_sz * 0.5), reload_frames, burst_n, delay,
                               static_cast<float>(damage), static_cast<float>(health), max_speed, accel, cruise_f,
                               pitch_f, grav_f, fill->heights, static_cast<float>(fill->height_z0),
                               static_cast<float>(fill->height_step));
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const BombUnit* fill_plane = nullptr;
    if (fill != nullptr) {
        for (const BombUnit& unit : fill->units) {
            if (unit.def == "corshad" && unit.team == 0) {
                fill_plane = &unit;
            }
        }
    }
    const bool placed = commander != nullptr && shooter != nullptr && target != nullptr && fill_plane != nullptr &&
                        hold->units.size() == 3 && std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - hold_expect.plane_x) <= pos_slop &&
                        std::fabs(shooter->y - hold_expect.plane_y) <= pos_slop &&
                        std::fabs(shooter->z - hold_expect.plane_z) <= pos_slop &&
                        std::fabs(fill_plane->x - fill_expect.plane_x) <= pos_slop &&
                        std::fabs(fill_plane->y - fill_expect.plane_y) <= pos_slop &&
                        std::fabs(fill_plane->z - fill_expect.plane_z) <= pos_slop &&
                        std::fabs(target->x - target_x) <= pos_slop && std::fabs(target->z - target_z) <= pos_slop &&
                        std::fabs(target->y - hold_expect.target_y) <= pos_slop && hold_expect.inside &&
                        fill_expect.inside;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(fill->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         static_cast<int>(fill->burst) == burst_n && static_cast<int>(fill->gap) == delay &&
                         std::fabs(fill->speed - max_speed) <= hp_slop && std::fabs(fill->accel - accel) <= hp_slop &&
                         std::fabs(fill->cruise - cruise_f) <= hp_slop && std::fabs(fill->pitch - pitch_f) <= hp_slop &&
                         static_cast<int>(fill->ttl) == 1000 && std::fabs(fill->gravity - grav_f) <= hp_slop &&
                         static_cast<int>(fill->water) == 0 && fill->def == "corshad" && fill->target_def == "armfav";
    const bool fill_ok = numbers && placed && strip && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots &&
                         static_cast<int>(fill->ground) == fill_expect.ground && fill_expect.die == 0 &&
                         fill_expect.shots > 0;
    const bool hold_ok = numbers && placed && strip && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots &&
                         static_cast<int>(hold->ground) == hold_expect.ground && hold_expect.die > 0 &&
                         hold_expect.health == 0.f && static_cast<int>(hold->health) == 0;

    failure << "# duel_bomb\n\n";
    failure << "Scenario: duel_bomb\n\n";
    failure << "Entity: corshad bomb against armfav\n\n";
    failure << "Field: health while bombs are falling\n\n";
    if (fill != nullptr && fill->have_bomb) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " expected "
                << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.die
                << " " << fill_expect.ground << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_bomb) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " expected "
                << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.die
                << " " << hold_expect.ground << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Bomb damage " << damage << " speed " << max_speed << " accel " << accel << " range " << file_range
            << " reload " << reload_frames << " burst " << burst_n << " delay " << delay << " gravity " << grav_f
            << ". The aim script is not hosted. The muzzle is the airplane origin. The aim point is the center of "
               "the target box. A burst stays committed after the drop window opens. Bombs that miss the box and "
               "reach the ground are counted.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || target == nullptr) {
        std::cout << "m5bomb FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " hits " << fill->hits << " shots " << fill->shots << " ground "
              << fill->ground << " expected " << fill_expect.health << " " << fill_expect.hits << " "
              << fill_expect.shots << " " << fill_expect.ground << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " hits " << hold->hits << " ground "
              << hold->ground << " expected " << hold_expect.health << " " << hold_expect.die << " "
              << hold_expect.hits << " " << hold_expect.ground << "\n";
    if (shooter != nullptr) {
        std::cout << "plane " << shooter->x << " " << shooter->y << " " << shooter->z << " expected "
                  << hold_expect.plane_x << " " << hold_expect.plane_y << " " << hold_expect.plane_z << "\n";
    }
    if (!fill_ok) {
        std::cout << "m5bomb FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5bomb FAIL hold\n";
        return 1;
    }
    std::cout << "m5bomb PASS\n";
    failure << "The bomb self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct DgunUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct DgunSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double speed = 0;
    double gravity = 0;
    std::vector<DgunUnit> units;
    bool have_dgun = false;
};

bool read_dgun_samples(const std::string& path, std::vector<DgunSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> shots;
        if (!json_objects(line, "units", units) || !json_objects(line, "dg", shots)) {
            continue;
        }
        DgunSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(shots[0], "def", sample.def) ||
            !json_string_field(shots[0], "td", sample.target_def) ||
            !json_number_field(shots[0], "hp", sample.health) || !json_number_field(shots[0], "die", sample.die) ||
            !json_number_field(shots[0], "hits", sample.hits) || !json_number_field(shots[0], "shots", sample.shots) ||
            !json_number_field(shots[0], "gnd", sample.ground) || !json_number_field(shots[0], "dmg", sample.damage) ||
            !json_number_field(shots[0], "rng", sample.range) || !json_number_field(shots[0], "rel", sample.reload_frames) ||
            !json_number_field(shots[0], "spd", sample.speed) || !json_number_field(shots[0], "grv", sample.gravity)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_dgun = true;
        bool units_ok = true;
        for (const std::string& object : units) {
            DgunUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (units_ok && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct DgunBit {
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float ox = 0;
    float oy = 0;
    float oz = 0;
};

struct DgunExpect {
    float health = 0;
    int die = 0;
    int hits = 0;
    int shots = 0;
    int ground = 0;
};

// Units stay put. The segment is tested, then the shot moves, then gravity is added.
// A hit does not remove the shot. Travel past the weapon range does, and that removal
// is not a ground hit. The gate does not sample terrain; a grounded engine shot fails.
DgunExpect expect_dgun(int sample_frame, float shooter_x, float shooter_y, float shooter_z, float target_x,
                       float target_y, float target_z, float sox, float soy, float soz, float shx, float shy, float shz,
                       float tox, float toy, float toz, float thx, float thy, float thz, float range, int reload_frames,
                       int burst, int delay, float damage, float health, float speed, float gravity) {
    const float mx = shooter_x + sox;
    const float my = shooter_y + soy + shy;
    const float mz = shooter_z + soz;
    const float ax = target_x + tox;
    const float ay = target_y + toy;
    const float az = target_z + toz;
    const float dx = ax - mx;
    const float dy = ay - my;
    const float dz = az - mz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool ready = dist > 0.f && dist <= range && target_y >= 0.f;
    const float inv = dist > 0.f ? 1.f / dist : 0.f;
    const float range2 = range * range;
    DgunExpect expect;
    expect.health = health;
    int salvo_left = 0;
    int reload_status = 0;
    int next_salvo = 0;
    std::vector<DgunBit> live;
    for (int frame = 1; frame <= sample_frame; ++frame) {
        if (expect.die != 0) {
            break;
        }
        if (salvo_left == 0 && next_salvo <= frame && frame >= reload_status && ready) {
            reload_status = frame + reload_frames;
            salvo_left = burst;
            next_salvo = frame;
        }
        if (salvo_left > 0 && next_salvo <= frame) {
            --salvo_left;
            next_salvo = frame + delay;
            ++expect.shots;
            if (!(dist > 0.f) || !(speed > 0.f)) {
                ++expect.ground;
            } else {
                DgunBit shot;
                shot.x = mx;
                shot.y = my;
                shot.z = mz;
                shot.ox = mx;
                shot.oy = my;
                shot.oz = mz;
                shot.vx = dx * inv * speed;
                shot.vy = dy * inv * speed;
                shot.vz = dz * inv * speed;
                live.push_back(shot);
            }
        }
        for (size_t i = 0; i < live.size();) {
            DgunBit& shot = live[i];
            if (flame_segment(shot.x, shot.y, shot.z, shot.vx, shot.vy, shot.vz, ax, ay, az, thx, thy, thz)) {
                if (expect.die == 0) {
                    expect.health = std::max(0.f, expect.health - damage);
                    ++expect.hits;
                    if (!(expect.health > 0.f)) {
                        expect.health = 0.f;
                        expect.die = frame;
                    }
                }
            }
            shot.x += shot.vx;
            shot.y += shot.vy;
            shot.z += shot.vz;
            shot.vy += gravity;
            const float traveled_x = shot.x - shot.ox;
            const float traveled_y = shot.y - shot.oy;
            const float traveled_z = shot.z - shot.oz;
            if (traveled_x * traveled_x + traveled_y * traveled_y + traveled_z * traveled_z > range2) {
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }
    return expect;
}

int gate_m5dgun(const Options& options) {
    const std::string trace_path = options.actual + "/m5dgun/duel_dgun.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double velocity = 0;
    double sim_hz = 0;
    double gravity_tag = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "dg_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "dg_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "dg_commander_x", commander_x) &&
        toml_number(tolerance_text, "dg_commander_z", commander_z) &&
        toml_number(tolerance_text, "dg_shooter_x", shooter_x) && toml_number(tolerance_text, "dg_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "dg_target_x", target_x) && toml_number(tolerance_text, "dg_target_z", target_z) &&
        toml_number(tolerance_text, "dg_pos_slop", pos_slop) && toml_number(tolerance_text, "dg_health", health) &&
        toml_number(tolerance_text, "dg_damage", damage) && toml_number(tolerance_text, "dg_file_range", file_range) &&
        toml_number(tolerance_text, "dg_reload", reload) && toml_number(tolerance_text, "dg_velocity", velocity) &&
        toml_number(tolerance_text, "dg_sim_hz", sim_hz) && toml_number(tolerance_text, "dg_gravity", gravity_tag) &&
        toml_number(tolerance_text, "dg_hp_slop", hp_slop) && toml_number(tolerance_text, "dg_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "dg_shooter_oy", shooter_oy) && toml_number(tolerance_text, "dg_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "dg_shooter_sx", shooter_sx) && toml_number(tolerance_text, "dg_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "dg_shooter_sz", shooter_sz) && toml_number(tolerance_text, "dg_target_ox", target_ox) &&
        toml_number(tolerance_text, "dg_target_oy", target_oy) && toml_number(tolerance_text, "dg_target_oz", target_oz) &&
        toml_number(tolerance_text, "dg_target_sx", target_sx) && toml_number(tolerance_text, "dg_target_sy", target_sy) &&
        toml_number(tolerance_text, "dg_target_sz", target_sz);
    std::vector<DgunSample> samples;
    const bool trace_ok = tolerances_ok && read_dgun_samples(trace_path, samples);
    const DgunSample* fill = nullptr;
    const DgunSample* hold = nullptr;
    for (const DgunSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float hz = static_cast<float>(sim_hz);
    const float reload_f = static_cast<float>(reload);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const float speed = static_cast<float>(velocity) / hz;
    const float grav_f = -std::max(0.001f, static_cast<float>(gravity_tag)) / (hz * hz);
    const DgunUnit* commander = nullptr;
    const DgunUnit* shooter = nullptr;
    const DgunUnit* target = nullptr;
    if (hold != nullptr) {
        for (const DgunUnit& unit : hold->units) {
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "corjugg" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                target = &unit;
            }
        }
    }
    const DgunUnit* fill_shooter = nullptr;
    const DgunUnit* fill_target = nullptr;
    if (fill != nullptr) {
        for (const DgunUnit& unit : fill->units) {
            if (unit.def == "corjugg" && unit.team == 0) {
                fill_shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                fill_target = &unit;
            }
        }
    }
    DgunExpect fill_expect;
    DgunExpect hold_expect;
    if (shooter != nullptr && target != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_dgun(frame, static_cast<float>(shooter->x), static_cast<float>(shooter->y),
                               static_cast<float>(shooter->z), static_cast<float>(target->x),
                               static_cast<float>(target->y), static_cast<float>(target->z),
                               static_cast<float>(shooter_ox), static_cast<float>(shooter_oy),
                               static_cast<float>(shooter_oz), static_cast<float>(shooter_sx * 0.5),
                               static_cast<float>(shooter_sy * 0.5), static_cast<float>(shooter_sz * 0.5),
                               static_cast<float>(target_ox), static_cast<float>(target_oy),
                               static_cast<float>(target_oz), static_cast<float>(target_sx * 0.5),
                               static_cast<float>(target_sy * 0.5), static_cast<float>(target_sz * 0.5),
                               static_cast<float>(file_range), reload_frames, 1, 0, static_cast<float>(damage),
                               static_cast<float>(health), speed, grav_f);
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const bool still = fill_shooter != nullptr && fill_target != nullptr && shooter != nullptr && target != nullptr &&
                       std::fabs(fill_shooter->x - shooter->x) <= pos_slop &&
                       std::fabs(fill_shooter->y - shooter->y) <= pos_slop &&
                       std::fabs(fill_shooter->z - shooter->z) <= pos_slop &&
                       std::fabs(fill_target->x - target->x) <= pos_slop &&
                       std::fabs(fill_target->y - target->y) <= pos_slop &&
                       std::fabs(fill_target->z - target->z) <= pos_slop;
    const bool placed = commander != nullptr && shooter != nullptr && target != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(target->x - target_x) <= pos_slop && std::fabs(target->z - target_z) <= pos_slop &&
                        shooter->y > 0.0 && target->y > 0.0 && still;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(hold->damage - damage) <= hp_slop && std::fabs(fill->range - file_range) <= hp_slop &&
                         std::fabs(hold->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         static_cast<int>(hold->reload_frames) == reload_frames &&
                         std::fabs(fill->speed - speed) <= hp_slop && std::fabs(hold->speed - speed) <= hp_slop &&
                         std::fabs(fill->gravity - grav_f) <= hp_slop && std::fabs(hold->gravity - grav_f) <= hp_slop &&
                         fill->def == "corjugg" && fill->target_def == "armfav" && hold->def == "corjugg" &&
                         hold->target_def == "armfav";
    const bool fill_ok = numbers && placed && std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && static_cast<int>(fill->ground) == 0 &&
                         fill_expect.ground == 0 && fill_expect.die > 0 && fill_expect.health == 0.f &&
                         static_cast<int>(fill->health) == 0 && fill_expect.shots > 0;
    const bool frozen = hold_expect.die == fill_expect.die && hold_expect.hits == fill_expect.hits &&
                        hold_expect.shots == fill_expect.shots && hold_expect.ground == fill_expect.ground;
    const bool hold_ok = numbers && placed && frozen && std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && static_cast<int>(hold->ground) == 0 &&
                         hold_expect.ground == 0 && hold_expect.die > 0 && hold_expect.health == 0.f &&
                         static_cast<int>(hold->health) == 0;

    failure << "# duel_dgun\n\n";
    failure << "Scenario: duel_dgun\n\n";
    failure << "Entity: corjugg dgun against armfav\n\n";
    failure << "Field: health after the fireball has already hit\n\n";
    if (fill != nullptr && fill->have_dgun) {
        failure << "Frame " << fill->frame << " health " << fill->health << " hits " << fill->hits << " shots "
                << fill->shots << " die " << fill->die << " ground " << fill->ground << " expected "
                << fill_expect.health << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.die
                << " " << fill_expect.ground << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_dgun) {
        failure << "Frame " << hold->frame << " health " << hold->health << " hits " << hold->hits << " shots "
                << hold->shots << " die " << hold->die << " ground " << hold->ground << " expected "
                << hold_expect.health << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.die
                << " " << hold_expect.ground << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "DGun damage " << damage << " speed " << speed << " range " << file_range << " reload " << reload_frames
            << " gravity " << grav_f
            << ". The aim script is not hosted. The muzzle is the top of the attacker cylinder treated as a box. "
               "The aim point is the center of the target box. A hit does not remove the shot. The shot stops when "
               "the target dies. Ground contact would fail this gate.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || target == nullptr) {
        std::cout << "m5dgun FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " die " << fill->die << " hits " << fill->hits << " shots "
              << fill->shots << " ground " << fill->ground << " expected " << fill_expect.health << " "
              << fill_expect.die << " " << fill_expect.hits << " " << fill_expect.shots << " " << fill_expect.ground
              << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " hits " << hold->hits << " shots "
              << hold->shots << " ground " << hold->ground << " expected " << hold_expect.health << " "
              << hold_expect.die << " " << hold_expect.hits << " " << hold_expect.shots << " " << hold_expect.ground
              << "\n";
    if (!fill_ok) {
        std::cout << "m5dgun FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5dgun FAIL hold\n";
        return 1;
    }
    std::cout << "m5dgun PASS\n";
    failure << "The dgun self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct LosUnit {
    std::string def;
    int team = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct LosOne {
    std::string tag;
    std::string attacker_def;
    std::string target_def;
    int attacker = -1;
    int target = -1;
    int see = 0;
    int far = 0;
    int weapon = 0;
    double dist = 0;
    double sight = 0;
    double square = 0;
    double attacker_lift = 0;
    double target_lift = 0;
    std::vector<float> heights;
};

struct LosSample {
    int frame = 0;
    bool have_beam = false;
    std::vector<LosOne> checks;
    std::vector<LosUnit> units;
};

bool read_los_samples(const std::string& path, std::vector<LosSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> checks;
        if (!json_objects(line, "units", units) || !json_objects(line, "lo", checks)) {
            continue;
        }
        LosSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_beam = line.find("\"bm\":[") != std::string::npos;
        bool checks_ok = true;
        for (const std::string& object : checks) {
            LosOne one;
            double attacker = 0;
            double target = 0;
            double see = 0;
            double far = 0;
            double weapon = 0;
            std::vector<double> heights;
            if (!json_string_field(object, "tag", one.tag) || !json_string_field(object, "ad", one.attacker_def) ||
                !json_string_field(object, "td", one.target_def) || !json_number_field(object, "ai", attacker) ||
                !json_number_field(object, "ti", target) || !json_number_field(object, "see", see) ||
                !json_number_field(object, "far", far) || !json_number_field(object, "wpn", weapon) ||
                !json_number_field(object, "dist", one.dist) || !json_number_field(object, "sight", one.sight) ||
                !json_number_field(object, "sq", one.square) || !json_number_field(object, "al", one.attacker_lift) ||
                !json_number_field(object, "tl", one.target_lift) || !json_number_array(object, "gh", heights)) {
                checks_ok = false;
                break;
            }
            one.attacker = static_cast<int>(attacker);
            one.target = static_cast<int>(target);
            one.see = static_cast<int>(see);
            one.far = static_cast<int>(far);
            one.weapon = static_cast<int>(weapon);
            one.heights.reserve(heights.size());
            for (double height : heights) {
                one.heights.push_back(static_cast<float>(height));
            }
            sample.checks.push_back(one);
        }
        bool units_ok = true;
        for (const std::string& object : units) {
            LosUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                units_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        if (checks_ok && units_ok && !sample.checks.empty() && !sample.units.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

struct LosExpect {
    int see = 0;
    int far = 0;
    bool blocked = false;
    bool count_ok = false;
    float dist = 0;
};

LosExpect expect_los(float ax, float az, float tx, float tz, float eye_s, float eye_t, float sight, float square,
                     const std::vector<float>& heights) {
    LosExpect expect;
    const float dx = tx - ax;
    const float dz = tz - az;
    expect.dist = std::sqrt(dx * dx + dz * dz);
    expect.far = expect.dist > sight ? 1 : 0;
    const int steps = expect.dist > 0.f ? static_cast<int>(std::floor(expect.dist / square)) : 0;
    const int expect_n = std::max(0, steps - 1);
    expect.count_ok = static_cast<int>(heights.size()) == expect_n;
    if (expect.count_ok) {
        for (int i = 1; i < steps; ++i) {
            const float t = (static_cast<float>(i) * square) / expect.dist;
            const float line = eye_s + (eye_t - eye_s) * t;
            if (heights[static_cast<size_t>(i - 1)] > line) {
                expect.blocked = true;
            }
        }
    }
    expect.see = expect.far != 0 || expect.blocked ? 0 : 1;
    return expect;
}

const LosOne* los_tagged(const LosSample& sample, const char* tag) {
    const LosOne* found = nullptr;
    for (const LosOne& one : sample.checks) {
        if (one.tag == tag) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &one;
        }
    }
    return found;
}

int gate_los(const Options& options) {
    const std::string trace_path = options.actual + "/los/los.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double pos_slop = 0;
    double hp_slop = 0;
    double sight = 0;
    double mip = 0;
    double map_square = 0;
    double box_oy = 0;
    double box_sy = 0;
    double open_ax = 0;
    double open_az = 0;
    double open_tx = 0;
    double open_tz = 0;
    double block_ax = 0;
    double block_az = 0;
    double block_tx = 0;
    double block_tz = 0;
    double far_ax = 0;
    double far_az = 0;
    double far_tx = 0;
    double far_tz = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "lo_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "lo_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "lo_commander_x", commander_x) &&
        toml_number(tolerance_text, "lo_commander_z", commander_z) &&
        toml_number(tolerance_text, "lo_pos_slop", pos_slop) && toml_number(tolerance_text, "lo_hp_slop", hp_slop) &&
        toml_number(tolerance_text, "lo_sight", sight) && toml_number(tolerance_text, "lo_mip", mip) &&
        toml_number(tolerance_text, "lo_map_square", map_square) && toml_number(tolerance_text, "lo_oy", box_oy) &&
        toml_number(tolerance_text, "lo_sy", box_sy) && toml_number(tolerance_text, "lo_open_ax", open_ax) &&
        toml_number(tolerance_text, "lo_open_az", open_az) && toml_number(tolerance_text, "lo_open_tx", open_tx) &&
        toml_number(tolerance_text, "lo_open_tz", open_tz) && toml_number(tolerance_text, "lo_block_ax", block_ax) &&
        toml_number(tolerance_text, "lo_block_az", block_az) && toml_number(tolerance_text, "lo_block_tx", block_tx) &&
        toml_number(tolerance_text, "lo_block_tz", block_tz) && toml_number(tolerance_text, "lo_far_ax", far_ax) &&
        toml_number(tolerance_text, "lo_far_az", far_az) && toml_number(tolerance_text, "lo_far_tx", far_tx) &&
        toml_number(tolerance_text, "lo_far_tz", far_tz);
    std::vector<LosSample> samples;
    const bool trace_ok = tolerances_ok && read_los_samples(trace_path, samples);
    const LosSample* fill = nullptr;
    const LosSample* hold = nullptr;
    for (const LosSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const int mip_i = static_cast<int>(static_cast<float>(mip));
    const float square = static_cast<float>(map_square) * static_cast<float>(1 << std::max(0, mip_i));
    const float lift = static_cast<float>(box_oy) + static_cast<float>(box_sy) * 0.5f;
    const float sight_f = static_cast<float>(sight);
    const auto placed_pair = [&](const LosSample& sample, const LosOne& one, double ax, double az, double tx,
                                 double tz) {
        if (one.attacker < 0 || one.target < 0 || one.attacker >= static_cast<int>(sample.units.size()) ||
            one.target >= static_cast<int>(sample.units.size())) {
            return false;
        }
        const LosUnit& attacker = sample.units[static_cast<size_t>(one.attacker)];
        const LosUnit& target = sample.units[static_cast<size_t>(one.target)];
        return attacker.def == "armfav" && target.def == "armfav" && attacker.team == 0 && target.team == 1 &&
               attacker.y > 0.0 && target.y > 0.0 && std::fabs(attacker.x - ax) <= pos_slop &&
               std::fabs(attacker.z - az) <= pos_slop && std::fabs(target.x - tx) <= pos_slop &&
               std::fabs(target.z - tz) <= pos_slop && one.attacker_def == "armfav" && one.target_def == "armfav" &&
               std::fabs(one.sight - sight) <= hp_slop && std::fabs(one.square - square) <= hp_slop &&
               std::fabs(one.attacker_lift - lift) <= hp_slop && std::fabs(one.target_lift - lift) <= hp_slop;
    };
    const auto expect_of = [&](const LosSample& sample, const LosOne& one) {
        const LosUnit& attacker = sample.units[static_cast<size_t>(one.attacker)];
        const LosUnit& target = sample.units[static_cast<size_t>(one.target)];
        const float eye_s = static_cast<float>(attacker.y) + static_cast<float>(one.attacker_lift);
        const float eye_t = static_cast<float>(target.y) + static_cast<float>(one.target_lift);
        return expect_los(static_cast<float>(attacker.x), static_cast<float>(attacker.z), static_cast<float>(target.x),
                          static_cast<float>(target.z), eye_s, eye_t, sight_f, square, one.heights);
    };
    bool fill_ok = false;
    bool hold_ok = false;
    LosExpect fill_open_expect;
    LosExpect fill_block_expect;
    LosExpect fill_far_expect;
    LosExpect hold_open_expect;
    LosExpect hold_block_expect;
    LosExpect hold_far_expect;
    const LosOne* fill_open = nullptr;
    const LosOne* fill_block = nullptr;
    const LosOne* fill_far = nullptr;
    const LosOne* hold_open = nullptr;
    const LosOne* hold_block = nullptr;
    const LosOne* hold_far = nullptr;
    const auto grade = [&](const LosSample& sample, const LosOne* open, const LosOne* block, const LosOne* far,
                           LosExpect& open_expect, LosExpect& block_expect, LosExpect& far_expect) {
        if (open == nullptr || block == nullptr || far == nullptr || sample.units.size() != 7 || !sample.have_beam) {
            return false;
        }
        if (sample.units[0].def != "armcom" || sample.units[0].team != 0 ||
            std::fabs(sample.units[0].x - commander_x) > pos_slop ||
            std::fabs(sample.units[0].z - commander_z) > pos_slop) {
            return false;
        }
        if (!placed_pair(sample, *open, open_ax, open_az, open_tx, open_tz) ||
            !placed_pair(sample, *block, block_ax, block_az, block_tx, block_tz) ||
            !placed_pair(sample, *far, far_ax, far_az, far_tx, far_tz)) {
            return false;
        }
        open_expect = expect_of(sample, *open);
        block_expect = expect_of(sample, *block);
        far_expect = expect_of(sample, *far);
        const bool open_ok = open->see == 1 && open->far == 0 && open->weapon == 1 && open_expect.see == 1 &&
                             open_expect.far == 0 && !open_expect.blocked && open_expect.count_ok &&
                             std::fabs(open->dist - open_expect.dist) <= hp_slop;
        const bool block_ok = block->see == 0 && block->far == 0 && block->weapon == 0 && block_expect.see == 0 &&
                              block_expect.far == 0 && block_expect.blocked && block_expect.count_ok &&
                              std::fabs(block->dist - block_expect.dist) <= hp_slop;
        const bool far_ok = far->see == 0 && far->far == 1 && far->weapon == 0 && far_expect.see == 0 &&
                            far_expect.far == 1 && far_expect.count_ok && far_expect.dist > sight_f &&
                            std::fabs(far->dist - far_expect.dist) <= hp_slop;
        return open_ok && block_ok && far_ok;
    };
    if (fill != nullptr) {
        fill_open = los_tagged(*fill, "open");
        fill_block = los_tagged(*fill, "block");
        fill_far = los_tagged(*fill, "far");
        fill_ok = grade(*fill, fill_open, fill_block, fill_far, fill_open_expect, fill_block_expect, fill_far_expect);
    }
    if (hold != nullptr) {
        hold_open = los_tagged(*hold, "open");
        hold_block = los_tagged(*hold, "block");
        hold_far = los_tagged(*hold, "far");
        hold_ok = grade(*hold, hold_open, hold_block, hold_far, hold_open_expect, hold_block_expect, hold_far_expect);
    }
    const bool still = fill_ok && hold_ok && fill->units.size() == hold->units.size();
    bool frozen = still;
    if (still) {
        for (size_t i = 0; i < fill->units.size(); ++i) {
            if (std::fabs(fill->units[i].x - hold->units[i].x) > pos_slop ||
                std::fabs(fill->units[i].y - hold->units[i].y) > pos_slop ||
                std::fabs(fill->units[i].z - hold->units[i].z) > pos_slop) {
                frozen = false;
            }
        }
        frozen = frozen && fill_open->see == hold_open->see && fill_block->see == hold_block->see &&
                 fill_far->see == hold_far->see && fill_open->weapon == hold_open->weapon &&
                 fill_block->weapon == hold_block->weapon && fill_far->weapon == hold_far->weapon;
    }

    failure << "# los\n\n";
    failure << "Scenario: los\n\n";
    failure << "Entity: armfav ground sight\n\n";
    failure << "Field: a unit behind the ridge or past sightdistance is not targetable\n\n";
    if (fill_open != nullptr && fill_block != nullptr && fill_far != nullptr) {
        failure << "Frame " << fill->frame << " open see " << fill_open->see << " block see " << fill_block->see
                << " far see " << fill_far->see << " expected " << fill_open_expect.see << " " << fill_block_expect.see
                << " " << fill_far_expect.see << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold_open != nullptr && hold_block != nullptr && hold_far != nullptr) {
        failure << "Frame " << hold->frame << " open see " << hold_open->see << " block see " << hold_block->see
                << " far see " << hold_far->see << " expected " << hold_open_expect.see << " " << hold_block_expect.see
                << " " << hold_far_expect.see << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Sight is " << sight << " and the los square is " << square
            << ". The eye is the top of the armfav box. A clear pair inside that sight may fire. A pair inside "
               "sight whose terrain rises through the eye line may not. A pair past sightdistance may not. Air LOS "
               "is not hosted.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr) {
        std::cout << "los FAIL trace\n";
        return 1;
    }
    if (fill_open != nullptr && fill_block != nullptr && fill_far != nullptr) {
        std::cout << "fill open " << fill_open->see << " block " << fill_block->see << " far " << fill_far->see
                  << " expected " << fill_open_expect.see << " " << fill_block_expect.see << " " << fill_far_expect.see
                  << "\n";
    }
    if (hold_open != nullptr && hold_block != nullptr && hold_far != nullptr) {
        std::cout << "hold open " << hold_open->see << " block " << hold_block->see << " far " << hold_far->see
                  << " expected " << hold_open_expect.see << " " << hold_block_expect.see << " " << hold_far_expect.see
                  << "\n";
    }
    if (!fill_ok || !frozen) {
        std::cout << "los FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "los FAIL hold\n";
        return 1;
    }
    std::cout << "los PASS\n";
    failure << "The los self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

struct DeathWreckBit {
    std::string def;
    double id = 0;
    double born = 0;
    double x = 0;
    double y = 0;
    double z = 0;
    double metal = 0;
    double cost = 0;
    double ratio = 0;
};

struct WreckDuelSample {
    int frame = 0;
    std::string def;
    std::string target_def;
    double health = 0;
    double die = 0;
    double hits = 0;
    double shots = 0;
    double ground = 0;
    double damage = 0;
    double range = 0;
    double reload_frames = 0;
    double speed = 0;
    double gravity = 0;
    double team_metal = 0;
    std::vector<DgunUnit> units;
    std::vector<DeathWreckBit> wrecks;
    bool have_dgun = false;
    bool have_team = false;
};

bool read_wreck_samples(const std::string& path, std::vector<WreckDuelSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> units;
        std::vector<std::string> shots;
        std::vector<std::string> bodies;
        std::vector<std::string> teams;
        if (!json_objects(line, "units", units) || !json_objects(line, "dg", shots) ||
            !json_objects(line, "dw", bodies) || !json_objects(line, "teams", teams)) {
            continue;
        }
        WreckDuelSample sample;
        double frame = 0;
        if (!json_number_field(line, "f", frame) || !json_string_field(shots[0], "def", sample.def) ||
            !json_string_field(shots[0], "td", sample.target_def) ||
            !json_number_field(shots[0], "hp", sample.health) || !json_number_field(shots[0], "die", sample.die) ||
            !json_number_field(shots[0], "hits", sample.hits) || !json_number_field(shots[0], "shots", sample.shots) ||
            !json_number_field(shots[0], "gnd", sample.ground) || !json_number_field(shots[0], "dmg", sample.damage) ||
            !json_number_field(shots[0], "rng", sample.range) || !json_number_field(shots[0], "rel", sample.reload_frames) ||
            !json_number_field(shots[0], "spd", sample.speed) || !json_number_field(shots[0], "grv", sample.gravity) ||
            !json_number_field(teams[0], "m", sample.team_metal)) {
            continue;
        }
        sample.frame = static_cast<int>(frame);
        sample.have_dgun = true;
        sample.have_team = true;
        bool rows_ok = true;
        for (const std::string& object : units) {
            DgunUnit unit;
            double team = 0;
            if (!json_string_field(object, "def", unit.def) || !json_number_field(object, "t", team) ||
                !json_number_field(object, "x", unit.x) || !json_number_field(object, "y", unit.y) ||
                !json_number_field(object, "z", unit.z)) {
                rows_ok = false;
                break;
            }
            unit.team = static_cast<int>(team);
            sample.units.push_back(unit);
        }
        for (const std::string& object : bodies) {
            DeathWreckBit body;
            if (!json_string_field(object, "def", body.def) || !json_number_field(object, "id", body.id) ||
                !json_number_field(object, "born", body.born) || !json_number_field(object, "x", body.x) ||
                !json_number_field(object, "y", body.y) || !json_number_field(object, "z", body.z) ||
                !json_number_field(object, "mt", body.metal) || !json_number_field(object, "cost", body.cost) ||
                !json_number_field(object, "ratio", body.ratio)) {
                rows_ok = false;
                break;
            }
            sample.wrecks.push_back(body);
        }
        if (rows_ok && !sample.units.empty() && !sample.wrecks.empty()) {
            out.push_back(sample);
        }
    }
    return !out.empty();
}

// Same floor(metalcost * ratio) as wreck_metal_amount. The multiply is in double
// after the costs have been narrowed to float32, matching the sim.
float wreck_metal_closed(double cost, double ratio) {
    const float cost_f = static_cast<float>(cost);
    const float ratio_f = static_cast<float>(ratio);
    const double raw = static_cast<double>(cost_f) * static_cast<double>(ratio_f);
    return static_cast<float>(std::floor(raw));
}

int gate_m5wreck(const Options& options) {
    const std::string trace_path = options.actual + "/m5wreck/duel_wreck.jsonl";
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::ostringstream failure;
    double fill_frame = 0;
    double hold_frame = 0;
    double commander_x = 0;
    double commander_z = 0;
    double shooter_x = 0;
    double shooter_z = 0;
    double target_x = 0;
    double target_z = 0;
    double pos_slop = 0;
    double health = 0;
    double damage = 0;
    double file_range = 0;
    double reload = 0;
    double velocity = 0;
    double sim_hz = 0;
    double gravity_tag = 0;
    double hp_slop = 0;
    double shooter_ox = 0;
    double shooter_oy = 0;
    double shooter_oz = 0;
    double shooter_sx = 0;
    double shooter_sy = 0;
    double shooter_sz = 0;
    double target_ox = 0;
    double target_oy = 0;
    double target_oz = 0;
    double target_sx = 0;
    double target_sy = 0;
    double target_sz = 0;
    double metal_cost = 0;
    double wreck_ratio = 0;
    double file_metal = 0;
    double start_metal = 0;
    double income = 0;
    double team_slop = 0;
    std::string tolerance_text;
    const bool tolerances_ok =
        read_text(tolerance_path, tolerance_text) && toml_number(tolerance_text, "dw_fill_frame", fill_frame) &&
        toml_number(tolerance_text, "dw_hold_frame", hold_frame) &&
        toml_number(tolerance_text, "dw_commander_x", commander_x) &&
        toml_number(tolerance_text, "dw_commander_z", commander_z) &&
        toml_number(tolerance_text, "dw_shooter_x", shooter_x) && toml_number(tolerance_text, "dw_shooter_z", shooter_z) &&
        toml_number(tolerance_text, "dw_target_x", target_x) && toml_number(tolerance_text, "dw_target_z", target_z) &&
        toml_number(tolerance_text, "dw_pos_slop", pos_slop) && toml_number(tolerance_text, "dw_health", health) &&
        toml_number(tolerance_text, "dw_damage", damage) && toml_number(tolerance_text, "dw_file_range", file_range) &&
        toml_number(tolerance_text, "dw_reload", reload) && toml_number(tolerance_text, "dw_velocity", velocity) &&
        toml_number(tolerance_text, "dw_sim_hz", sim_hz) && toml_number(tolerance_text, "dw_gravity", gravity_tag) &&
        toml_number(tolerance_text, "dw_hp_slop", hp_slop) && toml_number(tolerance_text, "dw_shooter_ox", shooter_ox) &&
        toml_number(tolerance_text, "dw_shooter_oy", shooter_oy) &&
        toml_number(tolerance_text, "dw_shooter_oz", shooter_oz) &&
        toml_number(tolerance_text, "dw_shooter_sx", shooter_sx) &&
        toml_number(tolerance_text, "dw_shooter_sy", shooter_sy) &&
        toml_number(tolerance_text, "dw_shooter_sz", shooter_sz) && toml_number(tolerance_text, "dw_target_ox", target_ox) &&
        toml_number(tolerance_text, "dw_target_oy", target_oy) && toml_number(tolerance_text, "dw_target_oz", target_oz) &&
        toml_number(tolerance_text, "dw_target_sx", target_sx) && toml_number(tolerance_text, "dw_target_sy", target_sy) &&
        toml_number(tolerance_text, "dw_target_sz", target_sz) && toml_number(tolerance_text, "dw_cost", metal_cost) &&
        toml_number(tolerance_text, "dw_ratio", wreck_ratio) && toml_number(tolerance_text, "dw_file_metal", file_metal) &&
        toml_number(tolerance_text, "dw_start_metal", start_metal) && toml_number(tolerance_text, "dw_income", income) &&
        toml_number(tolerance_text, "dw_team_slop", team_slop);
    std::vector<WreckDuelSample> samples;
    const bool trace_ok = tolerances_ok && read_wreck_samples(trace_path, samples);
    const WreckDuelSample* fill = nullptr;
    const WreckDuelSample* hold = nullptr;
    for (const WreckDuelSample& sample : samples) {
        if (sample.frame == static_cast<int>(fill_frame)) {
            fill = &sample;
        }
        if (sample.frame == static_cast<int>(hold_frame)) {
            hold = &sample;
        }
    }
    const float hz = static_cast<float>(sim_hz);
    const float reload_f = static_cast<float>(reload);
    const int reload_frames = std::max(1, static_cast<int>(reload_f * hz));
    const float speed = static_cast<float>(velocity) / hz;
    const float grav_f = -std::max(0.001f, static_cast<float>(gravity_tag)) / (hz * hz);
    const float expect_metal = wreck_metal_closed(metal_cost, wreck_ratio);
    const DgunUnit* commander = nullptr;
    const DgunUnit* shooter = nullptr;
    const DgunUnit* target = nullptr;
    int target_index = -1;
    if (hold != nullptr) {
        for (size_t i = 0; i < hold->units.size(); ++i) {
            const DgunUnit& unit = hold->units[i];
            if (unit.def == "armcom" && unit.team == 0) {
                commander = &unit;
            } else if (unit.def == "corjugg" && unit.team == 0) {
                shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                target = &unit;
                target_index = static_cast<int>(i);
            }
        }
    }
    const DgunUnit* fill_shooter = nullptr;
    const DgunUnit* fill_target = nullptr;
    if (fill != nullptr) {
        for (const DgunUnit& unit : fill->units) {
            if (unit.def == "corjugg" && unit.team == 0) {
                fill_shooter = &unit;
            } else if (unit.def == "armfav" && unit.team == 1) {
                fill_target = &unit;
            }
        }
    }
    DgunExpect fill_expect;
    DgunExpect hold_expect;
    if (shooter != nullptr && target != nullptr) {
        const auto expect_at = [&](int frame) {
            return expect_dgun(frame, static_cast<float>(shooter->x), static_cast<float>(shooter->y),
                               static_cast<float>(shooter->z), static_cast<float>(target->x),
                               static_cast<float>(target->y), static_cast<float>(target->z),
                               static_cast<float>(shooter_ox), static_cast<float>(shooter_oy),
                               static_cast<float>(shooter_oz), static_cast<float>(shooter_sx * 0.5),
                               static_cast<float>(shooter_sy * 0.5), static_cast<float>(shooter_sz * 0.5),
                               static_cast<float>(target_ox), static_cast<float>(target_oy),
                               static_cast<float>(target_oz), static_cast<float>(target_sx * 0.5),
                               static_cast<float>(target_sy * 0.5), static_cast<float>(target_sz * 0.5),
                               static_cast<float>(file_range), reload_frames, 1, 0, static_cast<float>(damage),
                               static_cast<float>(health), speed, grav_f);
        };
        fill_expect = expect_at(static_cast<int>(fill_frame));
        hold_expect = expect_at(static_cast<int>(hold_frame));
    }
    const bool still = fill_shooter != nullptr && fill_target != nullptr && shooter != nullptr && target != nullptr &&
                       std::fabs(fill_shooter->x - shooter->x) <= pos_slop &&
                       std::fabs(fill_shooter->y - shooter->y) <= pos_slop &&
                       std::fabs(fill_shooter->z - shooter->z) <= pos_slop &&
                       std::fabs(fill_target->x - target->x) <= pos_slop &&
                       std::fabs(fill_target->y - target->y) <= pos_slop &&
                       std::fabs(fill_target->z - target->z) <= pos_slop;
    const bool placed = commander != nullptr && shooter != nullptr && target != nullptr && hold->units.size() == 3 &&
                        std::fabs(commander->x - commander_x) <= pos_slop &&
                        std::fabs(commander->z - commander_z) <= pos_slop &&
                        std::fabs(shooter->x - shooter_x) <= pos_slop && std::fabs(shooter->z - shooter_z) <= pos_slop &&
                        std::fabs(target->x - target_x) <= pos_slop && std::fabs(target->z - target_z) <= pos_slop &&
                        shooter->y > 0.0 && target->y > 0.0 && still;
    const bool numbers = fill != nullptr && hold != nullptr && std::fabs(fill->damage - damage) <= hp_slop &&
                         std::fabs(hold->damage - damage) <= hp_slop && std::fabs(fill->range - file_range) <= hp_slop &&
                         std::fabs(hold->range - file_range) <= hp_slop &&
                         static_cast<int>(fill->reload_frames) == reload_frames &&
                         static_cast<int>(hold->reload_frames) == reload_frames &&
                         std::fabs(fill->speed - speed) <= hp_slop && std::fabs(hold->speed - speed) <= hp_slop &&
                         std::fabs(fill->gravity - grav_f) <= hp_slop && std::fabs(hold->gravity - grav_f) <= hp_slop &&
                         fill->def == "corjugg" && fill->target_def == "armfav" && hold->def == "corjugg" &&
                         hold->target_def == "armfav";
    auto stock_ok = [&](const WreckDuelSample& sample) {
        const double want = start_metal + income * static_cast<double>(sample.frame) / sim_hz;
        const double paid = want + static_cast<double>(expect_metal);
        return sample.have_team && std::fabs(sample.team_metal - want) <= team_slop &&
               std::fabs(sample.team_metal - paid) > team_slop;
    };
    auto corpse_ok = [&](const WreckDuelSample& sample, const DgunExpect& expect) {
        if (sample.wrecks.size() != 1 || target == nullptr || target_index < 0) {
            return false;
        }
        const DeathWreckBit& body = sample.wrecks[0];
        const bool identity = body.def == "armfav" && static_cast<int>(body.id) == target_index &&
                              std::fabs(body.cost - metal_cost) <= hp_slop &&
                              std::fabs(body.ratio - wreck_ratio) <= hp_slop &&
                              std::fabs(body.metal - static_cast<double>(expect_metal)) <= hp_slop &&
                              std::fabs(body.metal - file_metal) > hp_slop;
        const bool when = static_cast<int>(body.born) == expect.die && body.born > 0.0 && body.born < fill_frame;
        const bool where = std::fabs(body.x - target->x) <= pos_slop && std::fabs(body.y - target->y) <= pos_slop &&
                           std::fabs(body.z - target->z) <= pos_slop;
        return identity && when && where && stock_ok(sample);
    };
    const bool fill_ok = numbers && placed && fill->units.size() == 3 &&
                         std::fabs(fill->health - fill_expect.health) <= hp_slop &&
                         static_cast<int>(fill->die) == fill_expect.die &&
                         static_cast<int>(fill->hits) == fill_expect.hits &&
                         static_cast<int>(fill->shots) == fill_expect.shots && static_cast<int>(fill->ground) == 0 &&
                         fill_expect.ground == 0 && fill_expect.die > 0 && fill_expect.health == 0.f &&
                         static_cast<int>(fill->health) == 0 && fill_expect.shots > 0 &&
                         corpse_ok(*fill, fill_expect);
    const bool frozen_shot = hold_expect.die == fill_expect.die && hold_expect.hits == fill_expect.hits &&
                             hold_expect.shots == fill_expect.shots && hold_expect.ground == fill_expect.ground;
    const bool frozen_body =
        fill != nullptr && hold != nullptr && fill->wrecks.size() == 1 && hold->wrecks.size() == 1 &&
        fill->wrecks[0].def == hold->wrecks[0].def && static_cast<int>(fill->wrecks[0].id) == static_cast<int>(hold->wrecks[0].id) &&
        static_cast<int>(fill->wrecks[0].born) == static_cast<int>(hold->wrecks[0].born) &&
        std::fabs(fill->wrecks[0].metal - hold->wrecks[0].metal) <= hp_slop &&
        std::fabs(fill->wrecks[0].x - hold->wrecks[0].x) <= pos_slop &&
        std::fabs(fill->wrecks[0].y - hold->wrecks[0].y) <= pos_slop &&
        std::fabs(fill->wrecks[0].z - hold->wrecks[0].z) <= pos_slop;
    const bool hold_ok = numbers && placed && frozen_shot && frozen_body &&
                         std::fabs(hold->health - hold_expect.health) <= hp_slop &&
                         static_cast<int>(hold->die) == hold_expect.die &&
                         static_cast<int>(hold->hits) == hold_expect.hits &&
                         static_cast<int>(hold->shots) == hold_expect.shots && static_cast<int>(hold->ground) == 0 &&
                         hold_expect.ground == 0 && hold_expect.die > 0 && hold_expect.health == 0.f &&
                         static_cast<int>(hold->health) == 0 && corpse_ok(*hold, hold_expect);

    failure << "# duel_wreck\n\n";
    failure << "Scenario: duel_wreck\n\n";
    failure << "Entity: armfav corpse after the corjugg dgun\n\n";
    failure << "Field: wreck metal and team metal on the death frame\n\n";
    if (fill != nullptr && fill->have_dgun && !fill->wrecks.empty()) {
        failure << "Frame " << fill->frame << " health " << fill->health << " die " << fill->die << " ground "
                << fill->ground << " wrecks " << fill->wrecks.size() << " metal " << fill->wrecks[0].metal << " born "
                << fill->wrecks[0].born << " team " << fill->team_metal << " expected die " << fill_expect.die
                << " metal " << expect_metal << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    if (hold != nullptr && hold->have_dgun && !hold->wrecks.empty()) {
        failure << "Frame " << hold->frame << " health " << hold->health << " die " << hold->die << " ground "
                << hold->ground << " wrecks " << hold->wrecks.size() << " metal " << hold->wrecks[0].metal << " born "
                << hold->wrecks[0].born << " team " << hold->team_metal << " expected die " << hold_expect.die
                << " metal " << expect_metal << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(hold_frame) << ".\n\n";
    }
    failure << "Wreck metal is floor(metalcost * ratio) = " << expect_metal << ", not the file metal " << file_metal
            << ". The corpse is not a unit and it does not pay the team. Team metal stays on start " << start_metal
            << " plus income " << income << " per second. Death frame comes from the dgun closed form.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!trace_ok || fill == nullptr || hold == nullptr || shooter == nullptr || target == nullptr) {
        std::cout << "m5wreck FAIL trace\n";
        return 1;
    }
    std::cout << "fill health " << fill->health << " die " << fill->die << " born " << fill->wrecks[0].born << " metal "
              << fill->wrecks[0].metal << " team " << fill->team_metal << " expected die " << fill_expect.die
              << " metal " << expect_metal << "\n";
    std::cout << "hold health " << hold->health << " die " << hold->die << " born " << hold->wrecks[0].born << " metal "
              << hold->wrecks[0].metal << " team " << hold->team_metal << " expected die " << hold_expect.die
              << " metal " << expect_metal << "\n";
    if (!fill_ok) {
        std::cout << "m5wreck FAIL fill\n";
        return 1;
    }
    if (!hold_ok) {
        std::cout << "m5wreck FAIL hold\n";
        return 1;
    }
    std::cout << "m5wreck PASS\n";
    failure << "The wreck self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

int gate_m6spawn(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double fill_frame = 0;
    double com_x = 0;
    double com_z = 0;
    double place_slop = 0;
    double stock_metal = 0;
    double stock_energy = 0;
    double store_metal = 0;
    double store_energy = 0;
    double metal_income = 0;
    double energy_income = 0;
    double sim_hz = 0;
    double team_slop = 0;
    double start_pos = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "m6_fill", fill_frame) ||
        !toml_number(tolerance_text, "m6_com_x", com_x) || !toml_number(tolerance_text, "m6_com_z", com_z) ||
        !toml_number(tolerance_text, "m6_place_slop", place_slop) ||
        !toml_number(tolerance_text, "m6_stock_metal", stock_metal) ||
        !toml_number(tolerance_text, "m6_stock_energy", stock_energy) ||
        !toml_number(tolerance_text, "m6_store_metal", store_metal) ||
        !toml_number(tolerance_text, "m6_store_energy", store_energy) ||
        !toml_number(tolerance_text, "m6_metal_income", metal_income) ||
        !toml_number(tolerance_text, "m6_energy_income", energy_income) ||
        !toml_number(tolerance_text, "m6_sim_hz", sim_hz) || !toml_number(tolerance_text, "m6_team_slop", team_slop) ||
        !toml_number(tolerance_text, "m6_start_pos", start_pos)) {
        std::cerr << "gate: m6spawn tolerances are missing\n";
        return 2;
    }

    std::ostringstream failure;
    failure << "# spawn gadgets\n\n";
    failure << "Scenario: boot\n\n";
    failure << "Entity: Armada commander and team stocks\n\n";
    failure << "Field: one bound spawn and the resource gadget's start stocks\n\n";

    const std::string gadget_path = options.actual + "/m6spawn/gadget.json";
    const std::string trace_path = options.actual + "/m6spawn/boot.jsonl";
    std::string gadget_text;
    const bool gadget_ok = read_text(gadget_path, gadget_text);
    std::string def;
    double team = -1;
    double x = 0;
    double y = 0;
    double z = 0;
    double unit = -1;
    double hide = 0;
    double paralyze = 0;
    double movectrl = 0;
    double metal = 0;
    double energy = 0;
    double metal_store = 0;
    double energy_store = 0;
    double pos = -1;
    const bool gadget_fields =
        gadget_ok && json_string_field(gadget_text, "def", def) && json_number_field(gadget_text, "team", team) &&
        json_number_field(gadget_text, "x", x) && json_number_field(gadget_text, "y", y) &&
        json_number_field(gadget_text, "z", z) && json_number_field(gadget_text, "unit", unit) &&
        json_number_field(gadget_text, "hide", hide) && json_number_field(gadget_text, "paralyze", paralyze) &&
        json_number_field(gadget_text, "movectrl", movectrl) && json_number_field(gadget_text, "metal", metal) &&
        json_number_field(gadget_text, "energy", energy) &&
        json_number_field(gadget_text, "metal_store", metal_store) &&
        json_number_field(gadget_text, "energy_store", energy_store) && json_number_field(gadget_text, "pos", pos);

    std::ifstream trace_in(trace_path);
    std::string line;
    std::string sample;
    while (trace_in && std::getline(trace_in, line)) {
        double frame = -1;
        if (!json_number_field(line, "f", frame) || static_cast<int>(frame) != static_cast<int>(fill_frame)) {
            continue;
        }
        sample = line;
        break;
    }
    std::vector<std::string> units;
    std::vector<std::string> teams;
    const bool sample_ok = !sample.empty() && json_objects(sample, "units", units) && json_objects(sample, "teams", teams) &&
                           units.size() == 1 && teams.size() == 1;
    std::string unit_def;
    double unit_team = -1;
    double unit_x = 0;
    double unit_y = 0;
    double unit_z = 0;
    double spd = -1;
    double cmd = -1;
    double team_metal = 0;
    double team_energy = 0;
    double team_ms = 0;
    double team_es = 0;
    if (sample_ok) {
        json_string_field(units[0], "def", unit_def);
        json_number_field(units[0], "t", unit_team);
        json_number_field(units[0], "x", unit_x);
        json_number_field(units[0], "y", unit_y);
        json_number_field(units[0], "z", unit_z);
        json_number_field(units[0], "spd", spd);
        json_number_field(units[0], "cmd", cmd);
        json_number_field(teams[0], "m", team_metal);
        json_number_field(teams[0], "e", team_energy);
        json_number_field(teams[0], "ms", team_ms);
        json_number_field(teams[0], "es", team_es);
    }
    const double want_metal = std::min(store_metal, stock_metal + metal_income * fill_frame / sim_hz);
    const double want_energy = std::min(store_energy, stock_energy + energy_income * fill_frame / sim_hz);
    const bool asked = gadget_fields && def == "armcom" && static_cast<int>(team) == 0 && static_cast<int>(unit) == 0 &&
                       static_cast<int>(hide) == 1 && static_cast<int>(paralyze) == 1 &&
                       static_cast<int>(movectrl) == 1 && static_cast<int>(pos) == static_cast<int>(start_pos) &&
                       std::fabs(x - com_x) <= place_slop && std::fabs(z - com_z) <= place_slop &&
                       std::fabs(metal - stock_metal) <= team_slop && std::fabs(energy - stock_energy) <= team_slop &&
                       std::fabs(metal_store - store_metal) <= team_slop &&
                       std::fabs(energy_store - store_energy) <= team_slop;
    const bool placed = sample_ok && unit_def == "armcom" && static_cast<int>(unit_team) == 0 &&
                        std::fabs(unit_x - com_x) <= place_slop &&
                        std::fabs(unit_z - com_z) <= place_slop && std::fabs(unit_y - y) <= place_slop &&
                        std::fabs(spd) <= place_slop && static_cast<int>(cmd) == 0;
    const bool stocks = sample_ok && std::fabs(team_metal - want_metal) <= team_slop &&
                        std::fabs(team_energy - want_energy) <= team_slop &&
                        std::fabs(team_ms - store_metal) <= team_slop && std::fabs(team_es - store_energy) <= team_slop;

    if (gadget_fields) {
        failure << "Gadget asked for " << def << " team " << static_cast<int>(team) << " unit " << static_cast<int>(unit)
                << " at " << x << " " << y << " " << z << " hide " << static_cast<int>(hide) << " paralyze "
                << static_cast<int>(paralyze) << " movectrl " << static_cast<int>(movectrl) << " metal " << metal
                << " energy " << energy << " store " << metal_store << " " << energy_store << " pos "
                << static_cast<int>(pos) << ".\n\n";
    } else {
        failure << "No gadget report at " << gadget_path << ".\n\n";
    }
    if (sample_ok) {
        failure << "Frame " << static_cast<int>(fill_frame) << " units " << units.size() << " " << unit_def << " at "
                << unit_x << " " << unit_y << " " << unit_z << " metal " << team_metal << " energy " << team_energy
                << " store " << team_ms << " " << team_es << " expected metal " << want_metal << " energy "
                << want_energy << ".\n\n";
    } else {
        failure << "No one-unit sample at frame " << static_cast<int>(fill_frame) << ".\n\n";
    }
    failure << "boot.txt leaves start metal and energy at the modoption default " << stock_metal << " and "
            << stock_energy << ". Storage stays " << store_metal << ". Income that would pass storage is clamped, so "
            << "frame " << static_cast<int>(fill_frame) << " metal is " << want_metal << " and energy is " << want_energy
            << ". CreateUnit binds the commander already placed. Hide, paralyze, and movectrl are recorded and not "
            << "applied.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!gadget_fields || !sample_ok) {
        std::cout << "m6spawn FAIL trace\n";
        return 1;
    }
    std::cout << "ask " << def << " unit " << static_cast<int>(unit) << " hide " << static_cast<int>(hide)
              << " paralyze " << static_cast<int>(paralyze) << " movectrl " << static_cast<int>(movectrl) << " metal "
              << metal << " energy " << energy << "\n";
    std::cout << "frame " << static_cast<int>(fill_frame) << " units " << units.size() << " at " << unit_x << " "
              << unit_y << " " << unit_z << " metal " << team_metal << " energy " << team_energy << " expected "
              << want_metal << " " << want_energy << "\n";
    if (!asked) {
        std::cout << "m6spawn FAIL ask\n";
        return 1;
    }
    if (!placed) {
        std::cout << "m6spawn FAIL place\n";
        return 1;
    }
    if (!stocks) {
        std::cout << "m6spawn FAIL stocks\n";
        return 1;
    }
    std::cout << "m6spawn PASS\n";
    failure << "The spawn self-check passed on engine output. m1 stays red without reference pictures. The M4 "
               "milestone stays red until eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 "
               "milestone stays red until the duel corpus is within 15% time to kill of a Recoil trace. The M6 "
               "milestone stays red until ai_10min holds a 1v1 for ten minutes.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

bool json_string_array(const std::string& text, const char* key, std::vector<std::string>& out) {
    const std::string pattern = std::string("\"") + key + "\":[";
    const auto at = text.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    size_t i = at + pattern.size();
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    while (i < text.size() && text[i] != ']') {
        if (text[i] != '"') {
            ++i;
            continue;
        }
        const auto end = text.find('"', i + 1);
        if (end == std::string::npos) {
            return false;
        }
        out.push_back(text.substr(i + 1, end - (i + 1)));
        i = end + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i < text.size() && text[i] == ',') {
            ++i;
        }
    }
    return i < text.size() && text[i] == ']';
}

int gate_m6behavior(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double min_calls = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "m6b_min_calls", min_calls)) {
        std::cerr << "gate: m6behavior tolerances are missing\n";
        return 2;
    }

    const char* want[] = {
        "Paralyze Damage Multiplier", "No Land Damage",           "unit_combomb_full_damage",
        "Experimental Bots Steps Damages", "D-Gun Behaviour", "Mobile Unit Paralyze Damage Handler",
    };
    constexpr int want_count = 6;

    std::ostringstream failure;
    failure << "# unit behavior gadgets\n\n";
    failure << "Scenario: duel_laser\n\n";
    failure << "Entity: synced UnitPreDamaged chain\n\n";
    failure << "Field: loaded names, call count, and damage rewrites\n\n";

    const std::string path = options.actual + "/m6behavior/behavior.json";
    std::string text;
    const bool text_ok = read_text(path, text);
    double calls = 0;
    std::string deferred;
    std::vector<std::string> names;
    std::vector<std::string> rewrites;
    const bool calls_ok = text_ok && json_number_field(text, "calls", calls);
    const bool deferred_ok = text_ok && json_string_field(text, "deferred", deferred);
    const bool names_ok = text_ok && json_string_array(text, "names", names);
    const bool rewrites_ok = text_ok && json_string_array(text, "rewrites", rewrites);
    bool names_match = names_ok && static_cast<int>(names.size()) == want_count;
    if (names_match) {
        for (int i = 0; i < want_count; ++i) {
            if (names[static_cast<size_t>(i)] != want[i]) {
                names_match = false;
            }
        }
    }
    const bool enough = calls_ok && calls + 0.0001 >= min_calls;
    const bool identity = rewrites_ok && rewrites.empty();
    const bool deferred_match = deferred_ok && deferred == "dgun-projectile";

    if (text_ok) {
        failure << "Report " << text << "\n";
    } else {
        failure << "No behavior report at " << path << ".\n\n";
    }
    failure << "The chain is Paralyze Damage Multiplier, No Land Damage, unit_combomb_full_damage, Experimental "
               "Bots Steps Damages, D-Gun Behaviour, and Mobile Unit Paralyze Damage Handler. A returned damage "
               "that differs is recorded and not applied. Projectile ground-hug stays deferred as dgun-projectile. "
               "The laser health check is gate --suite m5laser.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!text_ok || !calls_ok || !names_ok || !rewrites_ok || !deferred_ok) {
        std::cout << "m6behavior FAIL trace\n";
        return 1;
    }
    std::cout << "calls " << static_cast<int>(calls) << " names " << names.size() << " rewrites " << rewrites.size()
              << " deferred " << deferred << "\n";
    for (const std::string& name : names) {
        std::cout << "gadget " << name << "\n";
    }
    for (const std::string& name : rewrites) {
        std::cout << "rewrite " << name << "\n";
    }
    if (!names_match) {
        std::cout << "m6behavior FAIL names\n";
        return 1;
    }
    if (!enough) {
        std::cout << "m6behavior FAIL calls\n";
        return 1;
    }
    if (!identity) {
        std::cout << "m6behavior FAIL rewrite\n";
        return 1;
    }
    if (!deferred_match) {
        std::cout << "m6behavior FAIL deferred\n";
        return 1;
    }
    std::cout << "m6behavior PASS\n";
    failure << "The behavior self-check passed on engine output. The six gadgets ran and none of them changed "
               "the laser damage. m1 stays red without reference pictures. The M4 milestone stays red until "
               "eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 milestone stays red until the "
               "duel corpus is within 15% time to kill of a Recoil trace. The M6 milestone stays red until "
               "ai_10min holds a 1v1 for ten minutes.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

int gate_m6ai(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double min_orders = 0;
    double frame_at = 0;
    double com_x = 0;
    double com_z = 0;
    double slop = 0;
    double metal = 0;
    double energy = 0;
    double occupy = 0;
    double team_slop = 0;
    double unit_id = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "m6a_min_orders", min_orders) ||
        !toml_number(tolerance_text, "m6a_frame", frame_at) || !toml_number(tolerance_text, "m6a_com_x", com_x) ||
        !toml_number(tolerance_text, "m6a_com_z", com_z) || !toml_number(tolerance_text, "m6a_slop", slop) ||
        !toml_number(tolerance_text, "m6a_metal", metal) || !toml_number(tolerance_text, "m6a_energy", energy) ||
        !toml_number(tolerance_text, "m6a_occupy", occupy) ||
        !toml_number(tolerance_text, "m6a_team_slop", team_slop) || !toml_number(tolerance_text, "m6a_unit", unit_id)) {
        std::cerr << "gate: m6ai tolerances are missing\n";
        return 2;
    }

    std::ostringstream failure;
    failure << "# simple ai\n\n";
    failure << "Scenario: simple_ai\n\n";
    failure << "Entity: Armada commander on a SimpleAI team\n\n";
    failure << "Field: one recorded extractor order on the closest open metal spot\n\n";

    const std::string report_path = options.actual + "/m6ai/ai.json";
    const std::string trace_path = options.actual + "/m6ai/simple_ai.jsonl";
    std::string report;
    const bool report_ok = read_text(report_path, report);
    std::string name;
    double enabled = 0;
    std::vector<std::string> orders;
    std::vector<std::string> spots;
    const bool name_ok = report_ok && json_string_field(report, "name", name);
    const bool enabled_ok = report_ok && json_number_field(report, "enabled", enabled);
    const bool orders_ok = report_ok && json_objects(report, "orders", orders);
    const bool spots_ok = report_ok && json_objects(report, "spots", spots);
    const std::string* mex = nullptr;
    std::string mex_def;
    double mex_unit = -1;
    double mex_x = 0;
    double mex_z = 0;
    if (orders_ok) {
        for (const std::string& order : orders) {
            std::string def;
            if (!json_string_field(order, "def", def)) {
                continue;
            }
            if (def == "armmex" || def == "armuwms") {
                mex = &order;
                mex_def = def;
                json_number_field(order, "unit", mex_unit);
                json_number_field(order, "x", mex_x);
                json_number_field(order, "z", mex_z);
                break;
            }
        }
    }
    double best = 1.0e300;
    double spot_x = 0;
    double spot_z = 0;
    bool spot_found = false;
    if (spots_ok) {
        const double reach = occupy * occupy;
        for (const std::string& spot : spots) {
            double x = 0;
            double z = 0;
            if (!json_number_field(spot, "x", x) || !json_number_field(spot, "z", z)) {
                continue;
            }
            const double dx = x - com_x;
            const double dz = z - com_z;
            const double dist = dx * dx + dz * dz;
            if (dist <= reach) {
                continue;
            }
            if (dist < best) {
                best = dist;
                spot_x = x;
                spot_z = z;
                spot_found = true;
            }
        }
    }
    const bool order_ok = mex != nullptr && static_cast<int>(orders.size()) + 0.0001 >= min_orders &&
                          static_cast<int>(mex_unit) == static_cast<int>(unit_id);
    const bool spot_ok = spot_found && std::fabs(mex_x - spot_x) <= slop && std::fabs(mex_z - spot_z) <= slop;

    std::ifstream trace_in(trace_path);
    std::string line;
    std::string sample;
    while (trace_in && std::getline(trace_in, line)) {
        double frame = -1;
        if (!json_number_field(line, "f", frame) || static_cast<int>(frame) != static_cast<int>(frame_at)) {
            continue;
        }
        sample = line;
        break;
    }
    std::vector<std::string> units;
    std::vector<std::string> teams;
    const bool sample_ok = !sample.empty() && json_objects(sample, "units", units) &&
                           json_objects(sample, "teams", teams) && units.size() == 1 && teams.size() == 1;
    std::string unit_def;
    double unit_x = 0;
    double unit_z = 0;
    double team_metal = 0;
    double team_energy = 0;
    if (sample_ok) {
        json_string_field(units[0], "def", unit_def);
        json_number_field(units[0], "x", unit_x);
        json_number_field(units[0], "z", unit_z);
        json_number_field(teams[0], "m", team_metal);
        json_number_field(teams[0], "e", team_energy);
    }
    const bool placed = sample_ok && unit_def == "armcom" && std::fabs(unit_x - com_x) <= slop &&
                        std::fabs(unit_z - com_z) <= slop;
    const bool stocks = sample_ok && std::fabs(team_metal - metal) <= team_slop &&
                        std::fabs(team_energy - energy) <= team_slop;

    if (report_ok) {
        failure << "Report name " << name << " enabled " << enabled << " orders " << orders.size() << " spots "
                << spots.size() << "\n";
    } else {
        failure << "No ai report at " << report_path << ".\n\n";
    }
    if (mex != nullptr) {
        failure << "Order " << mex_def << " unit " << mex_unit << " at " << mex_x << " " << mex_z << " spot " << spot_x
                << " " << spot_z << "\n";
    } else {
        failure << "No armmex or armuwms order.\n";
    }
    if (sample_ok) {
        failure << "Frame " << static_cast<int>(frame_at) << " " << unit_def << " at " << unit_x << " " << unit_z
                << " metal " << team_metal << " energy " << team_energy << "\n\n";
    } else {
        failure << "No one-unit sample at frame " << static_cast<int>(frame_at) << ".\n\n";
    }
    failure << "The extractor order is the closest metal spot more than " << occupy
            << " elmos from the start. The commander walks toward it. Frame " << static_cast<int>(frame_at)
            << " is still short of the site, so both stocks stay " << metal
            << ". A cloak toggle may be recorded before the extractor.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!report_ok || !name_ok || !enabled_ok || !orders_ok || !spots_ok || !sample_ok) {
        std::cout << "m6ai FAIL trace\n";
        return 1;
    }
    std::cout << "simpleai " << name << " enabled " << static_cast<int>(enabled) << " orders " << orders.size()
              << " spots " << spots.size() << "\n";
    if (mex != nullptr) {
        std::cout << "order " << mex_def << " unit " << static_cast<int>(mex_unit) << " at " << mex_x << " " << mex_z
                  << " spot " << spot_x << " " << spot_z << "\n";
    }
    std::cout << "frame " << static_cast<int>(frame_at) << " " << unit_def << " at " << unit_x << " " << unit_z
              << " metal " << team_metal << " energy " << team_energy << "\n";
    if (name != "SimpleAI") {
        std::cout << "m6ai FAIL name\n";
        return 1;
    }
    if (static_cast<int>(enabled) != 1) {
        std::cout << "m6ai FAIL enabled\n";
        return 1;
    }
    if (!order_ok) {
        std::cout << "m6ai FAIL order\n";
        return 1;
    }
    if (!spot_ok) {
        std::cout << "m6ai FAIL spot\n";
        return 1;
    }
    if (!placed) {
        std::cout << "m6ai FAIL place\n";
        return 1;
    }
    if (!stocks) {
        std::cout << "m6ai FAIL stock\n";
        return 1;
    }
    std::cout << "m6ai PASS\n";
    failure << "The simpleai self-check passed on engine output. The commander ordered one extractor and started "
               "walking toward it. m1 stays red without reference pictures. The M4 milestone stays red until "
               "eco_t1, build_tank, and reclaim match a Recoil trace at 2%. The M5 milestone stays red until the "
               "duel corpus is within 15% time to kill of a Recoil trace. The M6 milestone stays red until a "
               "Recoil trace shows the 10-minute resource totals within 15%.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

int gate_ai10(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double frame_at = 0;
    double mex_floor = 0;
    double unit_floor = 0;
    double sides = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "a10_frame", frame_at) ||
        !toml_number(tolerance_text, "a10_mex", mex_floor) || !toml_number(tolerance_text, "a10_units", unit_floor) ||
        !toml_number(tolerance_text, "a10_sides", sides)) {
        std::cerr << "gate: ai10 tolerances are missing\n";
        return 2;
    }

    std::ostringstream failure;
    failure << "# ai 10min\n\n";
    failure << "Scenario: ai_10min\n\n";
    failure << "Entity: two Armada SimpleAI commanders on Supreme Isthmus\n\n";
    failure << "Field: both still playing at 10 minutes, each with extractor income\n\n";

    const std::string trace_path = options.actual + "/ai10/ai_10min.jsonl";
    std::ifstream trace_in(trace_path);
    std::string line;
    std::string sample;
    while (trace_in && std::getline(trace_in, line)) {
        double frame = -1;
        if (!json_number_field(line, "f", frame) || static_cast<int>(frame) != static_cast<int>(frame_at)) {
            continue;
        }
        sample = line;
        break;
    }
    std::vector<std::string> units;
    std::vector<std::string> teams;
    const bool sample_ok = !sample.empty() && json_objects(sample, "units", units) && json_objects(sample, "teams", teams);
    int armcoms = 0;
    bool team0 = false;
    bool team1 = false;
    if (sample_ok) {
        for (const std::string& unit : units) {
            std::string def;
            double team = -1;
            if (!json_string_field(unit, "def", def) || def != "armcom" || !json_number_field(unit, "t", team)) {
                continue;
            }
            ++armcoms;
            if (static_cast<int>(team) == 0) {
                team0 = true;
            }
            if (static_cast<int>(team) == 1) {
                team1 = true;
            }
        }
    }
    double mex0 = 0;
    double mex1 = 0;
    bool have0 = false;
    bool have1 = false;
    if (sample_ok) {
        for (const std::string& team : teams) {
            double id = -1;
            double mex = 0;
            if (!json_number_field(team, "id", id) || !json_number_field(team, "mx", mex)) {
                continue;
            }
            if (static_cast<int>(id) == 0) {
                mex0 = mex;
                have0 = true;
            }
            if (static_cast<int>(id) == 1) {
                mex1 = mex;
                have1 = true;
            }
        }
    }
    const bool alive = sample_ok && team0 && team1 && armcoms >= static_cast<int>(sides);
    const bool built = have0 && have1 && mex0 > mex_floor && mex1 > mex_floor;
    const bool crowded = sample_ok && static_cast<int>(units.size()) >= static_cast<int>(unit_floor);
    const bool winner = alive;

    if (sample_ok) {
        failure << "Frame " << static_cast<int>(frame_at) << " units " << units.size() << " armcoms " << armcoms
                << " mex " << mex0 << " " << mex1 << "\n\n";
    } else {
        failure << "No sample at frame " << static_cast<int>(frame_at) << ".\n\n";
    }
    failure << "Both commanders stay in the trace and both teams have finished an extractor. Winner is none while "
               "both commanders remain. This gate does not compare stocks to a Recoil trace.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!sample_ok) {
        std::cout << "ai10 FAIL trace\n";
        return 1;
    }
    std::cout << "frame " << static_cast<int>(frame_at) << " units " << units.size() << " armcoms " << armcoms
              << " mex " << mex0 << " " << mex1 << "\n";
    if (!alive) {
        std::cout << "ai10 FAIL alive\n";
        return 1;
    }
    if (!built) {
        std::cout << "ai10 FAIL mex\n";
        return 1;
    }
    if (!crowded) {
        std::cout << "ai10 FAIL units\n";
        return 1;
    }
    if (!winner) {
        std::cout << "ai10 FAIL winner\n";
        return 1;
    }
    std::cout << "winner none\n";
    std::cout << "ai10 PASS\n";
    failure << "The ai_10min self-check passed on engine output. Both SimpleAI commanders are in the frame "
            << static_cast<int>(frame_at)
            << " trace and both teams have extractor income. Winner is none. This is not a Recoil comparison. "
               "The M6 milestone stays red until a Recoil trace shows the two resource totals within 15%. m1 stays "
               "red without reference pictures. The M4 milestone stays red until eco_t1, build_tank, and reclaim "
               "match a Recoil trace at 2%. The M5 milestone stays red until the duel corpus is within 15% time to "
               "kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}

int gate_pve(const Options& options) {
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double frame_at = 0;
    double unit_count = 0;
    double com_x = 0;
    double com_y = 0;
    double com_z = 0;
    double slop = 0;
    double map_side = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "pve_frame", frame_at) ||
        !toml_number(tolerance_text, "pve_units", unit_count) || !toml_number(tolerance_text, "pve_com_x", com_x) ||
        !toml_number(tolerance_text, "pve_com_y", com_y) || !toml_number(tolerance_text, "pve_com_z", com_z) ||
        !toml_number(tolerance_text, "pve_slop", slop) || !toml_number(tolerance_text, "pve_map", map_side)) {
        std::cerr << "gate: pve tolerances are missing\n";
        return 2;
    }

    auto sample_at = [&](const std::string& path, std::string& sample) {
        std::ifstream trace_in(path);
        std::string line;
        while (trace_in && std::getline(trace_in, line)) {
            double frame = -1;
            if (!json_number_field(line, "f", frame) || static_cast<int>(frame) != static_cast<int>(frame_at)) {
                continue;
            }
            sample = line;
            return true;
        }
        return false;
    };
    auto near_unit = [&](const std::string& unit, double x, double y, double z) {
        double ux = 0;
        double uy = 0;
        double uz = 0;
        return json_number_field(unit, "x", ux) && json_number_field(unit, "y", uy) && json_number_field(unit, "z", uz) &&
               std::fabs(ux - x) <= slop && std::fabs(uy - y) <= slop && std::fabs(uz - z) <= slop;
    };
    // The burrow search is seeded from the clock, so the structure's coordinates are not a locked sample.
    auto on_map = [&](const std::string& unit) {
        double ux = 0;
        double uz = 0;
        return json_number_field(unit, "x", ux) && json_number_field(unit, "z", uz) && ux >= 0 && uz >= 0 &&
               ux <= map_side && uz <= map_side;
    };
    auto check_side = [&](const std::string& path, const char* spawn_def, bool& ok) {
        ok = false;
        std::string sample;
        std::vector<std::string> units;
        if (!sample_at(path, sample) || !json_objects(sample, "units", units) ||
            static_cast<int>(units.size()) != static_cast<int>(unit_count)) {
            return;
        }
        bool commander = false;
        bool spawn = false;
        bool enemy_commander = false;
        for (const std::string& unit : units) {
            std::string def;
            double team = -1;
            if (!json_string_field(unit, "def", def) || !json_number_field(unit, "t", team)) {
                return;
            }
            if (def == "armcom" && static_cast<int>(team) == 0 && near_unit(unit, com_x, com_y, com_z)) {
                commander = true;
            }
            if (def == "armcom" && static_cast<int>(team) == 1) {
                enemy_commander = true;
            }
            if (def == spawn_def && static_cast<int>(team) == 1 && on_map(unit)) {
                spawn = true;
            }
        }
        ok = commander && spawn && !enemy_commander;
    };

    std::ostringstream failure;
    failure << "# pve spawners\n\n";
    failure << "Scenario: scav and raptor\n\n";
    failure << "Entity: one player commander and one spawned structure on Supreme Isthmus\n\n";
    failure << "Field: the mode commander is gone and the structure is on the mode team\n\n";

    bool scav_ok = false;
    bool rapt_ok = false;
    check_side(options.actual + "/pve/scav.jsonl", "scavbeacon_t1_scav", scav_ok);
    check_side(options.actual + "/pve/raptor.jsonl", "raptor_hive", rapt_ok);
    failure << "Frame " << static_cast<int>(frame_at) << " scav " << (scav_ok ? "ok" : "miss") << " raptor "
            << (rapt_ok ? "ok" : "miss") << "\n\n";
    failure << "Scavengers spawn one beacon. Raptors spawn one hive. The player commander stays at the start. "
               "The mode commander is removed on the first frame. Neither structure is drawn. This gate does not "
               "compare the spawn to a Recoil trace.\n\n";
    std::ofstream packet(options.oracle + "/last_failure.md");
    packet << failure.str();
    if (!scav_ok) {
        std::cout << "pve FAIL scav\n";
        return 1;
    }
    if (!rapt_ok) {
        std::cout << "pve FAIL raptor\n";
        return 1;
    }
    std::cout << "frame " << static_cast<int>(frame_at) << " scav beacon raptor hive\n";
    std::cout << "pve PASS\n";
    failure << "The pve self-check passed on engine output. Frame " << static_cast<int>(frame_at)
            << " has the player commander and one scavbeacon_t1_scav, and the same frame of the raptor trace has "
               "the player commander and one raptor_hive. The mode commanders are gone. This is not a Recoil "
               "comparison. The M6 milestone stays red until a Recoil trace shows the 10-minute resource totals "
               "within 15%. m1 stays red without reference pictures. The M4 milestone stays red until eco_t1, "
               "build_tank, and reclaim match a Recoil trace at 2%. The M5 milestone stays red until the duel "
               "corpus is within 15% time to kill of a Recoil trace.\n";
    packet.close();
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    return 0;
}


int count_magenta(const std::vector<uint8_t>& rgb) {
    int count = 0;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        if (rgb[i] > 200 && rgb[i + 1] < 40 && rgb[i + 2] > 200) {
            ++count;
        }
    }
    return count;
}

void ppm_pixel(const std::vector<uint8_t>& rgb, int w, int h, int x, int y, int& r, int& g, int& b) {
    x = std::max(0, std::min(w - 1, x));
    // Engine shots store row 0 at the top; OpenGL y grows up.
    const int sy = h - 1 - std::max(0, std::min(h - 1, y));
    const size_t o = static_cast<size_t>((sy * w + x) * 3);
    r = rgb[o];
    g = rgb[o + 1];
    b = rgb[o + 2];
}

bool log_has(const std::string& log, const char* needle) { return log.find(needle) != std::string::npos; }

int gate_m7(const Options& options) {
    // Self-only: no Recoil HUD/oracle traces exist for M7. Checks bar-metal outs from
    // check_m7select/build/minimap/hud/window under native/oracle/out/m7*.
    const std::string tolerance_path = options.oracle + "/tolerances.toml";
    std::string tolerance_text;
    double build_x = 0, build_z = 0, build_slop = 0;
    double mmap_x = 0, mmap_z = 0, mmap_slop = 0;
    double health_frac = 0, health_slop = 0, range = 0, range_slop = 0;
    double topbar_text_min = 0, wind_lit_min = 0, counter_lit_min = 0, income_colored_min = 0;
    double ring_min = 0, ring_max = 0, ring_width = 0, ring_med = 0, tip_hits_min = 0;
    double retina_w = 0, retina_h = 0, topbar_frac = 0, stable_diff = 0;
    if (!read_text(tolerance_path, tolerance_text) || !toml_number(tolerance_text, "m7_build_x", build_x) ||
        !toml_number(tolerance_text, "m7_build_z", build_z) || !toml_number(tolerance_text, "m7_build_slop", build_slop) ||
        !toml_number(tolerance_text, "m7_mmap_x", mmap_x) || !toml_number(tolerance_text, "m7_mmap_z", mmap_z) ||
        !toml_number(tolerance_text, "m7_mmap_slop", mmap_slop) ||
        !toml_number(tolerance_text, "m7_health_frac", health_frac) ||
        !toml_number(tolerance_text, "m7_health_slop", health_slop) || !toml_number(tolerance_text, "m7_range", range) ||
        !toml_number(tolerance_text, "m7_range_slop", range_slop) ||
        !toml_number(tolerance_text, "m7_topbar_text", topbar_text_min) ||
        !toml_number(tolerance_text, "m7_wind_lit", wind_lit_min) ||
        !toml_number(tolerance_text, "m7_counter_lit", counter_lit_min) ||
        !toml_number(tolerance_text, "m7_income_colored", income_colored_min) ||
        !toml_number(tolerance_text, "m7_ring_min", ring_min) || !toml_number(tolerance_text, "m7_ring_max", ring_max) ||
        !toml_number(tolerance_text, "m7_ring_width", ring_width) ||
        !toml_number(tolerance_text, "m7_ring_med", ring_med) ||
        !toml_number(tolerance_text, "m7_tip_hits", tip_hits_min) ||
        !toml_number(tolerance_text, "m7_retina_w", retina_w) || !toml_number(tolerance_text, "m7_retina_h", retina_h) ||
        !toml_number(tolerance_text, "m7_topbar_frac", topbar_frac) ||
        !toml_number(tolerance_text, "m7_stable_diff", stable_diff)) {
        std::cerr << "gate: m7 tolerances are missing\n";
        return 2;
    }

    std::ostringstream failure;
    failure << "# m7 hud\n\n";
    failure << "Scenario: m7select / m7build / m7minimap / m7hud / m7window\n\n";
    failure << "Entity: Armada commander HUD on Supreme Isthmus\n\n";
    failure << "Field: selection, build menu, minimap move, health/range/tooltip, Retina top bar\n\n";
    failure << "Self-only: no Recoil HUD/oracle trace is pinned for M7. This suite scores engine "
               "JSON and framebuffer shots from the check_m7* runs.\n\n";

    auto fail_write = [&](int code) {
        std::ofstream packet(options.oracle + "/last_failure.md");
        packet << failure.str();
        return code;
    };

    // --- selection ---
    std::string select_txt, select_log;
    const bool select_ok = read_text(options.actual + "/m7select/select.json", select_txt) &&
                           read_text(options.actual + "/m7select/run.log", select_log);
    std::string cursor;
    double cmd = -1;
    const bool cursor_ok = select_ok && json_string_field(select_txt, "cursor", cursor) && cursor == "cursormove";
    const bool cmd_ok = select_ok && json_number_field(select_txt, "cmd", cmd) && static_cast<int>(cmd) == 10;
    const bool sel_ok = select_ok && (select_txt.find("\"sel\":[1]") != std::string::npos ||
                                  select_txt.find("\"sel\": [1]") != std::string::npos);
    const bool select_log_ok =
        select_ok && log_has(select_log, "select box") && log_has(select_log, "ids 1") &&
        log_has(select_log, "order move 8000 4900 unit 1");
    double sim_x = 0;
    {
        const auto at = select_log.find("sim frame 60 at ");
        if (at != std::string::npos) {
            sim_x = std::strtod(select_log.c_str() + at + 16, nullptr);
        }
    }
    const bool moved_ok = sim_x > 7605.0;
    std::cout << "select cursor " << cursor << " cmd " << static_cast<int>(cmd) << " sel "
              << (sel_ok ? "[1]" : "?") << " sim_x " << sim_x << "\n";
    if (!select_ok || !cursor_ok || !cmd_ok || !sel_ok || !select_log_ok || !moved_ok) {
        failure << "Selection failed. Need cursormove, cmd 10, sel [1], move to 8000 4900, sim_x > 7605.\n\n";
        std::cout << "m7 FAIL select\n";
        return fail_write(1);
    }

    // --- build ---
    std::string build_txt, build_log;
    const bool build_ok = read_text(options.actual + "/m7build/build.json", build_txt) &&
                          read_text(options.actual + "/m7build/run.log", build_log);
    std::string build_def;
    double bcmd = 0, bx = 0, bz = 0, placing = 0, building = 0, building_now = 0, start_unit = 0;
    const bool def_ok = build_ok && json_string_field(build_txt, "def", build_def) && build_def == "armmex";
    const bool bcmd_ok = build_ok && json_number_field(build_txt, "cmd", bcmd) && static_cast<int>(bcmd) == -13;
    const bool place_ok = build_ok && json_number_field(build_txt, "placing", placing) && static_cast<int>(placing) == 1;
    const bool bxz_ok = build_ok && json_number_field(build_txt, "x", bx) && json_number_field(build_txt, "z", bz) &&
                        std::fabs(bx - build_x) <= build_slop && std::fabs(bz - build_z) <= build_slop;
    const bool nano_ok = build_ok && json_number_field(build_txt, "building", building) &&
                         json_number_field(build_txt, "buildingNow", building_now) && building > 0 &&
                         static_cast<int>(building) == static_cast<int>(building_now);
    const bool start_ok = build_ok && json_number_field(build_txt, "startUnit", start_unit) && start_unit > 0;
    const bool build_log_ok = build_ok && log_has(build_log, "player build armmex unit 1 at 7416 4888") &&
                              log_has(build_log, "build pick armmex");
    std::cout << "build " << build_def << " at " << bx << " " << bz << " nano " << static_cast<int>(building) << "\n";
    if (!build_ok || !def_ok || !bcmd_ok || !place_ok || !bxz_ok || !nano_ok || !start_ok || !build_log_ok) {
        failure << "Build failed. Need armmex cmd -13 at " << build_x << "," << build_z << " with nano id.\n\n";
        std::cout << "m7 FAIL build\n";
        return fail_write(1);
    }

    // --- minimap ---
    std::string mmap_txt, mmap_log;
    const bool mmap_ok = read_text(options.actual + "/m7minimap/minimap.json", mmap_txt) &&
                         read_text(options.actual + "/m7minimap/run.log", mmap_log);
    double sx = 0, sy = 0, minimized = 1, slave = 0, moved = 0, wx = 0, wz = 0;
    const bool geo_ok = mmap_ok && json_number_field(mmap_txt, "sx", sx) && json_number_field(mmap_txt, "sy", sy) &&
                        sx > 100 && sy > 100 && json_number_field(mmap_txt, "minimized", minimized) &&
                        static_cast<int>(minimized) == 0 && json_number_field(mmap_txt, "slave", slave) &&
                        static_cast<int>(slave) == 1;
    const bool move_ok = mmap_ok && json_number_field(mmap_txt, "moved", moved) && static_cast<int>(moved) == 1 &&
                         json_number_field(mmap_txt, "worldX", wx) && json_number_field(mmap_txt, "worldZ", wz) &&
                         std::fabs(wx - mmap_x) <= mmap_slop && std::fabs(wz - mmap_z) <= mmap_slop;
    const bool mmap_log_ok = mmap_ok && log_has(mmap_log, "minimap click") && log_has(mmap_log, "order move");
    std::cout << "minimap " << static_cast<int>(sx) << "x" << static_cast<int>(sy) << " world " << wx << " " << wz
              << "\n";
    if (!mmap_ok || !geo_ok || !move_ok || !mmap_log_ok) {
        failure << "Minimap failed. Need slave geometry and click near " << mmap_x << "," << mmap_z << ".\n\n";
        std::cout << "m7 FAIL minimap\n";
        return fail_write(1);
    }

    // --- hud status ---
    std::string hud_txt, hud_log, hud_text_log;
    const bool hud_ok = read_text(options.actual + "/m7hud/hud.json", hud_txt) &&
                        read_text(options.actual + "/m7hud/run.log", hud_log) &&
                        read_text(options.actual + "/m7hud/hud-text.log", hud_text_log);
    double hover = 0, health = 0, max_health = 0, hud_range = 0, bar_frac = 0;
    std::string tip;
    const bool tip_ok = hud_ok && json_number_field(hud_txt, "hover", hover) && static_cast<int>(hover) == 1 &&
                        json_string_field(hud_txt, "tooltip", tip) && tip == "armcom";
    const bool hp_ok = hud_ok && json_number_field(hud_txt, "health", health) &&
                       json_number_field(hud_txt, "maxHealth", max_health) && max_health > 100 &&
                       std::fabs(health - health_frac * max_health) <= health_slop;
    const bool range_ok =
        hud_ok && json_number_field(hud_txt, "range", hud_range) && std::fabs(hud_range - range) <= range_slop;
    const bool frac_ok =
        hud_ok && json_number_field(hud_txt, "barFrac", bar_frac) && std::fabs(bar_frac - health_frac) < 0.05;
    const bool hud_log_ok = hud_ok && log_has(hud_log, "hud hover unit 1 tooltip armcom") &&
                            log_has(hud_log, "hud damage unit 1 hp ") &&
                            hud_log.find("tooltip DrawScreen:") == std::string::npos &&
                            log_has(hud_text_log, "check-hud-text ok");
    std::cout << "hud tip " << tip << " hp " << health << "/" << max_health << " range " << hud_range << "\n";
    if (!hud_ok || !tip_ok || !hp_ok || !range_ok || !frac_ok || !hud_log_ok) {
        failure << "HUD status failed. Need armcom tip, half health, range " << range << ".\n\n";
        std::cout << "m7 FAIL hud\n";
        return fail_write(1);
    }

    // --- hud-end.ppm pixels (top bar, ring, tip, magenta) ---
    int hw = 0, hh = 0;
    std::vector<uint8_t> hrgb;
    std::string himg_err;
    const std::string hend = options.actual + "/m7hud/hud-end.ppm";
    if (!read_ppm(hend, hw, hh, hrgb, himg_err) || hw != 1280 || hh != 720) {
        failure << "hud-end.ppm missing or wrong size: " << himg_err << "\n\n";
        std::cout << "m7 FAIL hud-shot\n";
        return fail_write(1);
    }
    if (count_magenta(hrgb) != 0) {
        failure << "hud-end.ppm has magenta missing-texture pixels.\n\n";
        std::cout << "m7 FAIL magenta\n";
        return fail_write(1);
    }
    int topbar_text = 0, wind_lit = 0, counter_lit = 0, income_colored = 0, income_edge = 0;
    const int y0 = hh - std::max(8, static_cast<int>(hh * 0.09));
    for (int y = y0; y < hh; ++y) {
        for (int x = static_cast<int>(hw * 0.22); x < static_cast<int>(hw * 0.78); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(hrgb, hw, hh, x, y, r, g, b);
            const int bright = std::max(r, std::max(g, b));
            if (bright < 130) {
                continue;
            }
            if (std::abs(r - g) < 10 && std::abs(g - b) < 10 && bright < 160) {
                continue;
            }
            ++topbar_text;
        }
        for (int x = static_cast<int>(hw * 0.55); x < static_cast<int>(hw * 0.68); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(hrgb, hw, hh, x, y, r, g, b);
            const int bright = std::max(r, std::max(g, b));
            if (bright >= 160 && !(std::abs(r - g) < 10 && std::abs(g - b) < 10 && bright < 180)) {
                ++wind_lit;
            }
        }
        for (int x = static_cast<int>(hw * 0.66); x < static_cast<int>(hw * 0.80); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(hrgb, hw, hh, x, y, r, g, b);
            const int bright = std::max(r, std::max(g, b));
            if (bright >= 160 && !(std::abs(r - g) < 10 && std::abs(g - b) < 10 && bright < 180)) {
                ++counter_lit;
            }
        }
        for (int x = static_cast<int>(hw * 0.28); x < static_cast<int>(hw * 0.52); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(hrgb, hw, hh, x, y, r, g, b);
            const int bright = std::max(r, std::max(g, b));
            if (bright < 100) {
                continue;
            }
            const bool colored = (g > r + 25 && g > b + 20) || (r > g + 25 && r > b + 20);
            if (!colored) {
                continue;
            }
            ++income_colored;
            const int nbs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& nb : nbs) {
                int rr = 0, gg = 0, bb = 0;
                ppm_pixel(hrgb, hw, hh, x + nb[0], y + nb[1], rr, gg, bb);
                if (bright - std::max(rr, std::max(gg, bb)) > 45) {
                    ++income_edge;
                    break;
                }
            }
        }
    }
    std::cout << "topbar_text " << topbar_text << " wind " << wind_lit << " counter " << counter_lit << " income "
              << income_colored << "/" << income_edge << "\n";
    if (topbar_text < static_cast<int>(topbar_text_min) || wind_lit < static_cast<int>(wind_lit_min) ||
        counter_lit < static_cast<int>(counter_lit_min) || income_colored < static_cast<int>(income_colored_min) ||
        income_edge * 2 < income_colored) {
        failure << "Top bar text failed. topbar=" << topbar_text << " wind=" << wind_lit << " counter=" << counter_lit
                << " income=" << income_colored << " edge=" << income_edge << "\n\n";
        std::cout << "m7 FAIL topbar\n";
        return fail_write(1);
    }

    // Range ring: orange spread
    std::vector<std::pair<int, int>> orange;
    for (int y = 0; y < hh; ++y) {
        for (int x = 0; x < hw; ++x) {
            const size_t o = static_cast<size_t>((y * hw + x) * 3);
            const int r = hrgb[o], g = hrgb[o + 1], b = hrgb[o + 2];
            if (r > 140 && g < 120 && b < 100 && r > g + 30 && r > b + 40) {
                orange.push_back({x, hh - 1 - y});
            }
        }
    }
    int minx = hw, maxx = 0;
    std::vector<double> dists;
    dists.reserve(orange.size());
    const double ux = 640.0, uy = 371.0;
    for (const auto& p : orange) {
        minx = std::min(minx, p.first);
        maxx = std::max(maxx, p.first);
        dists.push_back(std::hypot(p.first - ux, p.second - uy));
    }
    std::sort(dists.begin(), dists.end());
    const double med = dists.empty() ? 0.0 : dists[dists.size() / 2];
    std::cout << "ring orange " << orange.size() << " width " << (maxx - minx) << " med " << med << "\n";
    if (static_cast<int>(orange.size()) < static_cast<int>(ring_min) ||
        static_cast<int>(orange.size()) > static_cast<int>(ring_max) || (maxx - minx) <= static_cast<int>(ring_width) ||
        med <= ring_med) {
        failure << "Range ring failed. count=" << orange.size() << " width=" << (maxx - minx) << " med=" << med
                << "\n\n";
        std::cout << "m7 FAIL ring\n";
        return fail_write(1);
    }

    // Tooltip text near cursor
    double hx = 640, hy = 360;
    json_number_field(hud_txt, "hoverX", hx);
    json_number_field(hud_txt, "hoverY", hy);
    int tip_hits = 0;
    for (int y = static_cast<int>(hy - 40); y < static_cast<int>(hy + 100); ++y) {
        for (int x = static_cast<int>(hx - 40); x < static_cast<int>(hx + 280); ++x) {
            if (x < 0 || y < 0 || x >= hw || y >= hh) {
                continue;
            }
            int r = 0, g = 0, b = 0;
            ppm_pixel(hrgb, hw, hh, x, y, r, g, b);
            if ((r > 180 && g > 180 && b > 170) || (r > 160 && g > 200 && b > 160)) {
                ++tip_hits;
            }
        }
    }
    std::cout << "tooltip pixels " << tip_hits << "\n";
    if (tip_hits < static_cast<int>(tip_hits_min)) {
        failure << "Tooltip text pixels near cursor: " << tip_hits << "\n\n";
        std::cout << "m7 FAIL tooltip\n";
        return fail_write(1);
    }

    // --- Retina window ---
    std::string win_log;
    if (!read_text(options.actual + "/m7window/run.log", win_log)) {
        failure << "m7window/run.log missing.\n\n";
        std::cout << "m7 FAIL window-log\n";
        return fail_write(1);
    }
    int dw = 0, dh = 0;
    {
        const auto at = win_log.find("drawable: ");
        if (at != std::string::npos) {
            std::sscanf(win_log.c_str() + at + 10, "%dx%d", &dw, &dh);
        }
    }
    std::cout << "drawable " << dw << "x" << dh << "\n";
    if (dw < static_cast<int>(retina_w) || dh < static_cast<int>(retina_h) ||
        win_log.find("attempt to perform arithmetic on local") != std::string::npos ||
        win_log.find("widgets view ") == std::string::npos || win_log.find("select box") == std::string::npos) {
        failure << "Retina window failed. drawable " << dw << "x" << dh << "\n\n";
        std::cout << "m7 FAIL window\n";
        return fail_write(1);
    }
    int ww = 0, wh = 0;
    std::vector<uint8_t> wrgb;
    std::string werr;
    if (!read_ppm(options.actual + "/m7window/window-end.ppm", ww, wh, wrgb, werr) || ww != dw || wh != dh) {
        failure << "window-end.ppm missing or size mismatch: " << werr << "\n\n";
        std::cout << "m7 FAIL window-shot\n";
        return fail_write(1);
    }
    if (count_magenta(wrgb) != 0) {
        failure << "window-end.ppm has magenta pixels.\n\n";
        std::cout << "m7 FAIL window-magenta\n";
        return fail_write(1);
    }
    // Minimap marker green-ish in top-left panel
    int marker_hits = 0;
    double msx = 200, msy = 200;
    {
        std::string mj;
        if (read_text(options.actual + "/m7window/minimap.json", mj)) {
            json_number_field(mj, "sx", msx);
            json_number_field(mj, "sy", msy);
        }
    }
    for (int y = wh - static_cast<int>(msy); y < wh; ++y) {
        for (int x = 0; x < static_cast<int>(msx); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(wrgb, ww, wh, x, y, r, g, b);
            if (g > 140 && g > r + 20 && g > b + 20) {
                ++marker_hits;
            }
        }
    }
    int w_topbar = 0;
    const int wy0 = wh - std::max(8, static_cast<int>(wh * 0.09));
    for (int y = wy0; y < wh; ++y) {
        for (int x = static_cast<int>(ww * 0.22); x < static_cast<int>(ww * 0.78); ++x) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(wrgb, ww, wh, x, y, r, g, b);
            const int bright = std::max(r, std::max(g, b));
            if (bright < 130) {
                continue;
            }
            if (std::abs(r - g) < 10 && std::abs(g - b) < 10 && bright < 160) {
                continue;
            }
            ++w_topbar;
        }
    }
    int top_flat = 0;
    for (int y = wh - 1; y > std::max(0, wh - static_cast<int>(wh * 0.35)); --y) {
        int row_grey = 0;
        for (int x = static_cast<int>(ww * 0.30); x < static_cast<int>(ww * 0.85); x += 3) {
            int r = 0, g = 0, b = 0;
            ppm_pixel(wrgb, ww, wh, x, y, r, g, b);
            if (std::abs(r - g) < 18 && std::abs(g - b) < 18 && r >= 25 && r <= 100 && b <= r + 10) {
                ++row_grey;
            }
        }
        if (row_grey > (ww * 0.55) / 3 * 0.40) {
            ++top_flat;
        } else if (top_flat > 0) {
            break;
        }
    }
    std::cout << "window marker " << marker_hits << " topbar_text " << w_topbar << " topbar_rows " << top_flat << "\n";
    if (marker_hits < 8 || w_topbar < static_cast<int>(topbar_text_min) ||
        top_flat > static_cast<int>(wh * topbar_frac) || top_flat < 4) {
        failure << "Window HUD failed. marker=" << marker_hits << " topbar_text=" << w_topbar
                << " topbar_rows=" << top_flat << "\n\n";
        std::cout << "m7 FAIL window-hud\n";
        return fail_write(1);
    }

    // Magenta on select/build/minimap end shots
    for (const char* shot : {"/m7select/select-end.ppm", "/m7build/build-end.ppm", "/m7minimap/minimap-end.ppm"}) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
        std::string err;
        if (!read_ppm(options.actual + shot, w, h, rgb, err) || w != 1280 || h != 720 || count_magenta(rgb) != 0) {
            failure << "Shot " << shot << " failed: " << err << "\n\n";
            std::cout << "m7 FAIL shot " << shot << "\n";
            return fail_write(1);
        }
    }

    failure << "Selection cursormove cmd 10 sel [1], build armmex at " << build_x << "," << build_z
            << ", minimap move near " << mmap_x << "," << mmap_z << ", HUD armcom half-health range " << range
            << ", Retina " << dw << "x" << dh << ". Self-only (no Recoil HUD trace).\n\n";
    failure << "The m7 self-check passed on engine output from check_m7*. No Recoil HUD oracle is pinned. m1 stays "
               "red without reference pictures. The M4/M5/M6 milestones stay red until their Recoil traces exist.\n";
    std::ofstream passed(options.oracle + "/last_failure.md");
    passed << failure.str();
    std::cout << "m7 PASS\n";
    return 0;
}


}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    if (options.suite == "m0") {
        return gate_m0(options);
    }
    if (options.suite == "m1") {
        return gate_m1(options);
    }
    if (options.suite == "m2") {
        return gate_m2(options);
    }
    if (options.suite == "m3") {
        return gate_m3(options);
    }
    if (options.suite == "m3walk") {
        return gate_m3walk(options);
    }
    if (options.suite == "m3ship") {
        return gate_m3ship(options);
    }
    if (options.suite == "m3hover") {
        return gate_m3hover(options);
    }
    if (options.suite == "m3gunship") {
        return gate_m3gunship(options);
    }
    if (options.suite == "m3plane") {
        return gate_m3plane(options);
    }
    if (options.suite == "m3cob") {
        return gate_m3cob(options);
    }
    if (options.suite == "m3htank") {
        return gate_m3htank(options);
    }
    if (options.suite == "m4eco") {
        return gate_m4eco(options);
    }
    if (options.suite == "m4mex") {
        return gate_m4mex(options);
    }
    if (options.suite == "m4pwr") {
        return gate_m4pwr(options);
    }
    if (options.suite == "m4nano") {
        return gate_m4nano(options);
    }
    if (options.suite == "m4lab") {
        return gate_m4lab(options);
    }
    if (options.suite == "m4reclaim") {
        return gate_m4reclaim(options);
    }
    if (options.suite == "m5laser") {
        return gate_m5laser(options);
    }
    if (options.suite == "m5cannon") {
        return gate_m5cannon(options);
    }
    if (options.suite == "m5beam") {
        return gate_m5beam(options);
    }
    if (options.suite == "m5missile") {
        return gate_m5missile(options);
    }
    if (options.suite == "m5lightning") {
        return gate_m5lightning(options);
    }
    if (options.suite == "m5flame") {
        return gate_m5flame(options);
    }
    if (options.suite == "m5torpedo") {
        return gate_m5torpedo(options);
    }
    if (options.suite == "m5bomb") {
        return gate_m5bomb(options);
    }
    if (options.suite == "m5dgun") {
        return gate_m5dgun(options);
    }
    if (options.suite == "los") {
        return gate_los(options);
    }
    if (options.suite == "m5wreck") {
        return gate_m5wreck(options);
    }
    if (options.suite == "m6spawn") {
        return gate_m6spawn(options);
    }
    if (options.suite == "m6behavior") {
        return gate_m6behavior(options);
    }
    if (options.suite == "m6ai") {
        return gate_m6ai(options);
    }
    if (options.suite == "pve") {
        return gate_pve(options);
    }
    if (options.suite == "ai_10min") {
        return gate_ai10(options);
    }
    if (options.suite == "m7") {
        return gate_m7(options);
    }
    std::cerr << "gate: unknown suite " << options.suite << "\n";
    return 2;
}
