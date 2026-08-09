// rf_spectrum.hpp -- heuristic RF signature analysis over already-computed
// spectrum data (frequency/power samples), the kind of output existing SDR
// tools already produce (rtl_power, HackRF sweep, GNU Radio's FFT sink,
// a spectrum analyzer's CSV export, etc).
//
// Honest scope note: this does NOT do signal processing on raw IQ samples
// (no FFT/demodulation) -- that's a much larger scope (real-time DSP,
// windowing, calibration against the specific SDR hardware) that belongs
// in the capture tool, not this HTTP service. This takes power-vs-frequency
// readings as input and applies frequency-band + bandwidth + (optionally)
// channel-hopping heuristics on top. It is a best-effort classifier, not a
// validated detector -- treat its output as a lead to investigate, not a
// confirmed identification, and expect to tune the thresholds against your
// own receiver's noise floor.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include "models.hpp"

namespace rf {

struct Sample { double freq_hz; double power_dbm; };

struct Band { double lo_hz, hi_hz; const char* name; };

// FCC Part 15 ISM bands where consumer drone control/video links commonly
// operate. Real deployments vary by region/manufacturer -- this is a
// starting point, not an exhaustive regulatory reference.
inline const std::vector<Band>& drone_relevant_bands() {
    static const std::vector<Band> bands = {
        {902e6, 928e6, "900MHz_ISM"},
        {2400e6, 2483.5e6, "2.4GHz_ISM"},
        {5725e6, 5875e6, "5.8GHz_ISM"},
    };
    return bands;
}

inline const Band* band_for(double freq_hz) {
    for (auto& b : drone_relevant_bands()) if (freq_hz >= b.lo_hz && freq_hz <= b.hi_hz) return &b;
    return nullptr;
}

struct PeakCluster {
    double center_hz, bandwidth_hz, peak_power_dbm;
};

// Noise floor: median power across the sweep. Median rather than mean so a
// handful of strong signals don't drag the floor estimate upward.
inline double estimate_noise_floor(const std::vector<Sample>& samples) {
    if (samples.empty()) return -120.0;
    std::vector<double> powers;
    powers.reserve(samples.size());
    for (auto& s : samples) powers.push_back(s.power_dbm);
    std::sort(powers.begin(), powers.end());
    return powers[powers.size() / 2];
}

// Groups contiguous above-threshold samples (assumes samples are frequency-
// sorted, as sweep output naturally is) into peak clusters.
inline std::vector<PeakCluster> find_peaks(const std::vector<Sample>& samples, double threshold_above_floor_db = 10.0) {
    std::vector<PeakCluster> clusters;
    if (samples.empty()) return clusters;
    double floor = estimate_noise_floor(samples);
    double threshold = floor + threshold_above_floor_db;

    size_t i = 0;
    while (i < samples.size()) {
        if (samples[i].power_dbm < threshold) { ++i; continue; }
        size_t start = i;
        double peak_power = samples[i].power_dbm;
        while (i < samples.size() && samples[i].power_dbm >= threshold) {
            peak_power = std::max(peak_power, samples[i].power_dbm);
            ++i;
        }
        size_t end = i - 1;
        double lo = samples[start].freq_hz, hi = samples[end].freq_hz;
        clusters.push_back({(lo + hi) / 2.0, hi - lo, peak_power});
    }
    return clusters;
}

struct SignalClassification {
    double center_hz, bandwidth_hz, peak_power_dbm;
    std::string band_name;
    std::string classification;
    double confidence;
    std::string reason;
};

inline SignalClassification classify_cluster(const PeakCluster& c) {
    SignalClassification r;
    r.center_hz = c.center_hz;
    r.bandwidth_hz = c.bandwidth_hz;
    r.peak_power_dbm = c.peak_power_dbm;

    const Band* band = band_for(c.center_hz);
    if (!band) {
        r.band_name = "outside_known_bands";
        r.classification = "unknown_signal";
        r.confidence = 0.1;
        r.reason = "Center frequency isn't in a band this heuristic recognizes as drone-relevant.";
        return r;
    }
    r.band_name = band->name;

    // Rough bandwidth fingerprints: a standard Wi-Fi channel is ~20MHz
    // (or 40/80MHz for wider modes); most drone C2 links are much
    // narrower-band per hop (typically under a few MHz); analog/digital
    // FPV video downlinks tend to sit in between (a few MHz to ~20MHz).
    if (c.bandwidth_hz >= 18e6 && c.bandwidth_hz <= 85e6) {
        r.classification = "wifi_like";
        r.confidence = 0.5;
        r.reason = "Bandwidth matches a Wi-Fi channel width more than a typical narrowband C2 link -- could still be Wi-Fi FPV.";
    } else if (c.bandwidth_hz < 3e6) {
        r.classification = "possible_drone_control_link";
        r.confidence = 0.45;
        r.reason = "Narrowband signal in a drone-relevant ISM band, consistent with a C2 uplink hop -- single-sweep evidence only, confirm with a hop-pattern check across multiple sweeps.";
    } else {
        r.classification = "possible_video_downlink";
        r.confidence = 0.35;
        r.reason = "Mid-bandwidth signal in a drone-relevant ISM band, consistent with an analog/digital FPV video downlink.";
    }
    return r;
}

// Frequency-hopping spread spectrum (FHSS) is the classic tell for a drone
// C2 link: many distinct narrowband channels used in rotation rather than
// one fixed frequency. Given multiple sweeps taken over time, this counts
// how many distinct channels (by rounding to a bin) show peak activity --
// a high distinct-channel count relative to sweep count is hop-like
// behavior; a single dominant channel is more consistent with fixed Wi-Fi
// or continuous video.
inline json detect_hopping(const std::vector<std::vector<Sample>>& sweeps, double bin_hz = 1e6) {
    std::map<int64_t, int> channel_hits;
    for (auto& sweep : sweeps) {
        for (auto& c : find_peaks(sweep)) {
            int64_t bin = static_cast<int64_t>(c.center_hz / bin_hz);
            channel_hits[bin]++;
        }
    }
    json j;
    j["sweeps_analyzed"] = sweeps.size();
    j["distinct_channels_seen"] = channel_hits.size();
    bool hopping_like = sweeps.size() >= 3 && channel_hits.size() >= 3;
    j["hopping_pattern_detected"] = hopping_like;
    j["reason"] = hopping_like
        ? "Peak activity moved across several distinct channels over the sweep sequence -- consistent with FHSS, as used by many drone C2 links."
        : "Not enough sweeps, or activity stayed on too few channels, to call this a hopping pattern.";
    return j;
}

inline json analyze(const std::vector<Sample>& samples) {
    auto clusters = find_peaks(samples);
    json signals = json::array();
    double best_confidence = 0.0;
    for (auto& c : clusters) {
        auto cls = classify_cluster(c);
        best_confidence = std::max(best_confidence, cls.confidence);
        signals.push_back({
            {"center_hz", cls.center_hz}, {"bandwidth_hz", cls.bandwidth_hz},
            {"peak_power_dbm", cls.peak_power_dbm}, {"band", cls.band_name},
            {"classification", cls.classification}, {"confidence", cls.confidence},
            {"reason", cls.reason},
        });
    }
    json j;
    j["noise_floor_dbm"] = estimate_noise_floor(samples);
    j["signals"] = signals;
    j["overall_confidence"] = best_confidence;
    return j;
}

} // namespace rf
