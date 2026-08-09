// kalman.hpp -- constant-velocity Kalman filter for track position/
// velocity estimation, replacing the fixed-alpha exponential position
// smoothing this project started with. That approach ("sensor fusion is
// the hard part" is exactly right) had no principled way to weight a
// measurement by its uncertainty, no real velocity estimate (speed was a
// raw two-point finite difference, prone to noise spikes when detections
// land close together in time -- the exact bug patched with
// MIN_SPEED_ESTIMATE_DT_SECONDS in fusion.hpp), and treated every update
// the same regardless of sensor confidence or time since the last fix.
//
// Operates in a local East-North tangent plane (meters) relative to the
// track's first position -- running the filter directly in lat/lon
// degrees would mix two axes with very different physical scale (a
// degree of longitude shrinks toward the poles) and bias the estimate.
// The two axes are independent 2-state (position, velocity) filters
// rather than one coupled 4-state filter, because the constant-velocity
// model has no cross-axis terms -- mathematically identical result, much
// simpler to implement and verify correctly.
#pragma once
#include <cmath>
#include <utility>
#include "geo_utils.hpp"

// One axis: state = [position, velocity], covariance P = [[p_pp, p_pv], [p_pv, p_vv]].
struct Kalman1D {
    double pos = 0, vel = 0;
    double p_pp = 25.0 * 25.0, p_pv = 0.0, p_vv = 20.0 * 20.0; // initial: ~25m position, ~20 m/s velocity uncertainty

    void predict(double dt, double q_accel) {
        pos += vel * dt;
        double new_p_pp = p_pp + 2 * dt * p_pv + dt * dt * p_vv;
        double new_p_pv = p_pv + dt * p_vv;
        // p_vv unchanged by the state transition itself (F leaves velocity
        // alone); constant-velocity process noise below is what lets the
        // filter track real acceleration.
        double dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt3 * dt;
        p_pp = new_p_pp + q_accel * dt4 / 4.0;
        p_pv = new_p_pv + q_accel * dt3 / 2.0;
        p_vv = p_vv + q_accel * dt2;
    }

    void correct(double measurement, double r) {
        double y = measurement - pos;      // innovation
        double s = p_pp + r;               // innovation covariance
        double k_pos = p_pp / s;           // Kalman gain
        double k_vel = p_pv / s;
        pos += k_pos * y;
        vel += k_vel * y;
        double new_p_pp = (1 - k_pos) * p_pp;
        double new_p_pv = (1 - k_pos) * p_pv;
        p_vv = p_vv - k_vel * p_pv;
        p_pp = new_p_pp;
        p_pv = new_p_pv;
    }
};

class KalmanTrackFilter {
public:
    // pos_noise_m: 1-sigma measurement noise at confidence 1.0 (GPS/sensor
    // position uncertainty). accel_noise_mps2: 1-sigma unmodeled
    // acceleration -- how sharply the target can maneuver between
    // updates. Both are tuning knobs, not physically measured constants;
    // defaults are reasonable for a small UAS, overridable via
    // TrackManagerConfig the same way the old smoothing alpha was.
    explicit KalmanTrackFilter(double pos_noise_m = 15.0, double accel_noise_mps2 = 3.0)
        : base_r_(pos_noise_m * pos_noise_m), q_accel_(accel_noise_mps2 * accel_noise_mps2) {}

    void init(double lat, double lon, double timestamp) {
        origin_lat_ = lat;
        origin_lon_ = lon;
        x_ = Kalman1D{};
        y_ = Kalman1D{};
        last_ts_ = timestamp;
        initialized_ = true;
    }

    // Fuses one position measurement, returning the filtered (lat, lon).
    // sensor_confidence in (0,1] widens the effective measurement noise
    // for low-confidence detections, so they pull the estimate less.
    std::pair<double, double> update(double lat, double lon, double timestamp, double sensor_confidence = 1.0) {
        if (!initialized_) { init(lat, lon, timestamp); return {lat, lon}; }

        double dt = timestamp - last_ts_;
        if (dt <= 0) dt = 0.001;      // guard against non-monotonic/duplicate timestamps
        dt = std::min(dt, 60.0);      // cap so a long gap doesn't blow up the covariance in one jump

        x_.predict(dt, q_accel_);
        y_.predict(dt, q_accel_);

        double conf = sensor_confidence > 0.05 ? sensor_confidence : 0.05;
        double r = base_r_ / conf;

        auto [mx, my] = to_local_meters(lat, lon);
        x_.correct(mx, r);
        y_.correct(my, r);

        last_ts_ = timestamp;
        return to_latlon(x_.pos, y_.pos);
    }

    double speed_mps() const { return std::sqrt(x_.vel * x_.vel + y_.vel * y_.vel); }
    std::pair<double, double> velocity_ms() const { return {x_.vel, y_.vel}; } // (east, north)

private:
    std::pair<double, double> to_local_meters(double lat, double lon) const {
        double dy = (lat - origin_lat_) * 111320.0;
        double dx = (lon - origin_lon_) * 111320.0 * std::cos(origin_lat_ * DEG2RAD);
        return {dx, dy};
    }
    std::pair<double, double> to_latlon(double x, double y) const {
        double lat = origin_lat_ + y / 111320.0;
        double lon = origin_lon_ + x / (111320.0 * std::cos(origin_lat_ * DEG2RAD));
        return {lat, lon};
    }

    bool initialized_ = false;
    double origin_lat_ = 0, origin_lon_ = 0;
    double last_ts_ = 0;
    double base_r_, q_accel_;
    Kalman1D x_, y_; // east, north
};
