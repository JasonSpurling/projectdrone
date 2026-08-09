// analytics.hpp -- local trajectory analysis, anomaly detection, and a
// learned statistical baseline, all computed from data this system has
// itself collected (Track history in memory + the SQLite-backed
// detection/incident log). No network calls, no external services.
//
// Honest scope note on "local machine-learning models": this implements
// Welford's online mean/variance algorithm -- classical streaming
// statistics, not a neural network or any framework-backed model. Pulling
// in TensorFlow/libtorch/onnxruntime for this would be a large,
// hard-to-verify build-complexity increase (matching the project's
// existing zero-heavy-dependency philosophy) for a use case that doesn't
// need it: a per-classification running mean/stddev of speed and altitude,
// updated incrementally as real detections arrive, used to flag
// statistically unusual tracks via z-score. It's genuinely "trained" (in
// the sense of being fit) on this system's own collected data and
// improves as more data arrives -- just not a deep model. It is also
// in-memory only and resets on restart; it does not bootstrap from
// historical SQLite data, since the detections table doesn't record what
// classification a track had at the time of each historical detection.
#pragma once
#include <vector>
#include <map>
#include <set>
#include <string>
#include <mutex>
#include <cmath>
#include <optional>
#include "models.hpp"
#include "geo_utils.hpp"

namespace analytics {

// ---------------------------------------------------------------------
// Trajectory / pattern analysis
// ---------------------------------------------------------------------

struct TrajectoryStats {
    double total_distance_m = 0;
    double duration_s = 0;
    double avg_speed_mps = 0;
    double max_speed_mps = 0;
    double min_alt_m = 0, max_alt_m = 0, avg_alt_m = 0;
    double total_heading_change_deg = 0;
    bool loiter_detected = false;
    double loiter_radius_m = 0;
};

inline double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double phi1 = lat1 * DEG2RAD, phi2 = lat2 * DEG2RAD, dl = (lon2 - lon1) * DEG2RAD;
    double y = std::sin(dl) * std::cos(phi2);
    double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(dl);
    double brg = std::atan2(y, x) * 180.0 / M_PI;
    return brg < 0 ? brg + 360.0 : brg;
}

constexpr double LOITER_RADIUS_M = 150.0;
constexpr double LOITER_MIN_DURATION_S = 30.0;
constexpr size_t LOITER_MIN_DETECTIONS = 4;

inline TrajectoryStats compute_trajectory(const std::vector<Detection>& history) {
    TrajectoryStats s;
    if (history.empty()) return s;

    double clat = 0, clon = 0;
    for (auto& d : history) { clat += d.lat; clon += d.lon; }
    clat /= history.size(); clon /= history.size();
    double max_dist_from_centroid = 0;

    double sum_alt = 0; int alt_n = 0;
    std::vector<double> speeds;
    std::optional<double> prev_bearing;

    for (size_t i = 0; i < history.size(); ++i) {
        auto& d = history[i];
        if (d.alt_m.has_value()) {
            sum_alt += *d.alt_m;
            s.min_alt_m = (alt_n == 0) ? *d.alt_m : std::min(s.min_alt_m, *d.alt_m);
            s.max_alt_m = (alt_n == 0) ? *d.alt_m : std::max(s.max_alt_m, *d.alt_m);
            alt_n++;
        }
        max_dist_from_centroid = std::max(max_dist_from_centroid, haversine_m(d.lat, d.lon, clat, clon));

        if (i > 0) {
            auto& p = history[i - 1];
            double dist = haversine_m(p.lat, p.lon, d.lat, d.lon);
            double dt = d.timestamp - p.timestamp;
            s.total_distance_m += dist;
            if (dt > 0) {
                double sp = dist / dt;
                speeds.push_back(sp);
                s.max_speed_mps = std::max(s.max_speed_mps, sp);
            }
            double brg = bearing_deg(p.lat, p.lon, d.lat, d.lon);
            if (prev_bearing.has_value()) {
                double diff = std::fabs(brg - *prev_bearing);
                if (diff > 180.0) diff = 360.0 - diff;
                s.total_heading_change_deg += diff;
            }
            prev_bearing = brg;
        }
    }

    s.duration_s = history.back().timestamp - history.front().timestamp;
    s.avg_alt_m = alt_n > 0 ? sum_alt / alt_n : 0.0;
    if (!speeds.empty()) {
        double sum = 0; for (double v : speeds) sum += v;
        s.avg_speed_mps = sum / speeds.size();
    }

    // Loiter/circling: stayed within a tight radius of its own centroid
    // for a meaningful stretch despite several detections -- consistent
    // with hovering/circling surveillance rather than transit.
    if (history.size() >= LOITER_MIN_DETECTIONS && s.duration_s >= LOITER_MIN_DURATION_S &&
        max_dist_from_centroid <= LOITER_RADIUS_M) {
        s.loiter_detected = true;
        s.loiter_radius_m = max_dist_from_centroid;
    }
    return s;
}

// ---------------------------------------------------------------------
// Flight-pattern heuristics (drone vs. bird vs. aircraft)
//
// Honest scope: this project has no labeled dataset of real bird, drone,
// and aircraft tracks to train or validate a classifier against -- "a lot
// harder" than the aircraft/helicopter envelope checks turned out to be
// exactly right. What follows is a hand-built heuristic score from
// physically-motivated signals (flapping flight is erratic in heading
// and speed in a way stabilized flight isn't; only a multirotor can hold
// a genuinely stationary hover; only cooperative traffic broadcasts an
// identity), not a trained model, and it has not been measured against
// real bird tracks. Treat bird_likelihood as a lead to investigate, the
// same caution already documented for the RF spectrum heuristics.
// ---------------------------------------------------------------------

struct FlightPatternSignal {
    double bird_likelihood = 0.0; // 0-1 heuristic score, NOT a trained/validated classifier
    std::vector<std::string> notes;
};

constexpr double BIRD_MIN_HEADING_CHANGE_PER_KM = 200.0; // cumulative heading change (deg) per km traveled -- erratic path
constexpr double BIRD_SPEED_VARIABILITY_RATIO = 0.5;     // (max-avg)/avg ground speed -- flapping-like speed variation
constexpr size_t FLIGHT_PATTERN_MIN_DETECTIONS = 4;
constexpr double FLIGHT_PATTERN_MIN_DISTANCE_M = 20.0;

inline FlightPatternSignal analyze_flight_pattern(const Track& track, const TrajectoryStats& traj) {
    FlightPatternSignal sig;
    if (track.history.size() < FLIGHT_PATTERN_MIN_DETECTIONS || traj.total_distance_m < FLIGHT_PATTERN_MIN_DISTANCE_M) {
        return sig; // not enough movement to say anything meaningful either way
    }

    bool has_cooperative_signal = track.sensor_hit_counts.count("remote_id") > 0 || track.sensor_hit_counts.count("rf") > 0;
    double heading_change_per_km = traj.total_heading_change_deg / (traj.total_distance_m / 1000.0);
    double speed_variability = traj.avg_speed_mps > 0.5 ? (traj.max_speed_mps - traj.avg_speed_mps) / traj.avg_speed_mps : 0.0;

    double score = 0.0;
    if (!has_cooperative_signal) {
        score += 0.3;
        sig.notes.push_back("No Remote ID/RF signature -- necessary but not sufficient on its own, "
                             "a drone with no active link would also lack this");
    }
    if (heading_change_per_km > BIRD_MIN_HEADING_CHANGE_PER_KM) {
        score += 0.35;
        sig.notes.push_back("Erratic heading relative to distance traveled, consistent with flapping flight");
    }
    if (speed_variability > BIRD_SPEED_VARIABILITY_RATIO) {
        score += 0.35;
        sig.notes.push_back("High speed variability relative to average, consistent with flapping flight rather than a stabilized platform");
    }
    if (traj.loiter_detected) {
        // Most birds can't hold a genuinely stationary hover the way a
        // multirotor can -- sustained near-zero-drift position pulls the
        // score back down even if the other signals fired.
        score = std::max(0.0, score - 0.4);
        sig.notes.push_back("Sustained near-stationary hover reduces bird likelihood");
    }

    sig.bird_likelihood = std::min(1.0, score);
    return sig;
}

// ---------------------------------------------------------------------
// Fixed-threshold anomaly heuristics
// ---------------------------------------------------------------------

struct AnomalyFlags {
    bool rapid_altitude_change = false;
    bool rapid_speed_change = false;
    bool loitering = false;
    bool repeated_zone_incursion = false;
    std::vector<std::string> notes;
};

constexpr double RAPID_ALTITUDE_CHANGE_M = 50.0;
constexpr double RAPID_ALTITUDE_CHANGE_WINDOW_S = 5.0;
constexpr double RAPID_SPEED_CHANGE_MPS = 20.0;

inline AnomalyFlags detect_anomalies(const Track& track, int zone_incursion_count) {
    AnomalyFlags flags;
    auto& h = track.history;

    for (size_t i = 1; i < h.size(); ++i) {
        double dt = h[i].timestamp - h[i - 1].timestamp;
        if (dt <= 0 || dt > RAPID_ALTITUDE_CHANGE_WINDOW_S) continue;

        if (h[i].alt_m.has_value() && h[i - 1].alt_m.has_value()) {
            double dalt = std::fabs(*h[i].alt_m - *h[i - 1].alt_m);
            if (dalt > RAPID_ALTITUDE_CHANGE_M) {
                flags.rapid_altitude_change = true;
                flags.notes.push_back("Altitude changed ~" + std::to_string(static_cast<int>(dalt)) +
                                       "m in " + std::to_string(dt) + "s");
            }
        }

        if (i >= 2) {
            double dt_prev = h[i - 1].timestamp - h[i - 2].timestamp;
            if (dt_prev > 0) {
                double speed = haversine_m(h[i - 1].lat, h[i - 1].lon, h[i].lat, h[i].lon) / dt;
                double prev_speed = haversine_m(h[i - 2].lat, h[i - 2].lon, h[i - 1].lat, h[i - 1].lon) / dt_prev;
                if (std::fabs(speed - prev_speed) > RAPID_SPEED_CHANGE_MPS) {
                    flags.rapid_speed_change = true;
                    flags.notes.push_back("Speed changed by ~" +
                                           std::to_string(static_cast<int>(std::fabs(speed - prev_speed))) +
                                           "m/s between consecutive detections");
                }
            }
        }
    }

    auto traj = compute_trajectory(h);
    flags.loitering = traj.loiter_detected;
    if (flags.loitering) {
        flags.notes.push_back("Loitering within ~" + std::to_string(static_cast<int>(traj.loiter_radius_m)) +
                               "m for " + std::to_string(static_cast<int>(traj.duration_s)) + "s");
    }

    if (zone_incursion_count > 1) {
        flags.repeated_zone_incursion = true;
        flags.notes.push_back(std::to_string(zone_incursion_count) + " separate zone incursions recorded for this track");
    }
    return flags;
}

// ---------------------------------------------------------------------
// Learned baseline (Welford's online mean/variance) -- see file header.
// ---------------------------------------------------------------------

struct RunningStat {
    int n = 0;
    double mean = 0, m2 = 0;

    void update(double x) {
        n++;
        double delta = x - mean;
        mean += delta / n;
        double delta2 = x - mean;
        m2 += delta * delta2;
    }
    double variance() const { return n > 1 ? m2 / (n - 1) : 0.0; }
    double stddev() const { return std::sqrt(variance()); }
    // z-score is 0 (not flagged) until there's enough of a baseline to
    // trust a stddev estimate from, and while stddev is ~0 (would
    // otherwise divide into a huge/undefined value).
    double zscore(double x) const {
        if (n < 10) return 0.0;
        double sd = stddev();
        return sd > 1e-9 ? (x - mean) / sd : 0.0;
    }
};

class LearnedBaselineModel {
public:
    void observe(const std::string& classification, double speed_mps, double alt_m) {
        std::lock_guard<std::mutex> lock(mu_);
        speed_by_class_[classification].update(speed_mps);
        alt_by_class_[classification].update(alt_m);
    }

    double speed_zscore(const std::string& classification, double speed_mps) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = speed_by_class_.find(classification);
        return it != speed_by_class_.end() ? it->second.zscore(speed_mps) : 0.0;
    }
    double altitude_zscore(const std::string& classification, double alt_m) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = alt_by_class_.find(classification);
        return it != alt_by_class_.end() ? it->second.zscore(alt_m) : 0.0;
    }

    json state() {
        std::lock_guard<std::mutex> lock(mu_);
        json out = json::object();
        std::set<std::string> classes;
        for (auto& [k, v] : speed_by_class_) classes.insert(k);
        for (auto& [k, v] : alt_by_class_) classes.insert(k);
        for (auto& c : classes) {
            json j;
            if (speed_by_class_.count(c)) {
                auto& s = speed_by_class_[c];
                j["speed_mps"] = {{"n", s.n}, {"mean", s.mean}, {"stddev", s.stddev()}};
            }
            if (alt_by_class_.count(c)) {
                auto& a = alt_by_class_[c];
                j["altitude_m"] = {{"n", a.n}, {"mean", a.mean}, {"stddev", a.stddev()}};
            }
            out[c] = j;
        }
        return out;
    }

private:
    std::mutex mu_;
    std::map<std::string, RunningStat> speed_by_class_, alt_by_class_;
};

} // namespace analytics
