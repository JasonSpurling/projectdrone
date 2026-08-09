// models.hpp -- core data structures shared across the fusion pipeline.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <atomic>
#include <cctype>
#include <nlohmann/json.hpp>
#include "kalman.hpp"

using json = nlohmann::json;

inline double now_seconds() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline int64_t next_id(std::atomic<int64_t>& counter) {
    return counter.fetch_add(1);
}

// sensor_type and sensor_id are client-supplied on several ingestion
// endpoints (POST /api/detections, telemetry_import's per-record
// override, the sensor_id param on remoteid/decode and rf/analyze) and
// flow, unmodified, into fusion_reason text and sensor_hit_counts map
// keys -- which the frontend renders via innerHTML without escaping.
// An unvalidated sensor_type of e.g. "<img src=x onerror=...>" is a
// confirmed working stored-XSS payload against the operator's browser.
// Restricting these to a safe identifier character set closes that at
// the source, for every current and future ingestion path, rather than
// relying on every frontend render site remembering to escape.
inline bool is_safe_identifier(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

// ---------------------------------------------------------------------
struct Detection {
    std::string sensor_type;   // remote_id | rf | radar | eo_ir | camera | acoustic
    std::string sensor_id;
    double lat = 0, lon = 0;
    std::optional<double> alt_m;
    double timestamp = 0;
    double sensor_confidence = 1.0;
    // Client-supplied JSON, essentially unconstrained -- unlike sensor_type/
    // sensor_id (validated by is_safe_identifier() above), metadata is
    // meant to carry arbitrary structured data (decoded Remote ID
    // messages, RF analysis results, GCS log fields) and can't be
    // restricted to a safe-identifier charset without breaking that.
    // It flows unmodified into Track.evidence -> Incident.evidence and is
    // returned as-is by the JSON API and CSV export, neither of which
    // HTML-escape it (nor should they -- that would corrupt the raw data
    // for non-HTML consumers). It is NOT rendered anywhere in the current
    // frontend. If you build a UI panel that displays evidence/metadata
    // fields, you MUST run every string value through escapeHtml() (or
    // the recursive escapeHtmlDeep() in index.html) at render time --
    // Remote ID Self-ID descriptions, serials, and operator IDs are
    // free text broadcast by the drone and are exactly as attacker-
    // controlled as the sensor_type field that was the confirmed XSS
    // vector fixed earlier in this project's history.
    json metadata = json::object();

    json to_json() const {
        json j{
            {"sensor_type", sensor_type}, {"sensor_id", sensor_id},
            {"lat", lat}, {"lon", lon}, {"timestamp", timestamp},
            {"sensor_confidence", sensor_confidence}, {"metadata", metadata},
        };
        j["alt_m"] = alt_m.has_value() ? json(*alt_m) : json(nullptr);
        return j;
    }
};

// ---------------------------------------------------------------------
struct Track {
    int64_t track_id = 0;
    double lat = 0, lon = 0;
    std::optional<double> alt_m;
    double created_at = 0, updated_at = 0;
    std::string status = "active";           // active | stale | dropped
    std::string classification = "unclassified";
    double confidence = 0.0;
    std::map<std::string, int> sensor_hit_counts;
    double last_alert_confidence = 0.0;
    std::vector<Detection> history;          // bounded
    std::string fusion_reason;
    json evidence = json::object();          // remote_id_fields, rf_summary, latest_visual -- see the metadata field comment on Detection above: unescaped, not currently rendered, must be escaped if that changes
    json snapshots = json::array();
    std::optional<std::string> in_restricted_zone;
    // Constant-velocity Kalman filter backing this track's position/
    // velocity estimate -- see kalman.hpp. Not serialized (to_json()
    // below doesn't touch it); copies of Track (e.g. the per-request
    // snapshot in ingest_detection) carry a snapshot of its state, which
    // is what's wanted since a new filter mid-flight would lose the
    // velocity estimate it's built up.
    KalmanTrackFilter kalman;

    static constexpr size_t MAX_HISTORY = 50;

    void add_detection(const Detection& det) {
        history.push_back(det);
        if (history.size() > MAX_HISTORY) history.erase(history.begin());
        sensor_hit_counts[det.sensor_type]++;
        updated_at = det.timestamp;
        update_evidence(det);
    }

    void update_evidence(const Detection& det) {
        if (det.sensor_type == "remote_id") {
            evidence["remote_id_fields"] = det.metadata;
        } else if (det.sensor_type == "rf") {
            evidence["rf_summary"] = {
                {"band_mhz", det.metadata.value("band_mhz", json(nullptr))},
                {"rssi_dbm", det.metadata.value("rssi_dbm", json(nullptr))},
                {"last_seen", det.timestamp},
            };
        } else if (det.sensor_type == "camera" || det.sensor_type == "eo_ir") {
            evidence["latest_visual"] = {
                {"sensor_type", det.sensor_type},
                {"bounding_box", det.metadata.value("bounding_box", json(nullptr))},
                {"detector_label", det.metadata.value("label", json(nullptr))},
            };
        }
    }

    std::vector<std::string> sensor_types_seen() const {
        std::vector<std::string> out;
        for (auto& [k, v] : sensor_hit_counts) out.push_back(k);
        return out;
    }

    json to_json() const {
        json shc = json::object();
        for (auto& [k, v] : sensor_hit_counts) shc[k] = v;
        json j{
            {"track_id", track_id}, {"lat", lat}, {"lon", lon},
            {"created_at", created_at}, {"updated_at", updated_at},
            {"status", status}, {"classification", classification},
            {"confidence", confidence}, {"sensor_hit_counts", shc},
            {"fusion_reason", fusion_reason}, {"detection_count", history.size()},
            {"evidence", evidence}, {"snapshots", snapshots},
        };
        j["alt_m"] = alt_m.has_value() ? json(*alt_m) : json(nullptr);
        j["in_restricted_zone"] = in_restricted_zone.has_value() ? json(*in_restricted_zone) : json(nullptr);
        return j;
    }
};

// ---------------------------------------------------------------------
struct Incident {
    int64_t incident_id = 0;
    int64_t track_id = 0;
    std::string classification;
    double confidence = 0;
    double lat = 0, lon = 0;
    std::string reason;
    double timestamp = 0;
    std::map<std::string, int> sensor_hit_counts;
    std::string incident_type = "classification"; // classification | geofence_violation
    std::optional<std::string> zone_id, zone_name;
    // Snapshot of the track's supporting evidence (remote_id_fields,
    // rf_summary, latest_visual) at the moment the alert fired -- captured
    // here rather than only living on the Track, since the Track's
    // evidence keeps mutating as new detections arrive and the alert
    // should record what actually justified it at the time. Same caveat
    // as Detection::metadata above: unescaped, attacker-reachable free
    // text (Remote ID descriptions/serials/operator IDs), not currently
    // rendered by the frontend -- escape it if that ever changes.
    json evidence = json::object();

    json to_json() const {
        json shc = json::object();
        for (auto& [k, v] : sensor_hit_counts) shc[k] = v;
        json j{
            {"incident_id", incident_id}, {"track_id", track_id},
            {"classification", classification}, {"confidence", confidence},
            {"lat", lat}, {"lon", lon}, {"reason", reason},
            {"timestamp", timestamp}, {"sensor_hit_counts", shc},
            {"incident_type", incident_type}, {"evidence", evidence},
        };
        j["zone_id"] = zone_id.has_value() ? json(*zone_id) : json(nullptr);
        j["zone_name"] = zone_name.has_value() ? json(*zone_name) : json(nullptr);
        return j;
    }
};

// ---------------------------------------------------------------------
// A single live aircraft, from the global OpenSky feed.
struct Aircraft {
    std::string icao24;
    std::string callsign;
    std::string origin_country;
    double lat = 0, lon = 0;
    std::optional<double> altitude_m;
    std::optional<double> velocity_mps;
    std::optional<double> heading_deg;
    std::optional<double> vertical_rate_mps;
    bool on_ground = false;
    double updated_at = 0;

    json to_json() const {
        json j{
            {"icao24", icao24}, {"callsign", callsign}, {"origin_country", origin_country},
            {"lat", lat}, {"lon", lon}, {"on_ground", on_ground}, {"updated_at", updated_at},
        };
        j["altitude_m"] = altitude_m.has_value() ? json(*altitude_m) : json(nullptr);
        j["velocity_mps"] = velocity_mps.has_value() ? json(*velocity_mps) : json(nullptr);
        j["heading_deg"] = heading_deg.has_value() ? json(*heading_deg) : json(nullptr);
        j["vertical_rate_mps"] = vertical_rate_mps.has_value() ? json(*vertical_rate_mps) : json(nullptr);
        return j;
    }
};

// ---------------------------------------------------------------------
// A restricted-airspace polygon, from FAA or another public source.
struct RestrictedArea {
    std::string name;
    std::string source;       // "faa_uas_facility_map" | "faa_national_security" | "openaip"
    std::string kind;         // "altitude_ceiling" | "no_fly"
    std::optional<double> ceiling_ft;
    std::vector<std::pair<double,double>> polygon; // (lat, lon) ring

    json to_json() const {
        json ring = json::array();
        for (auto& [la, lo] : polygon) ring.push_back({la, lo});
        json j{
            {"name", name}, {"source", source}, {"kind", kind}, {"polygon", ring},
        };
        j["ceiling_ft"] = ceiling_ft.has_value() ? json(*ceiling_ft) : json(nullptr);
        return j;
    }
};
