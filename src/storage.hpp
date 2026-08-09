// storage.hpp -- SQLite-backed logging. Uses the plain C sqlite3 API
// (libsqlite3-dev), wrapped for RAII-ish safety and thread safety.
#pragma once
#include <sqlite3.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <cmath>
#include <tuple>
#include "models.hpp"

class Storage {
public:
    explicit Storage(const std::string& db_path = "data_flight.db") {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        }
        exec(R"SQL(
            CREATE TABLE IF NOT EXISTS detections (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                sensor_type TEXT NOT NULL, sensor_id TEXT NOT NULL,
                lat REAL NOT NULL, lon REAL NOT NULL, alt_m REAL,
                timestamp REAL NOT NULL, sensor_confidence REAL,
                metadata TEXT, track_id INTEGER
            );
            CREATE TABLE IF NOT EXISTS incidents (
                incident_id INTEGER PRIMARY KEY, track_id INTEGER NOT NULL,
                classification TEXT NOT NULL, confidence REAL NOT NULL,
                lat REAL NOT NULL, lon REAL NOT NULL, reason TEXT,
                timestamp REAL NOT NULL, sensor_hit_counts TEXT,
                incident_type TEXT DEFAULT 'classification',
                zone_id TEXT, zone_name TEXT
            );
            CREATE TABLE IF NOT EXISTS incident_feedback (
                id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id INTEGER NOT NULL,
                correct INTEGER NOT NULL, actual_classification TEXT,
                note TEXT, timestamp REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS aircraft_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                icao24 TEXT NOT NULL, callsign TEXT,
                lat REAL NOT NULL, lon REAL NOT NULL, alt_m REAL,
                velocity_mps REAL, heading_deg REAL, on_ground INTEGER,
                source TEXT, timestamp REAL NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_detections_track ON detections(track_id);
            CREATE INDEX IF NOT EXISTS idx_incidents_track ON incidents(track_id);
            CREATE INDEX IF NOT EXISTS idx_feedback_incident ON incident_feedback(incident_id);
            CREATE INDEX IF NOT EXISTS idx_aircraft_history_icao ON aircraft_history(icao24);
            CREATE INDEX IF NOT EXISTS idx_aircraft_history_ts ON aircraft_history(timestamp);
        )SQL");
        // Lightweight migration: `evidence` was added to the incidents
        // table after its initial release. ALTER TABLE errors if the
        // column already exists (including on every fresh CREATE TABLE
        // above, since it's run unconditionally right after) -- that
        // error is expected and ignored rather than a real failure.
        {
            char* err = nullptr;
            sqlite3_exec(db_, "ALTER TABLE incidents ADD COLUMN evidence TEXT", nullptr, nullptr, &err);
            if (err) sqlite3_free(err);
        }
    }

    ~Storage() { if (db_) sqlite3_close(db_); }
    Storage(const Storage&) = delete;

    void log_detection(const Detection& det, std::optional<int64_t> track_id) {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "INSERT INTO detections "
            "(sensor_type, sensor_id, lat, lon, alt_m, timestamp, sensor_confidence, metadata, track_id) "
            "VALUES (?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, det.sensor_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, det.sensor_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, det.lat);
        sqlite3_bind_double(stmt, 4, det.lon);
        if (det.alt_m.has_value()) sqlite3_bind_double(stmt, 5, *det.alt_m);
        else sqlite3_bind_null(stmt, 5);
        sqlite3_bind_double(stmt, 6, det.timestamp);
        sqlite3_bind_double(stmt, 7, det.sensor_confidence);
        sqlite3_bind_text(stmt, 8, det.metadata.dump().c_str(), -1, SQLITE_TRANSIENT);
        if (track_id.has_value()) sqlite3_bind_int64(stmt, 9, *track_id);
        else sqlite3_bind_null(stmt, 9);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Batched into one transaction -- a snapshot can be hundreds to
    // thousands of aircraft, and committing each row individually (the
    // sqlite3 default) would make this the slowest thing the process
    // does. `aircraft` is the same array shape /api/aircraft returns.
    void log_aircraft_snapshot(const json& aircraft, double timestamp) {
        if (aircraft.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        exec("BEGIN TRANSACTION");
        const char* sql = "INSERT INTO aircraft_history "
            "(icao24, callsign, lat, lon, alt_m, velocity_mps, heading_deg, on_ground, source, timestamp) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        for (auto& ac : aircraft) {
            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, ac.value("icao24", std::string()).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, ac.value("callsign", std::string()).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, ac.value("lat", 0.0));
            sqlite3_bind_double(stmt, 4, ac.value("lon", 0.0));
            if (ac.contains("altitude_m") && !ac["altitude_m"].is_null()) sqlite3_bind_double(stmt, 5, ac["altitude_m"].get<double>());
            else sqlite3_bind_null(stmt, 5);
            if (ac.contains("velocity_mps") && !ac["velocity_mps"].is_null()) sqlite3_bind_double(stmt, 6, ac["velocity_mps"].get<double>());
            else sqlite3_bind_null(stmt, 6);
            if (ac.contains("heading_deg") && !ac["heading_deg"].is_null()) sqlite3_bind_double(stmt, 7, ac["heading_deg"].get<double>());
            else sqlite3_bind_null(stmt, 7);
            sqlite3_bind_int(stmt, 8, ac.value("on_ground", false) ? 1 : 0);
            sqlite3_bind_text(stmt, 9, ac.value("source", std::string()).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 10, timestamp);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        exec("COMMIT");
    }

    // Bounds table growth for a long-running deployment -- a snapshot
    // every few minutes across possibly thousands of aircraft adds up
    // fast otherwise. Called periodically, not on every snapshot.
    void prune_aircraft_history(double older_than_days) {
        std::lock_guard<std::mutex> lock(mu_);
        double cutoff = now_seconds() - older_than_days * 86400.0;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, "DELETE FROM aircraft_history WHERE timestamp < ?", -1, &stmt, nullptr);
        sqlite3_bind_double(stmt, 1, cutoff);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Aircraft position history in [start_ts, end_ts], grouped by
    // icao24, for the same time-scrubber replay used for drone tracks --
    // see history_window() below for the drone-track equivalent this
    // mirrors.
    json aircraft_history_window(double start_ts, double end_ts, int limit_rows = 20000) {
        std::lock_guard<std::mutex> lock(mu_);
        std::map<std::string, json> planes; // icao24 -> {icao24, callsign, points:[...]}
        const char* sql = "SELECT icao24, callsign, lat, lon, alt_m, timestamp FROM aircraft_history "
                           "WHERE timestamp BETWEEN ? AND ? ORDER BY icao24, timestamp ASC LIMIT ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_double(stmt, 1, start_ts);
        sqlite3_bind_double(stmt, 2, end_ts);
        sqlite3_bind_int(stmt, 3, limit_rows);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string icao = text_col(stmt, 0);
            if (icao.empty()) continue;
            if (!planes.count(icao)) {
                planes[icao] = {{"icao24", icao}, {"callsign", text_col(stmt, 1)}, {"points", json::array()}};
            }
            json pt;
            pt["lat"] = sqlite3_column_double(stmt, 2);
            pt["lon"] = sqlite3_column_double(stmt, 3);
            pt["alt_m"] = sqlite3_column_type(stmt, 4) == SQLITE_NULL ? json(nullptr) : json(sqlite3_column_double(stmt, 4));
            pt["timestamp"] = sqlite3_column_double(stmt, 5);
            planes[icao]["points"].push_back(pt);
        }
        sqlite3_finalize(stmt);

        json out = json::array();
        for (auto& [icao, p] : planes) out.push_back(p);
        return out;
    }

    void log_incident(const Incident& inc) {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "INSERT INTO incidents "
            "(incident_id, track_id, classification, confidence, lat, lon, reason, timestamp, "
            " sensor_hit_counts, incident_type, zone_id, zone_name, evidence) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, inc.incident_id);
        sqlite3_bind_int64(stmt, 2, inc.track_id);
        sqlite3_bind_text(stmt, 3, inc.classification.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 4, inc.confidence);
        sqlite3_bind_double(stmt, 5, inc.lat);
        sqlite3_bind_double(stmt, 6, inc.lon);
        sqlite3_bind_text(stmt, 7, inc.reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 8, inc.timestamp);
        json shc = json::object();
        for (auto& [k, v] : inc.sensor_hit_counts) shc[k] = v;
        sqlite3_bind_text(stmt, 9, shc.dump().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, inc.incident_type.c_str(), -1, SQLITE_TRANSIENT);
        if (inc.zone_id.has_value()) sqlite3_bind_text(stmt, 11, inc.zone_id->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 11);
        if (inc.zone_name.has_value()) sqlite3_bind_text(stmt, 12, inc.zone_name->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 12);
        sqlite3_bind_text(stmt, 13, inc.evidence.dump().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    json recent_incidents(int limit = 100) {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "SELECT incident_id, track_id, classification, confidence, lat, lon, reason, "
            "timestamp, sensor_hit_counts, incident_type, zone_id, zone_name, evidence "
            "FROM incidents ORDER BY timestamp DESC LIMIT ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, limit);
        json out = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json j;
            j["incident_id"] = sqlite3_column_int64(stmt, 0);
            j["track_id"] = sqlite3_column_int64(stmt, 1);
            j["classification"] = text_col(stmt, 2);
            j["confidence"] = sqlite3_column_double(stmt, 3);
            j["lat"] = sqlite3_column_double(stmt, 4);
            j["lon"] = sqlite3_column_double(stmt, 5);
            j["reason"] = text_col(stmt, 6);
            j["timestamp"] = sqlite3_column_double(stmt, 7);
            j["sensor_hit_counts"] = text_col(stmt, 8); // client can parse if needed; kept simple
            j["incident_type"] = text_col(stmt, 9);
            j["zone_id"] = nullable_text_col(stmt, 10);
            j["zone_name"] = nullable_text_col(stmt, 11);
            j["evidence"] = parse_json_col(stmt, 12);
            out.push_back(j);
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Ground-truth capture for tuning the fusion/classification thresholds
    // over time ("progressive improvement") -- an operator confirms or
    // corrects a past alert's classification. This deliberately does not
    // feed back into the running classifier automatically: it's a rule-
    // based heuristic system, not a model that retrains itself, and
    // pretending otherwise would overstate what this does. Returns false
    // (no row written) if incident_id doesn't exist.
    bool log_feedback(int64_t incident_id, bool correct, const std::optional<std::string>& actual_classification,
                       const std::optional<std::string>& note) {
        std::lock_guard<std::mutex> lock(mu_);
        {
            const char* check_sql = "SELECT 1 FROM incidents WHERE incident_id = ?";
            sqlite3_stmt* check_stmt;
            sqlite3_prepare_v2(db_, check_sql, -1, &check_stmt, nullptr);
            sqlite3_bind_int64(check_stmt, 1, incident_id);
            bool exists = sqlite3_step(check_stmt) == SQLITE_ROW;
            sqlite3_finalize(check_stmt);
            if (!exists) return false;
        }
        const char* sql = "INSERT INTO incident_feedback (incident_id, correct, actual_classification, note, timestamp) "
                           "VALUES (?,?,?,?,?)";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, incident_id);
        sqlite3_bind_int(stmt, 2, correct ? 1 : 0);
        if (actual_classification.has_value()) sqlite3_bind_text(stmt, 3, actual_classification->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 3);
        if (note.has_value()) sqlite3_bind_text(stmt, 4, note->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 4);
        sqlite3_bind_double(stmt, 5, now_seconds());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }

    // Aggregates feedback by the classification each alert originally
    // carried, so it's visible per-class whether e.g. "possible_drone"
    // alerts get confirmed less often than "confirmed_drone" ones --
    // exactly the signal needed to decide which threshold to adjust.
    json accuracy_summary() {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "SELECT i.classification, f.correct, f.actual_classification "
                           "FROM incident_feedback f JOIN incidents i ON f.incident_id = i.incident_id";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);

        int total = 0, confirmed = 0;
        std::map<std::string, int> confirmed_by_class, corrected_by_class;
        std::map<std::string, std::map<std::string, int>> corrections_to;
        std::set<std::string> classes_seen;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string orig_class = text_col(stmt, 0);
            bool ok = sqlite3_column_int(stmt, 1) != 0;
            std::string actual = text_col(stmt, 2);
            classes_seen.insert(orig_class);
            total++;
            if (ok) { confirmed++; confirmed_by_class[orig_class]++; }
            else {
                corrected_by_class[orig_class]++;
                if (!actual.empty()) corrections_to[orig_class][actual]++;
            }
        }
        sqlite3_finalize(stmt);

        json by_class = json::object();
        for (auto& c : classes_seen) {
            json ctos = json::object();
            for (auto& [k, v] : corrections_to[c]) ctos[k] = v;
            by_class[c] = {
                {"confirmed", confirmed_by_class.count(c) ? confirmed_by_class[c] : 0},
                {"corrected", corrected_by_class.count(c) ? corrected_by_class[c] : 0},
                {"corrections_to", ctos},
            };
        }

        json out;
        out["total_feedback"] = total;
        out["confirmed"] = confirmed;
        out["corrected"] = total - confirmed;
        out["accuracy"] = total > 0 ? json(static_cast<double>(confirmed) / total) : json(nullptr);
        out["by_classification"] = by_class;
        return out;
    }

    // Full historical reconstruction of a track's flight path -- unlike
    // the in-memory Track.history (capped at 50 detections, and gone
    // entirely once a track is swept/dropped), this reads the permanent
    // SQLite log, so a dropped track's full path is still recoverable.
    json detections_for_track(int64_t track_id, int limit = 5000) {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "SELECT sensor_type, sensor_id, lat, lon, alt_m, timestamp, sensor_confidence, metadata "
                           "FROM detections WHERE track_id = ? ORDER BY timestamp ASC LIMIT ?";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, track_id);
        sqlite3_bind_int(stmt, 2, limit);
        json out = json::array();
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json j;
            j["sensor_type"] = text_col(stmt, 0);
            j["sensor_id"] = text_col(stmt, 1);
            j["lat"] = sqlite3_column_double(stmt, 2);
            j["lon"] = sqlite3_column_double(stmt, 3);
            j["alt_m"] = sqlite3_column_type(stmt, 4) == SQLITE_NULL ? json(nullptr) : json(sqlite3_column_double(stmt, 4));
            j["timestamp"] = sqlite3_column_double(stmt, 5);
            j["sensor_confidence"] = sqlite3_column_double(stmt, 6);
            j["metadata"] = parse_json_col(stmt, 7);
            out.push_back(j);
        }
        sqlite3_finalize(stmt);
        return out;
    }

    int geofence_incident_count(int64_t track_id) {
        std::lock_guard<std::mutex> lock(mu_);
        const char* sql = "SELECT COUNT(*) FROM incidents WHERE track_id = ? AND incident_type = 'geofence_violation'";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, track_id);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count;
    }

    // Historical replay: every track's detection point sequence within
    // [start_ts, end_ts], reconstructed from the permanent log, plus the
    // incidents in that window for markers. This is position replay only
    // -- it does not reconstruct what classification a track carried at
    // each past instant (the detections table doesn't record that), so
    // the frontend uses each track's most recent classification for the
    // whole replay rather than an accurate historical value.
    json history_window(double start_ts, double end_ts, int limit_rows = 20000) {
        std::lock_guard<std::mutex> lock(mu_);

        std::map<int64_t, json> tracks; // track_id -> {track_id, classification, points:[...]}
        {
            const char* sql = "SELECT track_id, sensor_type, lat, lon, alt_m, timestamp FROM detections "
                               "WHERE track_id IS NOT NULL AND timestamp BETWEEN ? AND ? "
                               "ORDER BY track_id, timestamp ASC LIMIT ?";
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
            sqlite3_bind_double(stmt, 1, start_ts);
            sqlite3_bind_double(stmt, 2, end_ts);
            sqlite3_bind_int(stmt, 3, limit_rows);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t track_id = sqlite3_column_int64(stmt, 0);
                if (!tracks.count(track_id)) {
                    tracks[track_id] = {{"track_id", track_id}, {"points", json::array()}};
                }
                json pt;
                pt["sensor_type"] = text_col(stmt, 1);
                pt["lat"] = sqlite3_column_double(stmt, 2);
                pt["lon"] = sqlite3_column_double(stmt, 3);
                pt["alt_m"] = sqlite3_column_type(stmt, 4) == SQLITE_NULL ? json(nullptr) : json(sqlite3_column_double(stmt, 4));
                pt["timestamp"] = sqlite3_column_double(stmt, 5);
                tracks[track_id]["points"].push_back(pt);
            }
            sqlite3_finalize(stmt);
        }

        // Most recent classification on record for each track (not
        // necessarily what it was mid-window -- see note above).
        {
            const char* sql = "SELECT track_id, classification FROM incidents WHERE track_id = ? ORDER BY timestamp DESC LIMIT 1";
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
            for (auto& [track_id, tj] : tracks) {
                sqlite3_reset(stmt);
                sqlite3_bind_int64(stmt, 1, track_id);
                tj["classification"] = (sqlite3_step(stmt) == SQLITE_ROW) ? text_col(stmt, 1) : std::string("unknown");
            }
            sqlite3_finalize(stmt);
        }

        json out;
        json track_arr = json::array();
        for (auto& [id, tj] : tracks) track_arr.push_back(tj);
        out["tracks"] = track_arr;

        json incidents = json::array();
        {
            const char* sql = "SELECT incident_id, track_id, classification, confidence, lat, lon, reason, timestamp, "
                               "incident_type, zone_id, zone_name FROM incidents WHERE timestamp BETWEEN ? AND ? "
                               "ORDER BY timestamp ASC";
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
            sqlite3_bind_double(stmt, 1, start_ts);
            sqlite3_bind_double(stmt, 2, end_ts);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json j;
                j["incident_id"] = sqlite3_column_int64(stmt, 0);
                j["track_id"] = sqlite3_column_int64(stmt, 1);
                j["classification"] = text_col(stmt, 2);
                j["confidence"] = sqlite3_column_double(stmt, 3);
                j["lat"] = sqlite3_column_double(stmt, 4);
                j["lon"] = sqlite3_column_double(stmt, 5);
                j["reason"] = text_col(stmt, 6);
                j["timestamp"] = sqlite3_column_double(stmt, 7);
                j["incident_type"] = text_col(stmt, 8);
                j["zone_id"] = nullable_text_col(stmt, 9);
                j["zone_name"] = nullable_text_col(stmt, 10);
                incidents.push_back(j);
            }
            sqlite3_finalize(stmt);
        }
        out["incidents"] = incidents;
        out["start"] = start_ts;
        out["end"] = end_ts;
        return out;
    }

    json stats_summary() {
        std::lock_guard<std::mutex> lock(mu_);
        auto scalar_int = [&](const char* sql) {
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
            int v = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            return v;
        };
        auto group_counts = [&](const char* sql) {
            json out = json::object();
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW) out[text_col(stmt, 0)] = sqlite3_column_int(stmt, 1);
            sqlite3_finalize(stmt);
            return out;
        };

        json out;
        out["total_detections"] = scalar_int("SELECT COUNT(*) FROM detections");
        out["total_tracks_seen"] = scalar_int("SELECT COUNT(DISTINCT track_id) FROM detections WHERE track_id IS NOT NULL");
        out["total_incidents"] = scalar_int("SELECT COUNT(*) FROM incidents");
        out["incidents_by_classification"] = group_counts("SELECT classification, COUNT(*) FROM incidents GROUP BY classification");
        out["detections_by_sensor_type"] = group_counts("SELECT sensor_type, COUNT(*) FROM detections GROUP BY sensor_type");
        out["geofence_incidents_by_zone"] = group_counts(
            "SELECT zone_name, COUNT(*) FROM incidents WHERE incident_type='geofence_violation' AND zone_name IS NOT NULL GROUP BY zone_name");

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, "SELECT MIN(timestamp), MAX(timestamp) FROM detections", -1, &stmt, nullptr);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            out["earliest_detection"] = sqlite3_column_double(stmt, 0);
            out["latest_detection"] = sqlite3_column_double(stmt, 1);
        } else {
            out["earliest_detection"] = nullptr;
            out["latest_detection"] = nullptr;
        }
        sqlite3_finalize(stmt);
        return out;
    }

    // Basic link analysis: (a) the same declared Remote ID identity
    // reappearing under a different track_id later -- tracks get dropped
    // and a new one created when a drone goes out of range and comes
    // back, so this is what ties those sightings back together; (b) rough
    // grid-based clustering of each track's first-seen position, to spot
    // common launch/operating areas. Parsed here in C++ rather than via
    // SQLite's json_extract() -- can't verify the JSON1 extension is
    // compiled into every libsqlite3 build this might run against, so
    // this stays portable rather than relying on it.
    json link_analysis(int limit_rows = 5000) {
        std::lock_guard<std::mutex> lock(mu_);

        std::map<std::string, std::set<int64_t>> serial_to_tracks;
        {
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_,
                "SELECT track_id, metadata FROM detections WHERE sensor_type='remote_id' AND track_id IS NOT NULL "
                "ORDER BY timestamp DESC LIMIT ?", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, limit_rows);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t track_id = sqlite3_column_int64(stmt, 0);
                json meta = parse_json_col(stmt, 1);
                std::string serial;
                // metadata is either the raw decoded ODID message array
                // (from /api/remoteid/decode) or a flat object (from
                // /api/detections, e.g. the simulator's "rid_serial") --
                // handle both shapes.
                if (meta.is_array()) {
                    for (auto& m : meta) {
                        if (m.value("type", "") == "basic_id" && m.contains("uas_id")) {
                            serial = m.value("uas_id", std::string());
                        }
                    }
                } else if (meta.is_object()) {
                    if (meta.contains("rid_serial")) serial = meta.value("rid_serial", std::string());
                    else if (meta.contains("uas_id")) serial = meta.value("uas_id", std::string());
                }
                if (!serial.empty()) serial_to_tracks[serial].insert(track_id);
            }
            sqlite3_finalize(stmt);
        }
        json reappearances = json::array();
        for (auto& [serial, tracks] : serial_to_tracks) {
            if (tracks.size() > 1) {
                json ids = json::array();
                for (auto id : tracks) ids.push_back(id);
                reappearances.push_back({{"identifier", serial}, {"track_ids", ids}});
            }
        }

        std::map<int64_t, std::pair<double, double>> first_seen;
        {
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_,
                "SELECT track_id, lat, lon FROM detections WHERE track_id IS NOT NULL "
                "ORDER BY timestamp ASC LIMIT ?", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, limit_rows);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t track_id = sqlite3_column_int64(stmt, 0);
                if (first_seen.count(track_id)) continue; // rows are timestamp ASC, so first hit is earliest
                first_seen[track_id] = {sqlite3_column_double(stmt, 1), sqlite3_column_double(stmt, 2)};
            }
            sqlite3_finalize(stmt);
        }
        constexpr double GRID_DEG = 0.001; // ~110m at the equator
        std::map<std::pair<int64_t, int64_t>, int> grid_counts;
        for (auto& [tid, pos] : first_seen) {
            int64_t gx = static_cast<int64_t>(std::floor(pos.first / GRID_DEG));
            int64_t gy = static_cast<int64_t>(std::floor(pos.second / GRID_DEG));
            grid_counts[{gx, gy}]++;
        }
        json clusters = json::array();
        for (auto& [cell, count] : grid_counts) {
            if (count > 1) {
                clusters.push_back({
                    {"lat", (cell.first + 0.5) * GRID_DEG},
                    {"lon", (cell.second + 0.5) * GRID_DEG},
                    {"track_count", count},
                });
            }
        }

        json out;
        out["identifier_reappearances"] = reappearances;
        out["common_launch_areas"] = clusters;
        return out;
    }

    // Graph-powered link analysis: nodes are tracks/identifiers/sensors/
    // zones, edges are the relationships actually observed between them
    // (a track broadcasting an identifier, a track detected by a sensor,
    // a track entering a zone). Built in-process from the same
    // detections/incidents tables link_analysis() reads -- no separate
    // graph database engine, since nothing about this dataset's size or
    // query pattern needs one, and adding one would be a real new
    // dependency for a feature that doesn't require it. If focus_node is
    // given (e.g. "track:5" or "identifier:ABC123"), returns just that
    // node and its direct neighbors -- "show me everything connected to
    // X" -- otherwise the whole graph (capped by limit_rows, same as
    // link_analysis()).
    json link_graph(std::optional<std::string> focus_node = std::nullopt, int limit_rows = 5000) {
        std::lock_guard<std::mutex> lock(mu_);

        std::map<std::string, json> nodes;
        // Keyed by (source, target, type) so repeated observations of the
        // same relationship (e.g. 50 detections from the same sensor)
        // collapse into one edge with a weight, instead of one edge per
        // row -- a graph is nodes/relationships, not a raw event log.
        std::map<std::tuple<std::string, std::string, std::string>, int> edge_weights;
        auto ensure_node = [&](const std::string& id, const std::string& type, const std::string& label) {
            if (!nodes.count(id)) nodes[id] = {{"id", id}, {"type", type}, {"label", label}};
        };
        auto add_edge = [&](const std::string& src, const std::string& dst, const std::string& type) {
            edge_weights[{src, dst, type}]++;
        };

        {
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_,
                "SELECT track_id, sensor_id, metadata FROM detections WHERE track_id IS NOT NULL "
                "ORDER BY timestamp DESC LIMIT ?", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, limit_rows);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t track_id = sqlite3_column_int64(stmt, 0);
                std::string sensor_id = text_col(stmt, 1);
                json meta = parse_json_col(stmt, 2);

                std::string track_node = "track:" + std::to_string(track_id);
                ensure_node(track_node, "track", "Track #" + std::to_string(track_id));

                if (!sensor_id.empty()) {
                    std::string sensor_node = "sensor:" + sensor_id;
                    ensure_node(sensor_node, "sensor", sensor_id);
                    add_edge(track_node, sensor_node, "detected_by");
                }

                std::string serial;
                if (meta.is_array()) {
                    for (auto& m : meta) if (m.value("type", "") == "basic_id" && m.contains("uas_id")) serial = m.value("uas_id", std::string());
                } else if (meta.is_object()) {
                    if (meta.contains("rid_serial")) serial = meta.value("rid_serial", std::string());
                    else if (meta.contains("uas_id")) serial = meta.value("uas_id", std::string());
                }
                if (!serial.empty()) {
                    std::string id_node = "identifier:" + serial;
                    ensure_node(id_node, "identifier", serial);
                    add_edge(track_node, id_node, "broadcasts_identity");
                }
            }
            sqlite3_finalize(stmt);
        }

        {
            sqlite3_stmt* stmt;
            sqlite3_prepare_v2(db_,
                "SELECT track_id, zone_id, zone_name FROM incidents WHERE incident_type='geofence_violation' "
                "AND zone_id IS NOT NULL ORDER BY timestamp DESC LIMIT ?", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, limit_rows);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t track_id = sqlite3_column_int64(stmt, 0);
                std::string zone_id = text_col(stmt, 1);
                std::string zone_name = text_col(stmt, 2);
                if (zone_id.empty()) continue;

                std::string track_node = "track:" + std::to_string(track_id);
                ensure_node(track_node, "track", "Track #" + std::to_string(track_id));
                std::string zone_node = "zone:" + zone_id;
                ensure_node(zone_node, "zone", zone_name.empty() ? zone_id : zone_name);
                add_edge(track_node, zone_node, "entered_zone");
            }
            sqlite3_finalize(stmt);
        }

        json out;
        if (focus_node.has_value()) {
            std::set<std::string> keep_nodes = {*focus_node};
            json kept_edges = json::array();
            for (auto& [key, weight] : edge_weights) {
                auto& [src, dst, type] = key;
                if (src == *focus_node || dst == *focus_node) {
                    kept_edges.push_back({{"source", src}, {"target", dst}, {"type", type}, {"weight", weight}});
                    keep_nodes.insert(src);
                    keep_nodes.insert(dst);
                }
            }
            json node_arr = json::array();
            for (auto& id : keep_nodes) if (nodes.count(id)) node_arr.push_back(nodes[id]);
            out["nodes"] = node_arr;
            out["edges"] = kept_edges;
            out["focus"] = *focus_node;
        } else {
            json node_arr = json::array();
            for (auto& [id, n] : nodes) node_arr.push_back(n);
            json edge_arr = json::array();
            for (auto& [key, weight] : edge_weights) {
                auto& [src, dst, type] = key;
                edge_arr.push_back({{"source", src}, {"target", dst}, {"type", type}, {"weight", weight}});
            }
            out["nodes"] = node_arr;
            out["edges"] = edge_arr;
        }
        return out;
    }

private:
    static std::string text_col(sqlite3_stmt* stmt, int col) {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }

    static json nullable_text_col(sqlite3_stmt* stmt, int col) {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        return t ? json(std::string(reinterpret_cast<const char*>(t))) : json(nullptr);
    }

    // Unlike nullable_text_col, this parses the stored text back into a
    // real JSON value rather than returning it as a string -- used for
    // columns (like evidence) that hold a dumped JSON object the API
    // should re-expose as structured data, not a double-encoded string.
    // Rows written before the evidence column existed have a NULL here,
    // which becomes an empty object rather than null.
    static json parse_json_col(sqlite3_stmt* stmt, int col) {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        if (!t) return json::object();
        try { return json::parse(reinterpret_cast<const char*>(t)); }
        catch (...) { return json::object(); }
    }

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("SQLite error: " + msg);
        }
    }

    sqlite3* db_ = nullptr;
    std::mutex mu_;
};
