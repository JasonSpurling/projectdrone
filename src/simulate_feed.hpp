// simulate_feed.hpp -- synthetic multi-sensor demo scenario (optional,
// for showing the drone-fusion side of the app work without real sensors).
#pragma once
#include <cmath>
#include <random>
#include <thread>
#include <chrono>
#include "track_manager.hpp"

namespace simulate {

constexpr double CENTER_LAT = 38.9072, CENTER_LON = -77.0369;

inline std::pair<double,double> offset_ll(double lat, double lon, double north_m, double east_m) {
    double dlat = north_m / 111320.0;
    double dlon = east_m / (111320.0 * std::cos(lat * DEG2RAD));
    return {lat + dlat, lon + dlon};
}

inline void run(TrackManager& tm, double duration_s, double speed) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-0.0003, 0.0003);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    double t = 0.0;
    while (t < duration_s) {
        // Drone A: Remote ID + RF always, EO/IR after 40s -- loiters near center
        double angle = (t / 120.0) * 2 * M_PI;
        auto [alat, alon] = offset_ll(CENTER_LAT, CENTER_LON, 300 * std::sin(angle), 300 * std::cos(angle));

        Detection rid;
        rid.sensor_type = "remote_id"; rid.sensor_id = "RID-RX-1";
        rid.lat = alat; rid.lon = alon; rid.alt_m = 80 + 5 * std::sin(t / 10);
        rid.timestamp = now_seconds();
        rid.metadata = {{"rid_serial", "1SZAM1234567890ABCD"}, {"operator_id", "OP-88213"}};
        tm.ingest_detection(rid);

        Detection rf;
        rf.sensor_type = "rf"; rf.sensor_id = "RF-SENSOR-1";
        rf.lat = alat + jitter(rng); rf.lon = alon + jitter(rng);
        rf.timestamp = now_seconds();
        rf.metadata = {{"band_mhz", 5800}, {"rssi_dbm", -55}};
        tm.ingest_detection(rf);

        if (t > 40) {
            Detection eo;
            eo.sensor_type = "eo_ir"; eo.sensor_id = "EOIR-CAM-1";
            eo.lat = alat + jitter(rng) * 0.3; eo.lon = alon + jitter(rng) * 0.3;
            eo.timestamp = now_seconds();
            eo.metadata = {{"track_lock", true}};
            tm.ingest_detection(eo);
        }

        // Bogey B: RF-only, no remote ID -- rogue-drone-like pattern
        if (t < 150) {
            auto [blat, blon] = offset_ll(CENTER_LAT, CENTER_LON, -600 + t * 4, 800 - t * 2);
            Detection brf;
            brf.sensor_type = "rf"; brf.sensor_id = "RF-SENSOR-2";
            brf.lat = blat + jitter(rng); brf.lon = blon + jitter(rng);
            brf.timestamp = now_seconds();
            brf.metadata = {{"band_mhz", 2400}, {"rssi_dbm", -70}};
            tm.ingest_detection(brf);
        }

        // Intruder C: RF + camera, crosses the default restricted zone
        if (t < 100) {
            auto start = offset_ll(CENTER_LAT, CENTER_LON, -400, -300);
            auto end = offset_ll(CENTER_LAT, CENTER_LON, -2000, 100);
            double frac = std::min(t / 100.0, 1.0);
            double ilat = start.first + (end.first - start.first) * frac;
            double ilon = start.second + (end.second - start.second) * frac;

            Detection irf;
            irf.sensor_type = "rf"; irf.sensor_id = "RF-SENSOR-3";
            irf.lat = ilat + jitter(rng); irf.lon = ilon + jitter(rng);
            irf.timestamp = now_seconds();
            irf.metadata = {{"band_mhz", 5800}, {"rssi_dbm", -50}};
            tm.ingest_detection(irf);

            if (((int)t) % 4 == 0) {
                Detection cam;
                cam.sensor_type = "camera"; cam.sensor_id = "CAM-PTZ-1";
                cam.lat = ilat + jitter(rng) * 0.3; cam.lon = ilon + jitter(rng) * 0.3;
                cam.timestamp = now_seconds();
                cam.metadata = {{"label", "drone"}, {"detector_confidence", 0.81}};
                tm.ingest_detection(cam);
            }
        }

        // Sparse clutter
        if (unit(rng) < 0.15) {
            std::uniform_real_distribution<double> spread(-1200, 1200);
            auto [clat, clon] = offset_ll(CENTER_LAT, CENTER_LON, spread(rng), spread(rng));
            Detection clutter;
            const char* types[] = {"rf", "acoustic", "radar"};
            clutter.sensor_type = types[(int)(unit(rng) * 3) % 3];
            clutter.sensor_id = "CLUTTER-SENSOR";
            clutter.lat = clat; clutter.lon = clon;
            clutter.timestamp = now_seconds();
            tm.ingest_detection(clutter);
        }

        t += 1.0;
        std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / speed));
    }
}

} // namespace simulate
