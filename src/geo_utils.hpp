// geo_utils.hpp -- geometry helpers, no external dependencies.
#pragma once
#include <cmath>
#include <vector>
#include <utility>

constexpr double EARTH_RADIUS_M = 6371000.0;
constexpr double DEG2RAD = M_PI / 180.0;

inline double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    double phi1 = lat1 * DEG2RAD, phi2 = lat2 * DEG2RAD;
    double dphi = (lat2 - lat1) * DEG2RAD;
    double dlambda = (lon2 - lon1) * DEG2RAD;
    double a = std::sin(dphi / 2) * std::sin(dphi / 2) +
               std::cos(phi1) * std::cos(phi2) * std::sin(dlambda / 2) * std::sin(dlambda / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return EARTH_RADIUS_M * c;
}


// Ray-casting point-in-polygon test. polygon is a list of (lat, lon) pairs.
inline bool point_in_polygon(double lat, double lon,
                              const std::vector<std::pair<double,double>>& polygon) {
    bool inside = false;
    size_t n = polygon.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double yi = polygon[i].first, xi = polygon[i].second;
        double yj = polygon[j].first, xj = polygon[j].second;
        bool intersects = ((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi + 1e-15) + xi);
        if (intersects) inside = !inside;
    }
    return inside;
}
