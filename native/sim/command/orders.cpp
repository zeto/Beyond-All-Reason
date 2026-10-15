#include "command/orders.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace bar {

bool load_orders(const std::string& path, std::vector<ScriptOrder>& orders, std::string& error) {
    orders.clear();
    std::ifstream in(path);
    if (!in) {
        error = "could not read orders " + path;
        return false;
    }
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream text(line);
        ScriptOrder order;
        std::string third;
        if (!(text >> order.frame >> order.unit >> third)) {
            error = "bad order line " + std::to_string(number) + " in " + path;
            return false;
        }
        if (third == "build") {
            order.kind = OrderKind::Build;
            if (!(text >> order.build_def >> order.x >> order.z) || order.build_def.empty()) {
                error = "bad order line " + std::to_string(number) + " in " + path;
                return false;
            }
        } else if (third == "make") {
            order.kind = OrderKind::Make;
            if (!(text >> order.build_def) || order.build_def.empty()) {
                error = "bad order line " + std::to_string(number) + " in " + path;
                return false;
            }
        } else if (third == "reclaim") {
            order.kind = OrderKind::Reclaim;
            if (!(text >> order.build_def >> order.x >> order.z) || order.build_def.empty()) {
                error = "bad order line " + std::to_string(number) + " in " + path;
                return false;
            }
        } else if (third == "attack") {
            order.kind = OrderKind::Attack;
            if (!(text >> order.target) || order.target < 0) {
                error = "bad order line " + std::to_string(number) + " in " + path;
                return false;
            }
        } else {
            char* end = nullptr;
            order.x = std::strtof(third.c_str(), &end);
            if (end == third.c_str() || *end != '\0' || !(text >> order.z)) {
                error = "bad order line " + std::to_string(number) + " in " + path;
                return false;
            }
        }
        orders.push_back(order);
    }
    if (orders.empty()) {
        error = "orders file is empty: " + path;
        return false;
    }
    return true;
}

bool apply_orders(UnitSim& sim, const std::vector<ScriptOrder>& orders, int frame, const TerrainMesh& terrain,
                  std::string& error) {
    for (const ScriptOrder& order : orders) {
        if (order.frame != frame || order.kind != OrderKind::Move) {
            continue;
        }
        if (!sim_order_move(sim, order.unit, order.x, order.z, terrain, error)) {
            return false;
        }
    }
    return true;
}

}  // namespace bar