// sensor_health.hpp -- per-sensor heartbeat status tracking.
#pragma once
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include "models.hpp"

constexpr double SENSOR_ONLINE_SECONDS = 10.0;
constexpr double SENSOR_DEGRADED_SECONDS = 60.0;

class SensorHealth {
public:
    void mark_seen(const std::string& sensor_id, const std::string& sensor_type, double timestamp) {
        std::lock_guard<std::mutex> lock(mu_);
        auto& e = sensors_[sensor_id];
        e.sensor_type = sensor_type;
        e.last_seen = std::max(e.last_seen, timestamp);
        e.detection_count++;
    }

    std::string status_for(const std::string& sensor_id, double now) const {
        auto it = sensors_.find(sensor_id);
        if (it == sensors_.end()) return "unknown";
        double age = now - it->second.last_seen;
        if (age <= SENSOR_ONLINE_SECONDS) return "online";
        if (age <= SENSOR_DEGRADED_SECONDS) return "degraded";
        return "offline";
    }

    json all_statuses() {
        std::lock_guard<std::mutex> lock(mu_);
        double now = now_seconds();
        json out = json::array();
        for (auto& [id, e] : sensors_) {
            json j;
            j["sensor_id"] = id;
            j["sensor_type"] = e.sensor_type;
            j["last_seen"] = e.last_seen;
            j["seconds_since_last_seen"] = now - e.last_seen;
            j["detection_count"] = e.detection_count;
            j["status"] = status_for(id, now);
            out.push_back(j);
        }
        return out;
    }

private:
    struct Entry {
        std::string sensor_type;
        double last_seen = 0;
        int detection_count = 0;
    };
    std::map<std::string, Entry> sensors_;
    mutable std::mutex mu_;
};
