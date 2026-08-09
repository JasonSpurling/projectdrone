// community_adsb_feed.hpp -- community ADS-B feeds (adsb.lol, airplanes.live,
// ...) as redundant/failover aircraft sources alongside OpenSky. Both
// confirmed live against the same "/v2/point/{lat}/{lon}/{radius_nm}" API
// shape (api.adsb.lol and api.airplanes.live) -- free, unauthenticated,
// public. Radius is capped at 250nm by the services themselves.
//
// adsb.fi was also tried (it's the third major community aggregator in
// this family) but both api.adsb.fi and adsb.fi/api returned 403/404 from
// this host at the time this was written -- possibly bot-protection,
// possibly a moved/changed API. Not included since there's no confirmed
// working endpoint; revisit if that changes.
//
// ADS-B Exchange was also tried -- its public free API now returns
// HTTP 402 ("Please purchase a key") on every request. It's paid-only now,
// so it's excluded (this project only wires up genuinely free sources).
//
// Honest scope: unlike OpenSky's global feed, this is a point+radius query
// -- a single query can't cover a continent, so *_REGIONS supports
// multiple query points per feed, polled and merged (deduped by icao24)
// into one snapshot. The CONUS/NORTH_AMERICA presets are centered on major
// metro areas rather than an exhaustive geographic grid -- air traffic
// concentrates near population centers/airports, and a full tiling grid
// (35-40+ circles) would be a lot of continuous load on a free community
// service for coverage of mostly-empty airspace. This is broad, not
// literally 100% blanket, coverage. Each feed is disabled unless its
// *_LAT/*_LON or *_REGIONS env var is set -- no default center point is
// baked in otherwise (this project already removed a hardcoded-DC demo
// once; not repeating that as a silent default query location).
#pragma once
#include <curl/curl.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>
#include <atomic>
#include <cstdlib>
#include <sstream>
#include "models.hpp"

// Rest between full sweeps -- see the per-region stagger in poll_once() for
// the actual burst-limit fix; this is just cooldown between cycles, not
// part of what avoids the rate limit. Kept short deliberately: with the
// NORTH_AMERICA preset (17 regions), a full sweep alone already takes
// ~25-30s (16 * 1.5s stagger + per-region request time), so this rest is
// additive on top of that. Confirmed live that at the previous 25s rest,
// a single feed's real per-aircraft data was only refreshing every
// 60-95s -- well past the frontend's dead-reckoning cap, so aircraft were
// visibly freezing mid-cycle instead of continuing to move. Shortened to
// keep the full cycle closer to that cap.
constexpr double COMMUNITY_ADSB_POLL_SECONDS = 10.0;
constexpr double COMMUNITY_ADSB_DEFAULT_RADIUS_NM = 250.0; // service-enforced max

// These feeds' "category" field is the raw ADS-B ES emitter category code
// (e.g. "A3", "B6") rather than OpenSky's single 0-20 integer -- this maps
// the standard DO-260B emitter category table (Set A = airborne, Set B =
// airborne cont'd, Set C = surface) onto the same 0-20 scale used
// elsewhere in this codebase (see opensky_category_name in
// opensky_feed.hpp) so the frontend/fusion code only has to know one
// encoding. Best-effort against the public spec, not vendor-verified.
inline int community_adsb_category_to_numeric(const std::string& cat) {
    static const std::map<std::string, int> table = {
        {"A0", 1}, {"A1", 2}, {"A2", 3}, {"A3", 4}, {"A4", 5}, {"A5", 6}, {"A6", 7}, {"A7", 8},
        {"B0", 1}, {"B1", 9}, {"B2", 10}, {"B3", 11}, {"B4", 12}, {"B5", 13}, {"B6", 14}, {"B7", 15},
        {"C0", 1}, {"C1", 16}, {"C2", 17}, {"C3", 18}, {"C4", 19}, {"C5", 20},
    };
    auto it = table.find(cat);
    return it != table.end() ? it->second : 0;
}

struct CommunityAdsbRegion { double lat, lon, radius_nm; };

// 10 major CONUS metro areas -- not a precise tiling, see file header.
inline std::vector<CommunityAdsbRegion> community_adsb_conus_preset(double radius_nm) {
    return {
        {47.6, -122.3, radius_nm},  // Seattle
        {34.0, -118.2, radius_nm},  // Los Angeles
        {39.7, -104.9, radius_nm},  // Denver
        {32.8, -96.8, radius_nm},   // Dallas
        {41.8, -87.6, radius_nm},   // Chicago
        {33.7, -84.4, radius_nm},   // Atlanta
        {40.7, -74.0, radius_nm},   // New York
        {25.8, -80.2, radius_nm},   // Miami
        {44.9, -93.3, radius_nm},   // Minneapolis
        {33.4, -112.0, radius_nm},  // Phoenix
    };
}

// CONUS preset plus major Canadian and Mexican metro areas, for
// *_REGIONS=NORTH_AMERICA -- "North America" is a wider ask than the
// US-only coverage CONUS gives (this project already had the "only
// covered DC" gap once; not repeating a too-narrow default for a broader
// request). Same "population centers, not a tiling grid" approach as
// CONUS -- see file header.
inline std::vector<CommunityAdsbRegion> community_adsb_north_america_preset(double radius_nm) {
    auto regions = community_adsb_conus_preset(radius_nm);
    std::vector<CommunityAdsbRegion> extra = {
        {43.7, -79.4, radius_nm},   // Toronto
        {45.5, -73.6, radius_nm},   // Montreal
        {49.3, -123.1, radius_nm},  // Vancouver
        {51.0, -114.1, radius_nm},  // Calgary
        {19.4, -99.1, radius_nm},   // Mexico City
        {25.7, -100.3, radius_nm},  // Monterrey
        {20.7, -103.4, radius_nm},  // Guadalajara
    };
    regions.insert(regions.end(), extra.begin(), extra.end());
    return regions;
}

// One instance per community feed (adsb.lol, airplanes.live, ...) --
// parameterized by env var prefix, API base URL, and the "source" tag
// stamped onto each aircraft it returns, since they're otherwise
// identical in shape/behavior.
class CommunityAdsbFeed {
public:
    CommunityAdsbFeed(std::string env_prefix, std::string base_url, std::string source_name)
        : env_prefix_(std::move(env_prefix)), base_url_(std::move(base_url)), source_name_(std::move(source_name)) {
        double radius = COMMUNITY_ADSB_DEFAULT_RADIUS_NM;
        if (const char* radius_env = std::getenv((env_prefix_ + "_RADIUS_NM").c_str())) {
            try { radius = std::stod(radius_env); } catch (...) {}
        }

        if (const char* regions_env = std::getenv((env_prefix_ + "_REGIONS").c_str())) {
            std::string val = regions_env;
            if (val == "CONUS") {
                regions_ = community_adsb_conus_preset(radius);
            } else if (val == "NORTH_AMERICA") {
                regions_ = community_adsb_north_america_preset(radius);
            } else {
                regions_ = parse_regions(val, radius);
            }
        } else {
            const char* lat_env = std::getenv((env_prefix_ + "_LAT").c_str());
            const char* lon_env = std::getenv((env_prefix_ + "_LON").c_str());
            if (lat_env && lon_env) {
                try { regions_.push_back({std::stod(lat_env), std::stod(lon_env), radius}); } catch (...) {}
            }
        }
        enabled_ = !regions_.empty();
    }

    const std::string& source_name() const { return source_name_; }
    bool enabled() const { return enabled_; }
    size_t region_count() const { return regions_.size(); }

    void start() {
        if (!enabled_) return;
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                poll_once();
                std::this_thread::sleep_for(std::chrono::duration<double>(COMMUNITY_ADSB_POLL_SECONDS));
            }
        });
        worker_.detach();
    }

    // Parsed aircraft array + metadata, for the caller (main.cpp) to merge
    // with OpenSky's -- unlike OpenSkyFeed this doesn't own the final
    // /api/aircraft response shape, since that's a merge of >=2 sources.
    struct Snapshot {
        json aircraft = json::array();
        std::optional<double> last_poll;
        std::optional<std::string> error;
    };

    Snapshot get_snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        return {aircraft_, last_poll_, last_error_};
    }

private:
    static std::vector<CommunityAdsbRegion> parse_regions(const std::string& val, double default_radius) {
        // "lat,lon;lat,lon;..." -- radius shared across all regions
        // (*_RADIUS_NM), since per-region radius isn't worth the extra
        // parsing complexity for what this is used for.
        std::vector<CommunityAdsbRegion> out;
        std::istringstream regions_stream(val);
        std::string pair;
        while (std::getline(regions_stream, pair, ';')) {
            std::istringstream ps(pair);
            std::string lat_s, lon_s;
            if (std::getline(ps, lat_s, ',') && std::getline(ps, lon_s, ',')) {
                try { out.push_back({std::stod(lat_s), std::stod(lon_s), default_radius}); } catch (...) {}
            }
        }
        return out;
    }

    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    // One region's fetch; returns nullopt on failure so the caller can
    // keep going with whatever other regions succeeded rather than
    // discarding the whole poll over one bad request.
    std::optional<json> fetch_region(const CommunityAdsbRegion& region, std::string& error_out) {
        std::string url = base_url_ + "/v2/point/" + std::to_string(region.lat) + "/" +
                           std::to_string(region.lon) + "/" + std::to_string(region.radius_nm);

        CURL* curl = curl_easy_init();
        if (!curl) { error_out = "curl_easy_init failed"; return std::nullopt; }
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        // A single stuck/slow region used to be able to add a full 15s to
        // the sweep (confirmed live: one timeout alone stretched a cycle
        // well past a minute) -- shorter timeout bounds the worst case per
        // region; these services normally answer in well under a second.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) { error_out = std::string("curl error: ") + curl_easy_strerror(res); return std::nullopt; }
        if (http_code != 200) { error_out = source_name_ + " returned HTTP " + std::to_string(http_code); return std::nullopt; }

        try {
            return json::parse(response);
        } catch (const std::exception& e) {
            error_out = std::string("JSON parse error: ") + e.what();
            return std::nullopt;
        }
    }

    json row_to_aircraft(const json& row) const {
        json ac;
        ac["icao24"] = row.value("hex", std::string());
        std::string flight = row.value("flight", std::string());
        while (!flight.empty() && flight.back() == ' ') flight.pop_back(); // trim fixed-width padding
        ac["callsign"] = flight;
        ac["origin_country"] = json(nullptr); // not provided by this feed
        ac["lat"] = row["lat"];
        ac["lon"] = row["lon"];
        // alt_baro/alt_geom are feet here (vs. OpenSky's meters) --
        // convert so downstream code only ever sees meters.
        auto ft_to_m = [](const json& v) -> json {
            if (!v.is_number()) return json(nullptr);
            return json(v.get<double>() * 0.3048);
        };
        ac["baro_altitude_m"] = row.contains("alt_baro") ? ft_to_m(row["alt_baro"]) : json(nullptr);
        ac["geo_altitude_m"] = row.contains("alt_geom") ? ft_to_m(row["alt_geom"]) : json(nullptr);
        ac["altitude_m"] = ac["geo_altitude_m"].is_null() ? ac["baro_altitude_m"] : ac["geo_altitude_m"];
        ac["on_ground"] = row.value("alt_baro", json(0)) == "ground";
        ac["velocity_mps"] = row.contains("gs") && row["gs"].is_number() ? json(row["gs"].get<double>() * 0.514444) : json(nullptr);
        ac["heading_deg"] = row.contains("track") ? row["track"] : json(nullptr);
        ac["vertical_rate_mps"] = row.contains("baro_rate") && row["baro_rate"].is_number()
            ? json(row["baro_rate"].get<double>() * 0.00508) : json(nullptr);
        ac["squawk"] = row.value("squawk", json(nullptr));
        ac["spi"] = row.value("alert", 0) != 0;

        std::string cat_str = row.value("category", std::string());
        int cat = cat_str.empty() ? 0 : community_adsb_category_to_numeric(cat_str);
        ac["category"] = cat;
        ac["category_known"] = cat != 0 && cat != 1;
        ac["is_helicopter"] = cat == 8;
        ac["is_uav"] = cat == 14;
        ac["source"] = source_name_;
        return ac;
    }

    void poll_once() {
        json merged = json::array();
        std::set<std::string> seen_icao;
        int ok_regions = 0;
        std::string last_err;

        bool first = true;
        for (auto& region : regions_) {
            // Firing all regions back-to-back with zero spacing was tripping
            // adsb.lol's per-IP burst limit (confirmed live: a single ad-hoc
            // request from this same host succeeded fine outside the
            // container, while the app's 10-in-a-row burst was getting ~half
            // its regions 429'd every cycle). Spacing them out keeps the
            // whole sweep under COMMUNITY_ADSB_POLL_SECONDS with room to
            // spare while staying under whatever short-window burst
            // threshold each service enforces.
            if (!first) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            first = false;

            std::string err;
            auto body = fetch_region(region, err);
            if (!body.has_value()) { last_err = err; continue; }
            ok_regions++;
            if (!body->contains("ac") || !(*body)["ac"].is_array()) continue;

            for (auto& row : (*body)["ac"]) {
                if (!row.contains("lat") || !row.contains("lon") || row["lat"].is_null() || row["lon"].is_null()) continue;
                std::string icao = row.value("hex", std::string());
                // Overlapping region circles can see the same aircraft
                // twice -- keep the first sighting only.
                if (!icao.empty() && seen_icao.count(icao)) continue;
                if (!icao.empty()) seen_icao.insert(icao);
                merged.push_back(row_to_aircraft(row));
            }
        }

        std::lock_guard<std::mutex> lock(mu_);
        last_poll_ = now_seconds();
        if (ok_regions == 0) {
            last_error_ = regions_.empty() ? std::string("no regions configured") : last_err;
            return; // keep serving whatever aircraft_ already had rather than blanking it
        }
        aircraft_ = merged;
        last_error_ = (ok_regions < static_cast<int>(regions_.size()))
            ? std::optional<std::string>(std::to_string(regions_.size() - ok_regions) + " of " +
                                          std::to_string(regions_.size()) + " region(s) failed this poll: " + last_err)
            : std::nullopt;
    }

    std::string env_prefix_, base_url_, source_name_;
    bool enabled_ = false;
    std::vector<CommunityAdsbRegion> regions_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    json aircraft_ = json::array();
    std::optional<double> last_poll_;
    std::optional<std::string> last_error_;
};
