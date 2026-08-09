// geofence.hpp -- user-defined restricted zones, point-in-polygon violation checks.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <fstream>
#include "geo_utils.hpp"
#include "models.hpp"

struct Zone {
    std::string zone_id;
    std::string name;
    std::vector<std::pair<double,double>> polygon; // (lat, lon)

    bool contains(double lat, double lon) const {
        return point_in_polygon(lat, lon, polygon);
    }
};

inline std::vector<Zone> default_zones() {
    // Fallback used only when no zones.json is present -- see load_zones()
    // for the actual user-configurable path. One illustrative example
    // near Washington, DC so geofencing has something to demonstrate
    // against out of the box.
    return {
        Zone{
            "RZ-1", "Airfield Approach Corridor",
            {
                {38.8998, -77.0430}, {38.8998, -77.0330},
                {38.8930, -77.0330}, {38.8930, -77.0430},
            }
        }
    };
}

// zones.json, if present next to the binary, is an array of
// {"zone_id":"...", "name":"...", "polygon": [[lat,lon], ...]} -- this is
// what makes "restricted or custom zones" actually custom without a
// rebuild. Falls back to default_zones() if the file is missing, empty,
// or malformed, so geofencing degrades to one example zone rather than
// silently having none.
inline std::vector<Zone> load_zones(const std::string& path = "zones.json") {
    std::ifstream f(path);
    if (!f) return default_zones();
    try {
        json j; f >> j;
        if (!j.is_array() || j.empty()) return default_zones();
        std::vector<Zone> zones;
        for (auto& zj : j) {
            Zone z;
            z.zone_id = zj.value("zone_id", std::string());
            z.name = zj.value("name", std::string());
            if (!zj.contains("polygon") || !zj["polygon"].is_array()) continue;
            for (auto& pt : zj["polygon"]) {
                if (!pt.is_array() || pt.size() < 2) continue;
                z.polygon.push_back({pt[0].get<double>(), pt[1].get<double>()});
            }
            if (z.polygon.size() >= 3) zones.push_back(z);
        }
        return zones.empty() ? default_zones() : zones;
    } catch (...) {
        return default_zones();
    }
}

inline std::optional<Zone> zone_containing(double lat, double lon, const std::vector<Zone>& zones) {
    for (auto& z : zones) if (z.contains(lat, lon)) return z;
    return std::nullopt;
}
