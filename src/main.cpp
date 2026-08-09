// main.cpp -- Data Flight server entry point.
#include <iostream>
#include <csignal>
#include <cstring>
#include <thread>
#include <chrono>
#include <optional>
#include <algorithm>
#include <sstream>
#include <cmath>
#include "http_server.hpp"
#include "storage.hpp"
#include "track_manager.hpp"
#include "sensor_health.hpp"
#include "opensky_feed.hpp"
#include "community_adsb_feed.hpp"
#include "faa_airspace.hpp"
#include "airports_feed.hpp"
#include "runways_feed.hpp"
#include "airport_boundary.hpp"
#include "simulate_feed.hpp"
#include "remote_id_decoder.hpp"
#include "manufacturer_registry.hpp"
#include "rf_spectrum.hpp"
#include "telemetry_import.hpp"
#include "analytics.hpp"
#include "faa_aircraft_registry.hpp"
#include "icao_allocation.hpp"
#include "type_designators.hpp"

// Byte-for-byte equal in time proportional to length, not to the position
// of the first mismatch -- an early-exit compare here would let a network
// attacker recover DETECTIONS_API_KEY one byte at a time via response
// timing.
inline bool constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

// Every route below that can create tracks/detections shares this same
// gate as POST /api/detections -- they're the same class of unauthenticated
// data-injection risk. Returns the 401 to send, or nullopt if the request
// may proceed.
inline std::optional<HttpResponse> check_api_key(const HttpRequest& req, const std::string& configured_key) {
    if (configured_key.empty()) return std::nullopt;
    if (constant_time_equals(req.header("X-API-Key"), configured_key)) return std::nullopt;
    return HttpResponse{401, R"({"error":"missing or invalid X-API-Key"})", "application/json"};
}

int main(int argc, char** argv) {
    bool simulate_mode = false;
    double sim_speed = 6.0;
    int port = 5050;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--simulate") simulate_mode = true;
        else if (arg == "--sim-speed" && i + 1 < argc) sim_speed = std::stod(argv[++i]);
        else if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    }

    const char* detections_key_env = std::getenv("DETECTIONS_API_KEY");
    std::string detections_api_key = detections_key_env ? detections_key_env : "";
    if (detections_api_key.empty()) {
        std::cout << "WARNING: DETECTIONS_API_KEY not set -- POST /api/detections is open to "
                     "anyone with network access. Set DETECTIONS_API_KEY to require an X-API-Key "
                     "header on detection ingestion.\n";
    }

    // /data (not the working directory, /app) so a named volume mounted
    // there is all that's needed for incident history to survive a
    // container recreation -- see the Dockerfile comment on why /app
    // itself isn't the mount point. Overridable for non-Docker use.
    const char* db_path_env = std::getenv("DB_PATH");
    Storage storage(db_path_env ? db_path_env : "/data/data_flight.db");
    SensorHealth sensor_health;
    TrackManager track_manager(&sensor_health);
    OpenSkyFeed opensky;
    // Redundant/failover aircraft sources, each off unless its own
    // *_LAT/*_LON or *_REGIONS env var is set. Two free community feeds
    // rather than one -- see community_adsb_feed.hpp for why adsb.fi and
    // ADS-B Exchange aren't included (no working free endpoint for either
    // at the time this was wired up).
    CommunityAdsbFeed adsblol("ADSBLOL", "https://api.adsb.lol", "adsblol");
    CommunityAdsbFeed airplaneslive("AIRPLANESLIVE", "https://api.airplanes.live", "airplaneslive");
    AirspaceFeed airspace;
    AirportsFeed airports;
    RunwaysFeed runways;
    AirportBoundaryLookup airport_boundary;
    // Cached next to the SQLite DB, on the same persistent /data volume --
    // see faa_aircraft_registry.hpp for why this matters (avoids a fresh
    // 70MB download on every container restart).
    const char* faa_registry_dir_env = std::getenv("DB_PATH");
    std::string faa_registry_cache = faa_registry_dir_env
        ? (std::string(faa_registry_dir_env).substr(0, std::string(faa_registry_dir_env).find_last_of('/')) + "/faa_registry.zip")
        : "/data/faa_registry.zip";
    FaaAircraftRegistry faa_registry(faa_registry_cache);
    remoteid::ManufacturerRegistry manufacturer_registry;
    manufacturer_registry.load(); // manufacturer_codes.json next to the binary, if present; fine if absent
    std::cout << "Remote ID manufacturer-code table: " << manufacturer_registry.loaded_count()
              << " entries loaded (see manufacturer_registry.hpp for how to populate it).\n";

    // In-memory learned baseline for statistical anomaly detection -- see
    // analytics.hpp for the honest scope (online mean/variance, not a
    // trained model file). Resets on restart by design.
    analytics::LearnedBaselineModel baseline_model;

    track_manager.set_incident_callback([&](const Incident& inc) { storage.log_incident(inc); });
    track_manager.set_detection_callback([&](const Detection& det, const Track& track) {
        storage.log_detection(det, track.track_id);
        if (track.alt_m.has_value()) {
            baseline_model.observe(track.classification, fusion::estimate_speed_mps(track), *track.alt_m);
        }
    });

    // Background: track expiry sweep every 5s
    std::thread sweeper([&] {
        while (true) { track_manager.sweep(); std::this_thread::sleep_for(std::chrono::seconds(5)); }
    });
    sweeper.detach();

    opensky.start();
    std::cout << "Global live aircraft feed (OpenSky Network) polling every "
              << OPENSKY_POLL_SECONDS << "s worldwide...\n";

    for (auto* feed : { &adsblol, &airplaneslive }) {
        if (feed->enabled()) {
            feed->start();
            std::cout << "Community ADS-B feed (" << feed->source_name() << ") polling " << feed->region_count()
                      << " region(s) every " << COMMUNITY_ADSB_POLL_SECONDS << "s as a redundant aircraft source...\n";
        } else {
            std::string prefix = feed->source_name() == "adsblol" ? "ADSBLOL" : "AIRPLANESLIVE";
            std::cout << "Community ADS-B feed (" << feed->source_name() << ") disabled -- set " << prefix
                      << "_LAT/" << prefix << "_LON (or " << prefix << "_REGIONS, or " << prefix
                      << "_REGIONS=CONUS/NORTH_AMERICA) to enable.\n";
        }
    }

    // Shared by the /api/aircraft handler and the snapshot thread below,
    // so there's exactly one place that knows how to merge the sources.
    // OpenSky wins on conflict (longest-standing, globally-scoped source),
    // then adsb.lol, then airplanes.live -- order only matters for which
    // copy of a duplicate sighting is kept, not for coverage.
    auto get_merged_aircraft = [&]() {
        json openskyResp = json::parse(opensky.get_aircraft_json());
        auto adsblolSnap = adsblol.get_snapshot();
        auto airplanesliveSnap = airplaneslive.get_snapshot();
        std::map<std::string, json> merged;
        for (auto& ac : openskyResp.value("aircraft", json::array())) {
            std::string icao = ac.value("icao24", std::string());
            if (!icao.empty()) merged[icao] = ac;
        }
        for (auto& ac : adsblolSnap.aircraft) {
            std::string icao = ac.value("icao24", std::string());
            if (!icao.empty() && !merged.count(icao)) merged[icao] = ac;
        }
        for (auto& ac : airplanesliveSnap.aircraft) {
            std::string icao = ac.value("icao24", std::string());
            if (!icao.empty() && !merged.count(icao)) merged[icao] = ac;
        }
        json arr = json::array();
        for (auto& [icao, ac] : merged) arr.push_back(ac);
        return arr;
    };

    // Aircraft data otherwise only ever lives in memory -- without this,
    // /api/history/window would have nothing to show for general
    // aviation traffic, only for tracks that went through the drone
    // detection pipeline. Snapshot cadence and retention are both
    // deliberately coarse (a snapshot can be hundreds-to-thousands of
    // aircraft; logging every ~12s poll would make this table the
    // dominant thing growing the database) -- tune via
    // AIRCRAFT_SNAPSHOT_INTERVAL_SECONDS / AIRCRAFT_HISTORY_RETENTION_DAYS
    // if the defaults don't fit your deployment.
    double snapshot_interval = 300.0, retention_days = 30.0;
    if (const char* v = std::getenv("AIRCRAFT_SNAPSHOT_INTERVAL_SECONDS")) { try { snapshot_interval = std::stod(v); } catch (...) {} }
    if (const char* v = std::getenv("AIRCRAFT_HISTORY_RETENTION_DAYS")) { try { retention_days = std::stod(v); } catch (...) {} }
    std::thread aircraft_snapshot_thread([&, snapshot_interval, retention_days] {
        int ticks = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::duration<double>(snapshot_interval));
            storage.log_aircraft_snapshot(get_merged_aircraft(), now_seconds());
            if (++ticks % 100 == 0) storage.prune_aircraft_history(retention_days); // roughly once per ~100 snapshots
        }
    });
    aircraft_snapshot_thread.detach();
    std::cout << "Aircraft history snapshots every " << snapshot_interval << "s, retained "
              << retention_days << " days.\n";

    // start() already runs refresh() immediately in its own background
    // thread (see AirspaceFeed::start()/AirportsFeed::start()) before its
    // first sleep -- so it was never necessary to *also* call refresh()
    // synchronously here first. That redundant synchronous call was two
    // slow network fetches (curl timeouts of 20s and 60s respectively)
    // blocking server.start() below from ever being reached until they
    // finished, which on a fully air-gapped box (an explicit design goal:
    // "local processing and air-gapped capable") meant the HTTP server
    // wouldn't bind and start serving for up to ~80s. Just start() alone
    // gets the same "fetch immediately" behavior without blocking.
    airspace.start();
    std::cout << "Restricted + controlled airspace (FAA + OpenAIP) refreshing every "
              << (AIRSPACE_REFRESH_SECONDS / 3600.0) << "h (initial fetch in background)...\n";

    airports.start();
    std::cout << "Worldwide airport directory refreshing every "
              << (AIRPORTS_REFRESH_SECONDS / 3600.0) << "h (initial fetch in background)...\n";

    runways.start();
    std::cout << "Runway layouts (for airport outline drawing) refreshing every "
              << (RUNWAYS_REFRESH_SECONDS / 3600.0) << "h (initial fetch in background)...\n";

    faa_registry.start();
    std::cout << "FAA aircraft registry (N-numbers) refreshing every "
              << (FAA_REGISTRY_REFRESH_SECONDS / 3600.0 / 24.0) << " day(s), cached at "
              << faa_registry_cache << " (~70MB download on a cold cache, background)...\n";

    if (simulate_mode) {
        std::thread sim_thread([&] {
            while (true) simulate::run(track_manager, 180.0, sim_speed);
        });
        sim_thread.detach();
        std::cout << "Synthetic drone-sensor demo feed running in background...\n";
    }

    HttpServer server;
    server.serve_static_dir("/static", "public");

    server.get(R"(/)", [](const HttpRequest&) {
        std::ifstream f("public/index.html", std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf();
        return HttpResponse{200, ss.str(), "text/html; charset=utf-8"};
    });

    server.get(R"(/api/health)", [&](const HttpRequest&) {
        json j{{"status", "ok"}, {"active_tracks", (int)track_manager.get_active_tracks().size()}};
        return HttpResponse::json_ok(j.dump());
    });

    server.get(R"(/api/tracks)", [&](const HttpRequest&) {
        json arr = json::array();
        for (auto& t : track_manager.get_active_tracks()) arr.push_back(t.to_json());
        return HttpResponse::json_ok(arr.dump());
    });

    server.get(R"(/api/tracks/(\d+))", [&](const HttpRequest& req) {
        int64_t id = std::stoll(req.path_params[0]);
        auto t = track_manager.get_track(id);
        if (!t.has_value()) return HttpResponse::not_found();
        return HttpResponse::json_ok(t->to_json().dump());
    });

    // Local trajectory + anomaly analysis for a live track -- see
    // analytics.hpp for what each field means and its scope.
    server.get(R"(/api/tracks/(\d+)/trajectory)", [&](const HttpRequest& req) {
        int64_t id = std::stoll(req.path_params[0]);
        auto t = track_manager.get_track(id);
        if (!t.has_value()) return HttpResponse::not_found();

        auto traj = analytics::compute_trajectory(t->history);
        auto anomalies = analytics::detect_anomalies(*t, storage.geofence_incident_count(id));
        auto pattern = analytics::analyze_flight_pattern(*t, traj);
        double speed_now = fusion::estimate_speed_mps(*t);
        double speed_z = baseline_model.speed_zscore(t->classification, speed_now);
        double alt_z = t->alt_m.has_value() ? baseline_model.altitude_zscore(t->classification, *t->alt_m) : 0.0;

        json j;
        j["track_id"] = id;
        j["trajectory"] = {
            {"total_distance_m", traj.total_distance_m}, {"duration_s", traj.duration_s},
            {"avg_speed_mps", traj.avg_speed_mps}, {"max_speed_mps", traj.max_speed_mps},
            {"min_alt_m", traj.min_alt_m}, {"max_alt_m", traj.max_alt_m}, {"avg_alt_m", traj.avg_alt_m},
            {"total_heading_change_deg", traj.total_heading_change_deg},
            {"loiter_detected", traj.loiter_detected}, {"loiter_radius_m", traj.loiter_radius_m},
        };
        j["anomalies"] = {
            {"rapid_altitude_change", anomalies.rapid_altitude_change},
            {"rapid_speed_change", anomalies.rapid_speed_change},
            {"loitering", anomalies.loitering},
            {"repeated_zone_incursion", anomalies.repeated_zone_incursion},
            {"statistical_outlier", std::fabs(speed_z) > 2.5 || std::fabs(alt_z) > 2.5},
            {"speed_zscore", speed_z}, {"altitude_zscore", alt_z},
            {"notes", anomalies.notes},
        };
        // Heuristic, not a trained/validated classifier -- see the scope
        // note on analyze_flight_pattern() in analytics.hpp.
        j["flight_pattern"] = {
            {"bird_likelihood", pattern.bird_likelihood},
            {"notes", pattern.notes},
        };
        return HttpResponse::json_ok(j.dump());
    });

    // Full flight-path reconstruction from the permanent SQLite log --
    // works even for a track that's since been dropped/expired, unlike
    // the in-memory Track.history (capped at 50 detections).
    server.get(R"(/api/tracks/(\d+)/full_history)", [&](const HttpRequest& req) {
        int64_t id = std::stoll(req.path_params[0]);
        return HttpResponse::json_ok(storage.detections_for_track(id).dump());
    });

    // Historical replay with time controls: every track's position
    // sequence within [start, end], for the frontend's History tab to
    // scrub through. start/end are Unix epoch seconds; defaults to the
    // last hour if omitted. Includes aircraft position history
    // (aircraft_history table, populated by the snapshot thread above) --
    // note its time resolution is only as fine as
    // AIRCRAFT_SNAPSHOT_INTERVAL_SECONDS (default 5 minutes), much
    // coarser than drone tracks' per-detection resolution.
    server.get(R"(/api/history/window)", [&](const HttpRequest& req) {
        double now = now_seconds();
        double start = req.query.count("start") ? std::stod(req.query.at("start")) : now - 3600.0;
        double end = req.query.count("end") ? std::stod(req.query.at("end")) : now;
        if (end <= start) return HttpResponse::bad_request("end must be after start");
        json resp = storage.history_window(start, end);
        resp["aircraft"] = storage.aircraft_history_window(start, end);
        return HttpResponse::json_ok(resp.dump());
    });

    server.get(R"(/api/incidents)", [&](const HttpRequest& req) {
        int limit = req.query.count("limit") ? std::stoi(req.query.at("limit")) : 100;
        return HttpResponse::json_ok(storage.recent_incidents(limit).dump());
    });

    // Exportable incident report -- CSV so it opens directly in a
    // spreadsheet for after-the-fact review, rather than only being
    // available as JSON meant for programmatic consumption.
    server.get(R"(/api/incidents/export)", [&](const HttpRequest& req) {
        std::string format = req.query.count("format") ? req.query.at("format") : "csv";
        if (format != "csv") return HttpResponse::bad_request("format must be \"csv\"");
        int limit = req.query.count("limit") ? std::stoi(req.query.at("limit")) : 1000;

        auto csv_escape = [](const std::string& s) {
            if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
            std::string out = "\"";
            for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
            out += "\"";
            return out;
        };
        auto str_or_empty = [](const json& j) { return j.is_null() ? std::string() : j.get<std::string>(); };

        std::ostringstream csv;
        csv << "incident_id,track_id,classification,confidence,lat,lon,reason,timestamp,"
               "incident_type,zone_id,zone_name,sensor_hit_counts,evidence\n";
        for (auto& inc : storage.recent_incidents(limit)) {
            csv << inc.value("incident_id", (int64_t)0) << ","
                << inc.value("track_id", (int64_t)0) << ","
                << csv_escape(inc.value("classification", std::string())) << ","
                << inc.value("confidence", 0.0) << ","
                << inc.value("lat", 0.0) << ","
                << inc.value("lon", 0.0) << ","
                << csv_escape(inc.value("reason", std::string())) << ","
                << inc.value("timestamp", 0.0) << ","
                << csv_escape(inc.value("incident_type", std::string())) << ","
                << csv_escape(str_or_empty(inc["zone_id"])) << ","
                << csv_escape(str_or_empty(inc["zone_name"])) << ","
                << csv_escape(inc.value("sensor_hit_counts", std::string())) << ","
                << csv_escape(inc["evidence"].dump()) << "\n";
        }

        HttpResponse resp;
        resp.status = 200;
        resp.body = csv.str();
        resp.content_type = "text/csv";
        resp.extra_headers["Content-Disposition"] = "attachment; filename=\"incidents_export.csv\"";
        return resp;
    });

    // Operator feedback on a past alert -- the ground-truth data needed to
    // manually tune fusion/classification thresholds over time. This does
    // NOT feed back into the classifier automatically (it's a rule-based
    // heuristic system, not a self-retraining model).
    server.post(R"(/api/incidents/(\d+)/feedback)", [&](const HttpRequest& req) {
        if (auto denied = check_api_key(req, detections_api_key)) return *denied;
        try {
            int64_t incident_id = std::stoll(req.path_params[0]);
            auto body = json::parse(req.body);
            bool correct = body.at("correct").get<bool>();
            std::optional<std::string> actual_classification;
            if (body.contains("actual_classification") && !body["actual_classification"].is_null())
                actual_classification = body["actual_classification"].get<std::string>();
            std::optional<std::string> note;
            if (body.contains("note") && !body["note"].is_null())
                note = body["note"].get<std::string>();

            if (!storage.log_feedback(incident_id, correct, actual_classification, note)) return HttpResponse::not_found();
            return HttpResponse{201, R"({"status":"recorded"})", "application/json"};
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("invalid feedback payload: ") + e.what());
        }
    });

    server.get(R"(/api/incidents/accuracy)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(storage.accuracy_summary().dump());
    });

    // Scans all currently active tracks and returns only those with at
    // least one anomaly flag set -- a "what needs attention right now" view.
    server.get(R"(/api/analytics/anomalies)", [&](const HttpRequest&) {
        json out = json::array();
        for (auto& t : track_manager.get_active_tracks()) {
            auto anomalies = analytics::detect_anomalies(t, storage.geofence_incident_count(t.track_id));
            double speed_now = fusion::estimate_speed_mps(t);
            double speed_z = baseline_model.speed_zscore(t.classification, speed_now);
            double alt_z = t.alt_m.has_value() ? baseline_model.altitude_zscore(t.classification, *t.alt_m) : 0.0;
            bool stat_outlier = std::fabs(speed_z) > 2.5 || std::fabs(alt_z) > 2.5;
            if (!anomalies.rapid_altitude_change && !anomalies.rapid_speed_change && !anomalies.loitering &&
                !anomalies.repeated_zone_incursion && !stat_outlier) continue;

            json j = t.to_json();
            j["anomalies"] = {
                {"rapid_altitude_change", anomalies.rapid_altitude_change},
                {"rapid_speed_change", anomalies.rapid_speed_change},
                {"loitering", anomalies.loitering},
                {"repeated_zone_incursion", anomalies.repeated_zone_incursion},
                {"statistical_outlier", stat_outlier},
                {"speed_zscore", speed_z}, {"altitude_zscore", alt_z},
                {"notes", anomalies.notes},
            };
            out.push_back(j);
        }
        return HttpResponse::json_ok(out.dump());
    });

    server.get(R"(/api/analytics/links)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(storage.link_analysis().dump());
    });

    // Graph view of the same link data: nodes are tracks/identifiers/
    // sensors/zones, edges are the observed relationships between them.
    // GET /api/analytics/graph?node=track:5 (or identifier:..., sensor:...,
    // zone:...) returns just that node's direct neighbors -- "show me
    // everything connected to X" -- omit `node` for the whole graph.
    server.get(R"(/api/analytics/graph)", [&](const HttpRequest& req) {
        std::optional<std::string> focus;
        if (req.query.count("node") && !req.query.at("node").empty()) focus = req.query.at("node");
        return HttpResponse::json_ok(storage.link_graph(focus).dump());
    });

    server.get(R"(/api/analytics/stats)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(storage.stats_summary().dump());
    });

    // Transparency into the learned baseline used for statistical_outlier
    // above -- per-classification mean/stddev and sample count.
    server.get(R"(/api/analytics/model)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(baseline_model.state().dump());
    });

    server.get(R"(/api/sensors)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(sensor_health.all_statuses().dump());
    });

    server.get(R"(/api/zones)", [&](const HttpRequest&) {
        json arr = json::array();
        for (auto& z : load_zones()) {
            json ring = json::array();
            for (auto& [la, lo] : z.polygon) ring.push_back({la, lo});
            arr.push_back({{"zone_id", z.zone_id}, {"name", z.name}, {"polygon", ring}});
        }
        return HttpResponse::json_ok(arr.dump());
    });

    // Global live aircraft, merged from every enabled source. OpenSky
    // (global, no bounding box) is primary; the community feeds
    // (point+radius, if configured) fill in when OpenSky is empty/rate-
    // limited or simply add redundant coverage for their configured area.
    // Deduped by icao24 -- see get_merged_aircraft's comment for
    // conflict-resolution order.
    server.get(R"(/api/aircraft)", [&](const HttpRequest&) {
        json openskyResp = json::parse(opensky.get_aircraft_json());
        auto adsblolSnap = adsblol.get_snapshot();
        auto airplanesliveSnap = airplaneslive.get_snapshot();
        json arr = get_merged_aircraft();

        std::optional<double> opensky_last_poll = openskyResp.value("last_poll", json(nullptr)).is_null()
            ? std::optional<double>() : openskyResp["last_poll"].get<double>();
        std::optional<double> combined_last_poll = opensky_last_poll;
        for (auto* snap : { &adsblolSnap, &airplanesliveSnap }) {
            if (snap->last_poll.has_value() &&
                (!combined_last_poll.has_value() || *snap->last_poll > *combined_last_poll)) {
                combined_last_poll = snap->last_poll;
            }
        }

        json resp;
        resp["aircraft"] = arr;
        resp["count"] = arr.size();
        resp["last_poll"] = combined_last_poll.has_value() ? json(*combined_last_poll) : json(nullptr);
        resp["authenticated"] = openskyResp.value("authenticated", false);
        resp["error"] = openskyResp.value("error", json(nullptr)); // primary source's error, for backward compat
        resp["sources"] = {
            {"opensky", {
                {"count", openskyResp.value("aircraft", json::array()).size()},
                {"last_poll", openskyResp.value("last_poll", json(nullptr))},
                {"error", openskyResp.value("error", json(nullptr))},
                {"authenticated", openskyResp.value("authenticated", false)},
            }},
            {"adsblol", {
                {"enabled", adsblol.enabled()},
                {"count", adsblolSnap.aircraft.size()},
                {"last_poll", adsblolSnap.last_poll.has_value() ? json(*adsblolSnap.last_poll) : json(nullptr)},
                {"error", adsblolSnap.error.has_value() ? json(*adsblolSnap.error) : json(nullptr)},
            }},
            {"airplaneslive", {
                {"enabled", airplaneslive.enabled()},
                {"count", airplanesliveSnap.aircraft.size()},
                {"last_poll", airplanesliveSnap.last_poll.has_value() ? json(*airplanesliveSnap.last_poll) : json(nullptr)},
                {"error", airplanesliveSnap.error.has_value() ? json(*airplanesliveSnap.error) : json(nullptr)},
            }},
        };
        return HttpResponse::json_ok(resp.dump());
    });

    // Restricted + controlled airspace (FAA + OpenAIP)
    server.get(R"(/api/airspace)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(airspace.get_airspace_json());
    });

    // Worldwide airports/heliports directory (OurAirports)
    server.get(R"(/api/airports)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(airports.get_airports_json());
    });

    // Runway layouts (OurAirports) -- lets the frontend draw a selected
    // airport's actual outline instead of just a point marker.
    server.get(R"(/api/runways)", [&](const HttpRequest&) {
        return HttpResponse::json_ok(runways.get_runways_json());
    });

    // Airport property boundary (OpenStreetMap Overpass, on-demand -- see
    // airport_boundary.hpp for why this isn't a bulk feed like the two
    // above). ident is required (cache key); icao is optional but greatly
    // improves match accuracy when the airport has one on file.
    server.get(R"(/api/airport_boundary)", [&](const HttpRequest& req) {
        if (!req.query.count("ident") || !req.query.count("lat") || !req.query.count("lon")) {
            return HttpResponse::bad_request("provide ident, lat, and lon query parameters");
        }
        std::string ident = req.query.at("ident");
        std::string icao = req.query.count("icao") ? req.query.at("icao") : "";
        double lat, lon;
        try {
            lat = std::stod(req.query.at("lat"));
            lon = std::stod(req.query.at("lon"));
        } catch (...) {
            return HttpResponse::bad_request("lat/lon must be numeric");
        }
        json result = airport_boundary.lookup(ident, icao, lat, lon);
        return HttpResponse::json_ok(result.dump());
    });

    // Registry lookup tab: FAA N-number registry (US aircraft ownership/
    // make/model), ICAO Mode-S national-allocation guess, and ICAO type
    // designator expansion. See faa_aircraft_registry.hpp/
    // icao_allocation.hpp/type_designators.hpp for the honest scope of
    // each (none of these are exhaustive/authoritative databases).
    server.get(R"(/api/registry/status)", [&](const HttpRequest&) {
        json j = faa_registry.status_json();
        j["icao_allocation_table_size"] = icao_allocation_table().size();
        j["type_designator_table_size"] = type_designator_table().size();
        return HttpResponse::json_ok(j.dump());
    });

    server.get(R"(/api/registry/aircraft)", [&](const HttpRequest& req) {
        FaaAircraftRegistry::LookupResult result;
        std::string queried_icao24; // tracked separately so the allocation guess still runs even on an N-number lookup that has no Mode S hex on file
        if (req.query.count("n_number") && !req.query.at("n_number").empty()) {
            result = faa_registry.lookup_n_number(req.query.at("n_number"));
            if (result.found) queried_icao24 = result.record.icao24;
        } else if (req.query.count("icao24") && !req.query.at("icao24").empty()) {
            queried_icao24 = req.query.at("icao24");
            result = faa_registry.lookup_icao24(queried_icao24);
        } else {
            return HttpResponse::bad_request("provide n_number or icao24 query parameter");
        }

        json resp;
        resp["found"] = result.found;
        resp["faa_registry"] = result.found ? FaaAircraftRegistry::record_to_json(result.record) : json(nullptr);
        const char* country = queried_icao24.empty() ? nullptr : icao_allocation_country(queried_icao24);
        resp["icao_allocation_country_guess"] = country ? json(country) : json(nullptr);
        return HttpResponse::json_ok(resp.dump());
    });

    server.get(R"(/api/registry/type)", [&](const HttpRequest& req) {
        if (!req.query.count("code") || req.query.at("code").empty()) {
            return HttpResponse::bad_request("provide code query parameter");
        }
        auto entry = lookup_type_designator(req.query.at("code"));
        json resp;
        resp["found"] = entry.has_value();
        if (entry.has_value()) {
            resp["manufacturer"] = entry->manufacturer;
            resp["model"] = entry->model;
            resp["is_helicopter"] = entry->is_helicopter;
        }
        return HttpResponse::json_ok(resp.dump());
    });

    server.post(R"(/api/detections)", [&](const HttpRequest& req) {
        if (auto denied = check_api_key(req, detections_api_key)) return *denied;
        try {
            auto body = json::parse(req.body);
            Detection det;
            det.sensor_type = body.at("sensor_type").get<std::string>();
            det.sensor_id = body.at("sensor_id").get<std::string>();
            if (!is_safe_identifier(det.sensor_type) || !is_safe_identifier(det.sensor_id)) {
                return HttpResponse::bad_request("sensor_type/sensor_id must be alphanumeric (plus _-.), max 64 chars");
            }
            det.lat = body.at("lat").get<double>();
            det.lon = body.at("lon").get<double>();
            if (body.contains("alt_m") && !body["alt_m"].is_null()) det.alt_m = body["alt_m"].get<double>();
            det.sensor_confidence = body.value("sensor_confidence", 1.0);
            det.metadata = body.value("metadata", json::object());
            det.timestamp = now_seconds();

            auto track = track_manager.ingest_detection(det);
            json resp{{"track", track.to_json()}};
            return HttpResponse{201, resp.dump(), "application/json"};
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("invalid detection payload: ") + e.what());
        }
    });

    // Decodes a raw ASTM F3411/OpenDroneID message-pack payload (hex-encoded
    // bytes, already extracted from BLE/Wi-Fi framing by the caller -- see
    // remote_id_decoder.hpp for that scope boundary). If a Location/Vector
    // message is present (or the caller supplies lat/lon directly), the
    // result is ingested as a remote_id Detection through the normal
    // fusion/geofence pipeline.
    server.post(R"(/api/remoteid/decode)", [&](const HttpRequest& req) {
        if (auto denied = check_api_key(req, detections_api_key)) return *denied;
        try {
            auto body = json::parse(req.body);
            auto bytes = remoteid::hex_decode(body.at("payload_hex").get<std::string>());
            if (bytes.empty()) return HttpResponse::bad_request("payload_hex is empty or not valid hex");

            auto decoded = remoteid::decode_payload(bytes);
            if (!decoded.ok) return HttpResponse::bad_request(decoded.error);

            json resp;
            resp["messages"] = decoded.messages;

            std::optional<double> loc_lat, loc_lon, loc_alt;
            for (auto& m : decoded.messages) {
                if (m.value("type", "") == "basic_id" && m.value("id_type", "") == "serial_number") {
                    resp["manufacturer_lookup"] = manufacturer_registry.lookup(m.value("uas_id", ""));
                }
                if (m.value("type", "") == "basic_id" && m.value("id_type", "") == "caa_registration_id") {
                    resp["registration_lookup"] = remoteid::ManufacturerRegistry::registration_id_note();
                }
                if (m.value("type", "") == "location_vector") {
                    if (m.contains("lat") && !m["lat"].is_null()) loc_lat = m["lat"].get<double>();
                    if (m.contains("lon") && !m["lon"].is_null()) loc_lon = m["lon"].get<double>();
                    if (m.contains("geodetic_altitude_m") && !m["geodetic_altitude_m"].is_null()) loc_alt = m["geodetic_altitude_m"].get<double>();
                }
            }
            // An explicit lat/lon in the request overrides the decoded
            // Location message -- e.g. if the caller wants their receiver's
            // own position tracked instead of the drone's self-reported one.
            if (body.contains("lat") && !body["lat"].is_null()) loc_lat = body["lat"].get<double>();
            if (body.contains("lon") && !body["lon"].is_null()) loc_lon = body["lon"].get<double>();

            if (loc_lat.has_value() && loc_lon.has_value()) {
                Detection det;
                det.sensor_type = "remote_id";
                det.sensor_id = body.value("sensor_id", std::string("remoteid-import"));
                if (!is_safe_identifier(det.sensor_id)) return HttpResponse::bad_request("sensor_id must be alphanumeric (plus _-.), max 64 chars");
                det.lat = *loc_lat;
                det.lon = *loc_lon;
                det.alt_m = loc_alt;
                det.timestamp = now_seconds();
                det.metadata = decoded.messages;
                auto track = track_manager.ingest_detection(det);
                resp["track"] = track.to_json();
            } else {
                resp["track"] = nullptr;
                resp["note"] = "no position available (no Location/Vector message decoded and no lat/lon supplied) -- decoded fields only, no track created";
            }
            return HttpResponse{201, resp.dump(), "application/json"};
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("invalid remote id decode request: ") + e.what());
        }
    });

    server.get(R"(/api/remoteid/manufacturers)", [&](const HttpRequest&) {
        json j{{"count", manufacturer_registry.loaded_count()}, {"codes", manufacturer_registry.list()}};
        return HttpResponse::json_ok(j.dump());
    });

    // Heuristic RF signature analysis over already-computed spectrum data
    // (freq_hz/power_dbm samples) -- see rf_spectrum.hpp for what this
    // does and doesn't do. "sweeps" (plural, optional) enables the
    // channel-hopping check across multiple time-ordered sweeps.
    server.post(R"(/api/rf/analyze)", [&](const HttpRequest& req) {
        if (auto denied = check_api_key(req, detections_api_key)) return *denied;
        try {
            auto body = json::parse(req.body);
            auto to_samples = [](const json& arr) {
                std::vector<rf::Sample> samples;
                for (auto& s : arr) samples.push_back({s.at("freq_hz").get<double>(), s.at("power_dbm").get<double>()});
                std::sort(samples.begin(), samples.end(), [](const rf::Sample& a, const rf::Sample& b) { return a.freq_hz < b.freq_hz; });
                return samples;
            };

            auto samples = to_samples(body.at("samples"));
            json resp = rf::analyze(samples);

            if (body.contains("sweeps") && body["sweeps"].is_array()) {
                std::vector<std::vector<rf::Sample>> sweeps;
                for (auto& sweep_j : body["sweeps"]) sweeps.push_back(to_samples(sweep_j));
                resp["hopping_analysis"] = rf::detect_hopping(sweeps);
            }

            double confidence = resp.value("overall_confidence", 0.0);
            bool has_pos = body.contains("lat") && body.contains("lon") && !body["lat"].is_null() && !body["lon"].is_null();
            if (confidence >= 0.3 && has_pos) {
                Detection det;
                det.sensor_type = "rf";
                det.sensor_id = body.value("sensor_id", std::string("rf-spectrum-import"));
                if (!is_safe_identifier(det.sensor_id)) return HttpResponse::bad_request("sensor_id must be alphanumeric (plus _-.), max 64 chars");
                det.lat = body["lat"].get<double>();
                det.lon = body["lon"].get<double>();
                det.sensor_confidence = confidence;
                det.timestamp = now_seconds();
                det.metadata = resp;
                auto track = track_manager.ingest_detection(det);
                resp["track"] = track.to_json();
            } else {
                resp["track"] = nullptr;
                resp["note"] = has_pos ? "confidence below ingestion threshold (0.3) -- analysis only, no track created"
                                        : "no lat/lon supplied -- analysis only, no track created";
            }
            return HttpResponse{201, resp.dump(), "application/json"};
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("invalid rf analyze request: ") + e.what());
        }
    });

    // Generic importer for recorded telemetry (CSV or a JSON array of
    // records) -- see telemetry_import.hpp for the recognized field names
    // and the numeric-epoch-only timestamp scope note.
    server.post(R"(/api/telemetry/import)", [&](const HttpRequest& req) {
        if (auto denied = check_api_key(req, detections_api_key)) return *denied;
        try {
            auto body = json::parse(req.body);
            std::string sensor_id = body.value("sensor_id", std::string("telemetry-import"));
            std::string sensor_type = body.value("sensor_type", std::string("remote_id"));
            std::string format = body.value("format", std::string("json"));
            if (!is_safe_identifier(sensor_id) || !is_safe_identifier(sensor_type)) {
                return HttpResponse::bad_request("sensor_type/sensor_id must be alphanumeric (plus _-.), max 64 chars");
            }

            telemetry::ImportResult result;
            if (format == "csv") {
                result = telemetry::import_csv(body.at("data").get<std::string>(), sensor_id, sensor_type, track_manager);
            } else if (format == "json") {
                result = telemetry::import_json(body.at("data"), sensor_id, sensor_type, track_manager);
            } else {
                return HttpResponse::bad_request("format must be \"csv\" or \"json\"");
            }

            json ids = json::array();
            for (auto id : result.track_ids) ids.push_back(id);
            json resp{{"imported", result.imported}, {"skipped", result.skipped}, {"errors", result.errors}, {"track_ids", ids}};
            return HttpResponse{201, resp.dump(), "application/json"};
        } catch (const std::exception& e) {
            return HttpResponse::bad_request(std::string("invalid telemetry import request: ") + e.what());
        }
    });

    std::cout << "Data Flight server starting on port " << port << " ...\n";
    if (!server.start(port)) {
        std::cerr << "Failed to start server on port " << port << "\n";
        return 1;
    }
    std::cout << "Running. Open http://localhost:" << port << "/\n";

    // Block forever (server runs on its own accept thread)
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
