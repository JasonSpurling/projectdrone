// opensky_feed.hpp -- OpenSky Network live global ADS-B feed.
//
// OpenSky is the best FREE option for this: a community-run receiver
// network with worldwide coverage. Flightradar24/FlightAware have larger
// networks but are paid commercial APIs; OpenSky's anonymous access is
// free (rate-limited) and its authenticated access (register a free
// account, OAuth2 client credentials) raises that limit substantially.
//
// This feed intentionally does NOT restrict to a bounding box -- it polls
// /api/states/all globally, matching "live data of all of them, all over
// the world".
#pragma once
#include <curl/curl.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>
#include <atomic>
#include <cstdlib>
#include "models.hpp"

constexpr double OPENSKY_POLL_SECONDS = 12.0; // global pull is a big payload; don't hammer it
// extended=1 requests the 18th state-vector field (ADS-B emitter category),
// which lets us tell real rotorcraft/UAVs/gliders apart from a guess.
constexpr const char* OPENSKY_STATES_URL = "https://opensky-network.org/api/states/all?extended=1";
constexpr const char* OPENSKY_TOKEN_URL =
    "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";

// ADS-B emitter category (state vector field 17, from the `extended=1` query).
inline const char* opensky_category_name(int cat) {
    switch (cat) {
        case 0: return "no_info";
        case 1: return "no_category_info";
        case 2: return "light";
        case 3: return "small";
        case 4: return "large";
        case 5: return "high_vortex_large";
        case 6: return "heavy";
        case 7: return "high_performance";
        case 8: return "rotorcraft";
        case 9: return "glider";
        case 10: return "lighter_than_air";
        case 11: return "parachutist";
        case 12: return "ultralight";
        case 13: return "reserved";
        case 14: return "uav";
        case 15: return "space_vehicle";
        case 16: return "emergency_vehicle";
        case 17: return "service_vehicle";
        case 18: return "point_obstacle";
        case 19: return "cluster_obstacle";
        case 20: return "line_obstacle";
        default: return "unknown";
    }
}

class OpenSkyFeed {
public:
    OpenSkyFeed() {
        const char* cid = std::getenv("OPENSKY_CLIENT_ID");
        const char* csec = std::getenv("OPENSKY_CLIENT_SECRET");
        if (cid && csec) { client_id_ = cid; client_secret_ = csec; }
    }

    void start() {
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                poll_once();
                std::this_thread::sleep_for(std::chrono::duration<double>(OPENSKY_POLL_SECONDS));
            }
        });
        worker_.detach();
    }

    std::string get_aircraft_json() {
        std::lock_guard<std::mutex> lock(mu_);
        return cached_json_;
    }

private:
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    std::optional<std::string> get_token() {
        if (client_id_.empty()) return std::nullopt;
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        std::string response;
        std::string postfields = "grant_type=client_credentials&client_id=" + client_id_ +
                                  "&client_secret=" + client_secret_;
        curl_easy_setopt(curl, CURLOPT_URL, OPENSKY_TOKEN_URL);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) return std::nullopt;
        try {
            auto j = json::parse(response);
            if (j.contains("access_token")) return j["access_token"].get<std::string>();
        } catch (...) {}
        return std::nullopt;
    }

    void poll_once() {
        auto token = get_token();

        CURL* curl = curl_easy_init();
        if (!curl) return;
        std::string response;
        struct curl_slist* headers = nullptr;
        if (token.has_value()) {
            std::string auth = "Authorization: Bearer " + *token;
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_URL, OPENSKY_STATES_URL);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        std::lock_guard<std::mutex> lock(mu_);
        last_poll_ = now_seconds();

        if (res != CURLE_OK) {
            last_error_ = std::string("curl error: ") + curl_easy_strerror(res);
            rebuild_cache();
            return;
        }
        if (http_code != 200) {
            last_error_ = "OpenSky returned HTTP " + std::to_string(http_code);
            rebuild_cache();
            return;
        }

        try {
            auto j = json::parse(response);
            json new_aircraft = json::array();
            if (j.contains("states") && j["states"].is_array()) {
                for (auto& row : j["states"]) {
                    // OpenSky state vector column order (extended=1 adds #17):
                    // 0 icao24, 1 callsign, 2 origin_country, 3 time_position,
                    // 4 last_contact, 5 lon, 6 lat, 7 baro_altitude, 8 on_ground,
                    // 9 velocity, 10 true_track, 11 vertical_rate, 12 sensors,
                    // 13 geo_altitude, 14 squawk, 15 spi, 16 position_source, 17 category
                    if (row.size() < 14) continue;
                    if (row[6].is_null() || row[5].is_null()) continue; // no position fix
                    auto field = [&](size_t i) -> json { return (i < row.size() && !row[i].is_null()) ? row[i] : json(nullptr); };

                    json ac;
                    ac["icao24"] = row[0].is_null() ? "" : row[0];
                    ac["callsign"] = row[1].is_null() ? "" : row[1];
                    ac["origin_country"] = row[2].is_null() ? "" : row[2];
                    ac["time_position"] = field(3);
                    ac["last_contact"] = field(4);
                    ac["lat"] = row[6];
                    ac["lon"] = row[5];
                    ac["baro_altitude_m"] = field(7);
                    ac["geo_altitude_m"] = field(13);
                    ac["altitude_m"] = field(13).is_null() ? field(7) : field(13); // best-available, kept for backward compat
                    ac["on_ground"] = row[8];
                    ac["velocity_mps"] = field(9);
                    ac["heading_deg"] = field(10);
                    ac["vertical_rate_mps"] = field(11);
                    ac["sensors"] = field(12);
                    ac["squawk"] = field(14);
                    ac["spi"] = row.size() > 15 && !row[15].is_null() ? row[15] : json(false);
                    ac["position_source"] = field(16);

                    int cat = (row.size() > 17 && !row[17].is_null()) ? row[17].get<int>() : 0;
                    ac["category"] = cat;
                    ac["category_name"] = opensky_category_name(cat);
                    ac["category_known"] = cat != 0 && cat != 1;
                    ac["is_helicopter"] = cat == 8;
                    ac["is_uav"] = cat == 14;
                    ac["source"] = "opensky";

                    new_aircraft.push_back(ac);
                }
            }
            aircraft_ = new_aircraft;
            last_error_ = std::nullopt;
        } catch (const std::exception& e) {
            last_error_ = std::string("JSON parse error: ") + e.what();
        }
        rebuild_cache();
    }

    // Serialize once per poll (every OPENSKY_POLL_SECONDS) rather than
    // re-copying + re-dumping the whole aircraft array on every
    // /api/aircraft request. Caller must hold mu_.
    void rebuild_cache() {
        json out;
        out["aircraft"] = aircraft_;
        out["last_poll"] = last_poll_.has_value() ? json(*last_poll_) : json(nullptr);
        out["error"] = last_error_.has_value() ? json(*last_error_) : json(nullptr);
        out["authenticated"] = !client_id_.empty();
        out["source"] = "opensky";
        out["count"] = aircraft_.size();
        cached_json_ = out.dump();
    }

    std::string client_id_, client_secret_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    json aircraft_ = json::array();
    std::string cached_json_ =
        R"({"aircraft":[],"last_poll":null,"error":null,"authenticated":false,"source":"opensky","count":0})";
    std::optional<double> last_poll_;
    std::optional<std::string> last_error_;
};
