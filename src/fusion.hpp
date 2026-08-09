// fusion.hpp -- rule-based drone classification, same logic as the Python version.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sstream>
#include "models.hpp"
#include "geo_utils.hpp"

namespace fusion {

// Envelope heuristics for telling a manned aircraft/helicopter apart from a
// small UAS using only altitude + ground speed (the fields available from
// non-cooperative sensors like radar/RF/camera -- Remote ID gives a direct
// answer when present, but most of these sensors don't). These thresholds
// are deliberately conservative (well outside anything a consumer/prosumer
// drone can plausibly do) so they only override the drone-confidence score
// on strong evidence, not marginal cases.
constexpr double AIRCRAFT_ALTITUDE_THRESHOLD_M = 1500.0; // well above any legal/plausible small-UAS ceiling
constexpr double AIRCRAFT_SPEED_THRESHOLD_MPS = 60.0;    // ~134mph, beyond virtually all consumer/prosumer drones
constexpr double HELICOPTER_ALTITUDE_THRESHOLD_M = 150.0; // above the ~120m/400ft Part 107 UAS ceiling
constexpr double HOVER_SPEED_THRESHOLD_MPS = 15.0;

// Ground speed from the track's Kalman filter (see kalman.hpp) rather
// than a raw two-point finite difference between the last two
// detections. The old two-point approach had no way to distinguish real
// velocity from noise -- two different-sensor detections landing
// milliseconds apart on the same track could produce a wildly inflated
// instantaneous speed (this was patched with an ad-hoc minimum-dt
// threshold before the Kalman filter existed). A Kalman-filtered
// velocity is damped by the filter's own uncertainty tracking instead:
// a measurement that arrives before much process-noise uncertainty has
// accumulated gets a small velocity gain, so it can't swing the
// estimate on its own the way a raw derivative could.
inline double estimate_speed_mps(const Track& track) {
    return track.kalman.speed_mps();
}

inline const std::map<std::string, double>& sensor_weights() {
    static const std::map<std::string, double> w = {
        {"remote_id", 0.55}, {"rf", 0.35}, {"radar", 0.30},
        {"eo_ir", 0.45}, {"camera", 0.40}, {"acoustic", 0.25},
    };
    return w;
}

inline double noisy_or(const std::vector<double>& weights) {
    double p_none = 1.0;
    for (double w : weights) p_none *= (1.0 - w);
    return 1.0 - p_none;
}

inline std::string band_classification(double confidence) {
    if (confidence >= 0.85) return "confirmed_drone";
    if (confidence >= 0.60) return "likely_drone";
    if (confidence >= 0.30) return "possible_drone";
    return "unknown";
}

struct ClassificationResult {
    std::string classification;
    double confidence;
    std::string reason;
};

inline ClassificationResult classify_track(const Track& track) {
    auto sensors = track.sensor_types_seen();
    if (sensors.empty()) return {"unknown", 0.0, "No detections associated with track."};

    std::vector<double> weights;
    auto& sw = sensor_weights();
    for (auto& s : sensors) {
        auto it = sw.find(s);
        weights.push_back(it != sw.end() ? it->second : 0.2);
    }
    double confidence = noisy_or(weights);

    std::ostringstream reason;
    std::vector<std::string> sorted_sensors = sensors;
    std::sort(sorted_sensors.begin(), sorted_sensors.end());
    reason << "Evidence from: ";
    for (size_t i = 0; i < sorted_sensors.size(); ++i) {
        reason << sorted_sensors[i] << (i + 1 < sorted_sensors.size() ? ", " : "");
    }
    reason << ".";

    auto has = [&](const std::string& s) {
        return std::find(sensors.begin(), sensors.end(), s) != sensors.end();
    };

    if (has("remote_id") && has("rf")) {
        confidence = std::max(confidence, 0.80);
        reason << " Remote ID broadcast + co-located RF energy: strong corroborated drone signature.";
    }
    if (sensors.size() >= 3) {
        confidence = std::min(confidence + 0.10, 0.97);
        reason << " " << sensors.size() << " independent sensor types corroborate this track.";
    }
    if (has("eo_ir") && sensors.size() >= 2) {
        confidence = std::min(confidence + 0.05, 0.97);
        reason << " Visual/thermal confirmation adds high-confidence corroboration.";
    }
    if (sensors.size() == 1 && sensors[0] == "remote_id") {
        confidence = std::min(confidence, 0.55);
        reason << " Remote ID only, uncorroborated: capped below 'confirmed'.";
    }

    confidence = std::clamp(confidence, 0.0, 1.0);

    // Envelope override: no amount of corroborating sensor hits makes a
    // track cruising at 200 m/s and 3000m altitude an actual small drone --
    // this outranks the sensor-count confidence math above rather than
    // just nudging it, since it's evidence about physical plausibility,
    // not sensor corroboration.
    double speed = estimate_speed_mps(track);
    bool has_remote_id = has("remote_id");
    if ((track.alt_m.has_value() && *track.alt_m > AIRCRAFT_ALTITUDE_THRESHOLD_M) ||
        speed > AIRCRAFT_SPEED_THRESHOLD_MPS) {
        reason << " Altitude/speed (" << (track.alt_m.has_value() ? *track.alt_m : 0.0) << "m, "
               << speed << "m/s) is well outside a small UAS's plausible envelope -- "
               << "reclassified from drone-confidence scoring to aircraft.";
        return {"aircraft", confidence, reason.str()};
    }
    if (!has_remote_id && track.alt_m.has_value() && *track.alt_m > HELICOPTER_ALTITUDE_THRESHOLD_M &&
        speed < HOVER_SPEED_THRESHOLD_MPS) {
        reason << " Altitude (" << *track.alt_m << "m) is above typical small-UAS ceiling with "
               << "low/hovering speed and no Remote ID broadcast -- consistent with a manned "
               << "helicopter rather than a drone.";
        return {"helicopter", confidence, reason.str()};
    }

    return {band_classification(confidence), confidence, reason.str()};
}

// Alert threshold logic
constexpr double INCIDENT_CONFIDENCE_THRESHOLD = 0.60;
constexpr double INCIDENT_REALERT_DELTA = 0.15;

inline bool should_alert(double last_alert_confidence, double new_confidence) {
    if (new_confidence < INCIDENT_CONFIDENCE_THRESHOLD) return false;
    if (new_confidence >= last_alert_confidence + INCIDENT_REALERT_DELTA) return true;
    if (last_alert_confidence < INCIDENT_CONFIDENCE_THRESHOLD) return true;
    return false;
}

} // namespace fusion
