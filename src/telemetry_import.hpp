// telemetry_import.hpp -- generic importer for recorded/open drone
// telemetry (CSV logs exported from a GCS like Mission Planner/
// QGroundControl, a converted DJI flight log, or any other source that can
// be reduced to timestamp+position rows) into the same Detection/Track
// pipeline used by live sensors, so imported tracks get the same fusion
// classification and geofence checks as everything else.
//
// Column/field names are matched case-insensitively against a few common
// aliases (see FIELD_ALIASES below) rather than a single fixed schema,
// since "common drone telemetry log" isn't one format in practice.
// Honest scope note: only numeric epoch timestamps (seconds or
// milliseconds, auto-detected by magnitude) are supported -- ISO-8601 or
// other date-string timestamp formats are not parsed in this pass. Rows
// missing lat/lon are skipped, not rejected wholesale, so one bad row
// doesn't fail an entire import.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "models.hpp"
#include "track_manager.hpp"

namespace telemetry {

inline std::string lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Same minimal RFC4180-ish field splitter used by airports_feed.hpp,
// duplicated locally to keep each feed/importer header self-contained.
inline std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
                else in_quotes = false;
            } else field += c;
        } else {
            if (c == '"') in_quotes = true;
            else if (c == ',') { out.push_back(field); field.clear(); }
            else field += c;
        }
    }
    out.push_back(field);
    return out;
}

inline double normalize_timestamp(double raw) {
    // Magnitude heuristic: anything above ~1e12 is almost certainly
    // milliseconds (that's year 33658 in seconds, but year 2001+ in ms);
    // treat it as ms and convert. Below that, assume seconds already.
    return (raw > 1e12) ? (raw / 1000.0) : raw;
}

struct FieldMap {
    std::map<std::string, size_t> col; // lowercased column name -> index (CSV only)

    std::optional<size_t> find(const std::vector<std::string>& aliases) const {
        for (auto& a : aliases) { auto it = col.find(a); if (it != col.end()) return it->second; }
        return std::nullopt;
    }
};

inline const std::vector<std::string>& alias_lat() { static const std::vector<std::string> v = {"lat", "latitude"}; return v; }
inline const std::vector<std::string>& alias_lon() { static const std::vector<std::string> v = {"lon", "lng", "long", "longitude"}; return v; }
inline const std::vector<std::string>& alias_alt() { static const std::vector<std::string> v = {"alt", "altitude", "alt_m", "altitude_m"}; return v; }
inline const std::vector<std::string>& alias_ts()  { static const std::vector<std::string> v = {"timestamp", "time", "time_s", "time_ms", "epoch"}; return v; }
inline const std::vector<std::string>& alias_speed() { static const std::vector<std::string> v = {"speed", "velocity", "speed_mps", "groundspeed"}; return v; }
inline const std::vector<std::string>& alias_heading() { static const std::vector<std::string> v = {"heading", "track", "course", "yaw"}; return v; }

struct ImportResult {
    int imported = 0;
    int skipped = 0;
    std::vector<std::string> errors;
    std::vector<int64_t> track_ids; // de-duplicated, insertion order
};

inline void note_track(ImportResult& r, int64_t id) {
    if (std::find(r.track_ids.begin(), r.track_ids.end(), id) == r.track_ids.end()) r.track_ids.push_back(id);
}

inline std::optional<double> parse_double(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try { size_t pos; double v = std::stod(s, &pos); return v; } catch (...) { return std::nullopt; }
}

inline ImportResult import_csv(const std::string& csv, const std::string& sensor_id, const std::string& sensor_type, TrackManager& tm) {
    ImportResult result;
    std::istringstream stream(csv);
    std::string line;
    FieldMap fm;
    bool header_seen = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto fields = split_csv_line(line);

        if (!header_seen) {
            for (size_t i = 0; i < fields.size(); ++i) fm.col[lower(fields[i])] = i;
            header_seen = true;
            if (!fm.find(alias_lat()) || !fm.find(alias_lon())) {
                result.errors.push_back("CSV header has no recognizable latitude/longitude column");
                return result;
            }
            continue;
        }

        auto lat_i = fm.find(alias_lat()), lon_i = fm.find(alias_lon());
        auto get = [&](std::optional<size_t> idx) -> std::string {
            return (idx.has_value() && *idx < fields.size()) ? fields[*idx] : std::string();
        };

        auto lat = parse_double(get(lat_i));
        auto lon = parse_double(get(lon_i));
        if (!lat.has_value() || !lon.has_value()) { result.skipped++; continue; }

        Detection det;
        det.sensor_type = sensor_type;
        det.sensor_id = sensor_id;
        det.lat = *lat;
        det.lon = *lon;
        if (auto alt = parse_double(get(fm.find(alias_alt())))) det.alt_m = alt;
        if (auto ts = parse_double(get(fm.find(alias_ts())))) det.timestamp = normalize_timestamp(*ts);
        else det.timestamp = now_seconds();

        json meta = json::object();
        if (auto sp = parse_double(get(fm.find(alias_speed())))) meta["speed_mps"] = *sp;
        if (auto hd = parse_double(get(fm.find(alias_heading())))) meta["heading_deg"] = *hd;
        det.metadata = meta;

        Track t = tm.ingest_detection(det);
        note_track(result, t.track_id);
        result.imported++;
    }
    return result;
}

inline ImportResult import_json(const json& records, const std::string& sensor_id, const std::string& sensor_type, TrackManager& tm) {
    ImportResult result;
    if (!records.is_array()) { result.errors.push_back("expected a JSON array of telemetry records"); return result; }

    auto field = [](const json& rec, const std::vector<std::string>& aliases) -> std::optional<double> {
        for (auto& a : aliases) {
            if (rec.contains(a) && !rec[a].is_null()) {
                try { return rec[a].get<double>(); } catch (...) {}
            }
        }
        return std::nullopt;
    };

    for (auto& rec : records) {
        if (!rec.is_object()) { result.skipped++; continue; }
        auto lat = field(rec, alias_lat()), lon = field(rec, alias_lon());
        if (!lat.has_value() || !lon.has_value()) { result.skipped++; continue; }

        Detection det;
        det.sensor_type = rec.value("sensor_type", sensor_type);
        det.sensor_id = rec.value("sensor_id", sensor_id);
        // A per-record override here bypasses whatever validation the
        // HTTP handler already did on the request-level defaults -- these
        // strings flow into fusion_reason/sensor_hit_counts, which the
        // frontend renders unescaped, so an unvalidated value here is a
        // stored-XSS vector same as the top-level params.
        if (!is_safe_identifier(det.sensor_type) || !is_safe_identifier(det.sensor_id)) { result.skipped++; continue; }
        det.lat = *lat;
        det.lon = *lon;
        if (auto alt = field(rec, alias_alt())) det.alt_m = alt;
        if (auto ts = field(rec, alias_ts())) det.timestamp = normalize_timestamp(*ts);
        else det.timestamp = now_seconds();

        json meta = json::object();
        if (auto sp = field(rec, alias_speed())) meta["speed_mps"] = *sp;
        if (auto hd = field(rec, alias_heading())) meta["heading_deg"] = *hd;
        det.metadata = meta;

        Track t = tm.ingest_detection(det);
        note_track(result, t.track_id);
        result.imported++;
    }
    return result;
}

} // namespace telemetry
