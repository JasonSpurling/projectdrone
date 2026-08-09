// track_manager.hpp -- ingest detections, gated nearest-neighbor association,
// fusion classification, geofence checks, track lifecycle.
#pragma once
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <cstdlib>
#include <string>
#include "models.hpp"
#include "geo_utils.hpp"
#include "fusion.hpp"
#include "geofence.hpp"
#include "sensor_health.hpp"

// Tunable at runtime via env vars (defaults match the original hardcoded
// constants) so association/geofence sensitivity can be adjusted without a
// rebuild while these get tuned against real-world data over time.
struct TrackManagerConfig {
    double association_gate_meters = 250.0;
    double association_gate_seconds = 20.0;
    // Kalman filter tuning (see kalman.hpp) -- replaced the old fixed-alpha
    // exponential position smoothing (POSITION_SMOOTHING_ALPHA), which had
    // no principled way to weight a measurement by sensor confidence or
    // produce a real velocity estimate.
    double kalman_pos_noise_m = 15.0;
    double kalman_accel_noise_mps2 = 3.0;
    double track_stale_seconds = 30.0;
    double track_drop_seconds = 120.0;
    double geofence_min_confidence = 0.25;

    static double env_or(const char* name, double fallback) {
        const char* v = std::getenv(name);
        if (!v) return fallback;
        try { return std::stod(v); } catch (...) { return fallback; }
    }

    static TrackManagerConfig from_env() {
        TrackManagerConfig c;
        c.association_gate_meters = env_or("ASSOCIATION_GATE_METERS", c.association_gate_meters);
        c.association_gate_seconds = env_or("ASSOCIATION_GATE_SECONDS", c.association_gate_seconds);
        c.kalman_pos_noise_m = env_or("KALMAN_POS_NOISE_M", c.kalman_pos_noise_m);
        c.kalman_accel_noise_mps2 = env_or("KALMAN_ACCEL_NOISE_MPS2", c.kalman_accel_noise_mps2);
        c.track_stale_seconds = env_or("TRACK_STALE_SECONDS", c.track_stale_seconds);
        c.track_drop_seconds = env_or("TRACK_DROP_SECONDS", c.track_drop_seconds);
        c.geofence_min_confidence = env_or("GEOFENCE_MIN_CONFIDENCE", c.geofence_min_confidence);
        return c;
    }
};

class TrackManager {
public:
    using IncidentCallback = std::function<void(const Incident&)>;
    using DetectionCallback = std::function<void(const Detection&, const Track&)>;

    TrackManager(SensorHealth* sensor_health = nullptr)
        : sensor_health_(sensor_health), zones_(load_zones()), cfg_(TrackManagerConfig::from_env()) {}

    void set_incident_callback(IncidentCallback cb) { incident_cb_ = cb; }
    void set_detection_callback(DetectionCallback cb) { detection_cb_ = cb; }

    Track ingest_detection(const Detection& det) {
        if (sensor_health_) sensor_health_->mark_seen(det.sensor_id, det.sensor_type, det.timestamp);

        std::vector<Incident> incidents;
        Track track_copy;

        {
            std::lock_guard<std::mutex> lock(mu_);
            expire_tracks(det.timestamp);

            Track* track = find_candidate_track(det);
            if (!track) {
                Track t;
                t.track_id = next_id(id_counter_);
                t.kalman = KalmanTrackFilter(cfg_.kalman_pos_noise_m, cfg_.kalman_accel_noise_mps2);
                t.kalman.init(det.lat, det.lon, det.timestamp);
                t.lat = det.lat; t.lon = det.lon; t.alt_m = det.alt_m;
                t.created_at = det.timestamp; t.updated_at = det.timestamp;
                tracks_[t.track_id] = t;
                track = &tracks_[t.track_id];
            } else {
                auto [nlat, nlon] = track->kalman.update(det.lat, det.lon, det.timestamp, det.sensor_confidence);
                track->lat = nlat; track->lon = nlon;
                if (det.alt_m.has_value()) track->alt_m = det.alt_m;
                track->status = "active";
            }

            track->add_detection(det);

            auto result = fusion::classify_track(*track);
            track->classification = result.classification;
            track->confidence = result.confidence;
            track->fusion_reason = result.reason;

            if (fusion::should_alert(track->last_alert_confidence, result.confidence)) {
                Incident inc;
                inc.incident_id = next_id(incident_counter_);
                inc.track_id = track->track_id;
                inc.classification = result.classification;
                inc.confidence = result.confidence;
                inc.lat = track->lat; inc.lon = track->lon;
                inc.reason = result.reason;
                inc.timestamp = det.timestamp;
                inc.sensor_hit_counts = track->sensor_hit_counts;
                inc.incident_type = "classification";
                inc.evidence = track->evidence;
                incidents.push_back(inc);
                track->last_alert_confidence = result.confidence;
            }

            auto geofence_incident = check_geofence(*track, det.timestamp);
            if (geofence_incident.has_value()) incidents.push_back(*geofence_incident);

            track_copy = *track;
        }

        if (detection_cb_) detection_cb_(det, track_copy);
        if (incident_cb_) for (auto& inc : incidents) incident_cb_(inc);

        return track_copy;
    }

    std::vector<Track> get_active_tracks() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Track> out;
        for (auto& [id, t] : tracks_) if (t.status != "dropped") out.push_back(t);
        return out;
    }

    std::optional<Track> get_track(int64_t track_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tracks_.find(track_id);
        if (it == tracks_.end()) return std::nullopt;
        return it->second;
    }

    void sweep() {
        std::lock_guard<std::mutex> lock(mu_);
        expire_tracks(now_seconds());
    }

private:
    Track* find_candidate_track(const Detection& det) {
        Track* best = nullptr;
        double best_dist = -1;
        for (auto& [id, t] : tracks_) {
            if (t.status == "dropped") continue;
            if (det.timestamp - t.updated_at > cfg_.association_gate_seconds) continue;
            double dist = haversine_m(t.lat, t.lon, det.lat, det.lon);
            if (dist > cfg_.association_gate_meters) continue;
            if (best_dist < 0 || dist < best_dist) { best_dist = dist; best = &t; }
        }
        return best;
    }

    void expire_tracks(double now) {
        for (auto& [id, t] : tracks_) {
            double age = now - t.updated_at;
            if (age > cfg_.track_drop_seconds) t.status = "dropped";
            else if (age > cfg_.track_stale_seconds) t.status = "stale";
        }
    }

    std::optional<Incident> check_geofence(Track& track, double timestamp) {
        auto zone = zone_containing(track.lat, track.lon, zones_);
        std::optional<std::string> current_zone_id = zone.has_value() ? std::optional(zone->zone_id) : std::nullopt;
        std::optional<std::string> previous = track.in_restricted_zone;
        track.in_restricted_zone = current_zone_id;

        if (!zone.has_value() || current_zone_id == previous) return std::nullopt;
        if (track.confidence < cfg_.geofence_min_confidence) return std::nullopt;

        Incident inc;
        inc.incident_id = next_id(incident_counter_);
        inc.track_id = track.track_id;
        inc.classification = track.classification;
        inc.confidence = track.confidence;
        inc.lat = track.lat; inc.lon = track.lon;
        inc.reason = "Track entered restricted zone '" + zone->name + "' (" + zone->zone_id + ").";
        inc.timestamp = timestamp;
        inc.sensor_hit_counts = track.sensor_hit_counts;
        inc.incident_type = "geofence_violation";
        inc.zone_id = zone->zone_id;
        inc.zone_name = zone->name;
        inc.evidence = track.evidence;
        return inc;
    }

    std::map<int64_t, Track> tracks_;
    std::mutex mu_;
    std::atomic<int64_t> id_counter_{1};
    std::atomic<int64_t> incident_counter_{1};
    SensorHealth* sensor_health_;
    std::vector<Zone> zones_;
    TrackManagerConfig cfg_;
    IncidentCallback incident_cb_;
    DetectionCallback detection_cb_;
};
