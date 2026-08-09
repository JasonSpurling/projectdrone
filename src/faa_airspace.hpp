// faa_airspace.hpp -- restricted/no-fly airspace polygons, worldwide-capable,
// pulled from public sources:
//   1. FAA UAS Facility Map + National Security UAS Flight Restrictions
//      (US-specific, via ArcGIS REST -- see notes below on URL stability)
//   2. OpenAIP (openaip.net) -- worldwide airspace data including
//      prohibited/restricted/danger areas, via a free API (register for a
//      free API key at https://www.openaip.net/users/sign_up)
// See the note above fetch_openaip() for why FAA Class Airspace (Class
// B/C/D/E boundaries) was evaluated and NOT included.
//
// Both fetches degrade gracefully: on failure, whatever was last
// successfully fetched keeps being served (never blanks the map), and the
// error is surfaced via get_airspace()["error"] rather than crashing.
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

constexpr double AIRSPACE_REFRESH_SECONDS = 24.0 * 3600.0; // restriction polygons change rarely

// Best-known ArcGIS FeatureServer query endpoints for FAA UAS airspace data.
// ArcGIS Hub item URLs can change when the FAA republishes a dataset -- if
// these start erroring, get current URLs from
// https://udds-faa.opendata.arcgis.com/ (search the dataset, open its item
// page, go to "API" -> copy the "Query" URL) and update here.
constexpr const char* FAA_UAS_FACILITY_MAP_URL =
    "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
    "FAA_UAS_FacilityMap_Data/FeatureServer/0/query";
constexpr const char* FAA_SECURITY_RESTRICTIONS_URL =
    "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
    "DoD_Mar_13/FeatureServer/0/query";
constexpr const char* OPENAIP_AIRSPACES_URL = "https://api.core.openaip.net/api/airspaces";
// FAA's own GeoServer WFS feed backing tfr.faa.gov's public map -- found
// by observing the real network calls that page makes (no documented
// public API exists for this), confirmed live: ~150 current TFR polygons,
// well within a single unpaginated request. If this starts erroring, load
// https://tfr.faa.gov/tfr3/ in a browser with devtools open and check
// what URL the map actually requests now.
constexpr const char* FAA_TFR_URL =
    "https://tfr.faa.gov/geoserver/TFR/ows?service=WFS&version=2.0.0&request=GetFeature"
    "&typeName=TFR:V_TFR_LOC&outputFormat=application/json&srsname=EPSG:4326";

class AirspaceFeed {
public:
    void start() {
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                refresh();
                std::this_thread::sleep_for(std::chrono::duration<double>(AIRSPACE_REFRESH_SECONDS));
            }
        });
        worker_.detach();
    }

    // Fetch immediately (also called once at startup by main.cpp, so the
    // map has data right away rather than waiting a full refresh cycle).
    void refresh() {
        // FAA_UAS_FacilityMap_Data is ~380,000 tiny LAANC grid cells nationwide
        // -- meant for point/area lookups, not a bulk worldwide fetch, so this
        // one intentionally stays capped to a single page (first ~2000). The
        // other two sources are small enough to fetch in full via pagination.
        auto faa_facility = fetch_faa(FAA_UAS_FACILITY_MAP_URL, "faa_uas_facility_map", "altitude_ceiling", /*paginate=*/false);
        auto faa_security = fetch_faa(FAA_SECURITY_RESTRICTIONS_URL, "faa_national_security", "no_fly", /*paginate=*/true);
        auto tfr = fetch_tfr();
        auto openaip = fetch_openaip();

        std::lock_guard<std::mutex> lock(mu_);
        std::string errors;
        if (faa_facility.has_value()) faa_facility_map_ = *faa_facility;
        else errors += "faa_facility_map fetch failed; ";
        if (faa_security.has_value()) faa_security_ = *faa_security;
        else errors += "faa_security fetch failed; ";
        if (tfr.has_value()) faa_tfr_ = *tfr;
        else errors += "faa_tfr fetch failed; ";
        if (openaip.has_value()) openaip_ = *openaip;
        else errors += "openaip fetch failed (needs OPENAIP_API_KEY); ";
        last_error_ = errors.empty() ? std::nullopt : std::optional(errors);
        last_fetch_ = now_seconds();

        // Polygon data (thousands of features) rarely changes -- serialize once
        // here rather than re-copying + re-dumping the whole tree on every
        // /api/airspace request.
        json out;
        out["faa_uas_facility_map"] = faa_facility_map_;
        out["faa_national_security_restrictions"] = faa_security_;
        out["faa_tfr"] = faa_tfr_;
        out["openaip_restrictions"] = openaip_;
        out["last_fetch"] = last_fetch_.has_value() ? json(*last_fetch_) : json(nullptr);
        out["error"] = last_error_.has_value() ? json(*last_error_) : json(nullptr);
        cached_json_ = out.dump();
    }

    std::string get_airspace_json() {
        std::lock_guard<std::mutex> lock(mu_);
        return cached_json_;
    }

private:
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static std::optional<std::string> http_get(const std::string& url,
                                                const std::vector<std::string>& extra_headers = {}) {
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        std::string response;
        struct curl_slist* headers = nullptr;
        for (auto& h : extra_headers) headers = curl_slist_append(headers, h.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        // 20s was too tight -- confirmed live that ArcGIS can take just
        // over 20s to compute a single page of the security-restrictions
        // dataset at higher resultOffset values, causing that page's
        // request to time out and fetch_faa() to silently return with
        // only the first page (1000 of ~2200+ real features) rather than
        // the full paginated dataset. This wasn't the geometry-simplification
        // slowness the "FAA Class Airspace" cut documented -- it's this
        // dataset, at pagination, being slower than expected.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || http_code != 200) return std::nullopt;
        return response;
    }

    // FAA ArcGIS FeatureServer, queried as GeoJSON, worldwide extent
    // (envelope covering the whole globe -- ArcGIS still only returns
    // features that exist, so this is fine for a US-only dataset).
    // When paginate is true, loops on resultOffset until ArcGIS stops
    // reporting exceededTransferLimit, so the full dataset is retrieved
    // rather than just the first page (ArcGIS's own page cap is 2000).
    std::optional<json> fetch_faa(const std::string& base_url, const std::string& source, const std::string& kind, bool paginate) {
        json areas = json::array();
        const int page_size = 2000;
        const int max_pages = paginate ? 50 : 1; // safety backstop, not expected to be hit
        int offset = 0;
        bool got_any_page = false;

        for (int page = 0; page < max_pages; ++page) {
            std::string url = base_url +
                "?f=geojson&outFields=*&geometry=-180,-90,180,90"
                "&geometryType=esriGeometryEnvelope&spatialRel=esriSpatialRelIntersects"
                "&inSR=4326&outSR=4326&resultRecordCount=" + std::to_string(page_size) +
                "&resultOffset=" + std::to_string(offset);
            auto body = http_get(url);
            if (!body.has_value()) return got_any_page ? std::optional<json>(areas) : std::nullopt;

            bool exceeded = false;
            try {
                auto geojson = json::parse(*body);
                got_any_page = true;
                if (!geojson.contains("features")) break; // valid empty response
                size_t page_feature_count = geojson["features"].size();
                for (auto& feat : geojson["features"]) {
                    if (!feat.contains("geometry") || feat["geometry"].is_null()) continue;
                    auto& geom = feat["geometry"];
                    std::string gtype = geom.value("type", "");
                    if (gtype != "Polygon" && gtype != "MultiPolygon") continue;

                    json ring = (gtype == "Polygon") ? geom["coordinates"][0] : geom["coordinates"][0][0];
                    json latlon_ring = json::array();
                    for (auto& coord : ring) {
                        // GeoJSON is [lon, lat] -> we want [lat, lon] for the frontend
                        latlon_ring.push_back({coord[1], coord[0]});
                    }

                    auto props = feat.value("properties", json::object());
                    json area;
                    area["name"] = props.value("APT1_NAME", props.value("SITE", props.value("Facility",
                        props.value("NAME", props.value("SITE_NAME", std::string("Restricted area"))))));
                    area["source"] = source;
                    area["kind"] = kind;
                    area["ceiling_ft"] = props.contains("CEILING") ? props["CEILING"] : json(nullptr);
                    area["polygon"] = latlon_ring;
                    areas.push_back(area);
                }
                exceeded = geojson.value("properties", json::object()).value("exceededTransferLimit", false);
                // Advance by what the server actually returned, not the
                // requested page size -- ArcGIS can truncate a page short of
                // resultRecordCount (e.g. on response-size limits), and
                // advancing by the requested size in that case would skip
                // records.
                offset += (int)page_feature_count;
                if (page_feature_count == 0) break;
            } catch (...) {
                return got_any_page ? std::optional<json>(areas) : std::nullopt;
            }

            if (!paginate || !exceeded) break;
        }
        return areas;
    }

    // NOTE on FAA Class Airspace (Class B/C/D/E boundaries around towered
    // airports): deliberately NOT integrated. Verified by hand: this
    // dataset's polygons are extremely high-vertex (precise circular-arc
    // boundaries), averaging ~25-65KB of geometry PER FEATURE -- a 200-record
    // page alone ran 5-6MB and took ~20s even with server-side geometry
    // simplification (maxAllowableOffset) applied; the full ~6,061-feature
    // dataset extrapolates to 150-400MB and several minutes to fetch. That
    // would either block server startup for minutes or ship a payload large
    // enough to reintroduce the exact page-load slowness this app was
    // already fixed for once. Airports themselves are still fully covered,
    // worldwide, via AirportsFeed (airports_feed.hpp) -- just not their
    // controlled-airspace boundary shapes.

    // OpenAIP -- worldwide airspace data. Requires a free API key
    // (OPENAIP_API_KEY env var); without one this simply returns nullopt
    // and the FAA layers still work fine on their own.
    std::optional<json> fetch_openaip() {
        const char* api_key = std::getenv("OPENAIP_API_KEY");
        if (!api_key) return std::nullopt;

        // type 4/5/6 in OpenAIP's schema roughly correspond to
        // prohibited/restricted/danger areas -- adjust per their current
        // docs at https://www.openaip.net/docs/api if this changes.
        std::string url = std::string(OPENAIP_AIRSPACES_URL) + "?type=4,5,6&limit=1000";
        auto body = http_get(url, {std::string("x-openaip-api-key: ") + api_key});
        if (!body.has_value()) return std::nullopt;

        try {
            auto data = json::parse(*body);
            json areas = json::array();
            if (!data.contains("items")) return areas;
            for (auto& item : data["items"]) {
                if (!item.contains("geometry")) continue;
                auto& geom = item["geometry"];
                if (geom.value("type", "") != "Polygon") continue;

                json latlon_ring = json::array();
                for (auto& coord : geom["coordinates"][0]) {
                    latlon_ring.push_back({coord[1], coord[0]});
                }
                json area;
                area["name"] = item.value("name", std::string("Restricted airspace"));
                area["source"] = "openaip";
                area["kind"] = "no_fly";
                area["ceiling_ft"] = json(nullptr);
                area["polygon"] = latlon_ring;
                areas.push_back(area);
            }
            return areas;
        } catch (...) {
            return std::nullopt;
        }
    }

    // FAA Temporary Flight Restrictions -- current polygons from the same
    // GeoServer WFS the official tfr.faa.gov map itself calls (see
    // FAA_TFR_URL above for how that was found). Small dataset (~150
    // features), no pagination needed.
    std::optional<json> fetch_tfr() {
        auto body = http_get(FAA_TFR_URL);
        if (!body.has_value()) return std::nullopt;

        try {
            auto geojson = json::parse(*body);
            json areas = json::array();
            if (!geojson.contains("features")) return areas;
            for (auto& feat : geojson["features"]) {
                if (!feat.contains("geometry") || feat["geometry"].is_null()) continue;
                auto& geom = feat["geometry"];
                std::string gtype = geom.value("type", "");
                if (gtype != "Polygon" && gtype != "MultiPolygon") continue;

                json ring = (gtype == "Polygon") ? geom["coordinates"][0] : geom["coordinates"][0][0];
                json latlon_ring = json::array();
                for (auto& coord : ring) {
                    // GeoJSON is [lon, lat] -> [lat, lon] for the frontend, same as fetch_faa()
                    latlon_ring.push_back({coord[1], coord[0]});
                }

                auto props = feat.value("properties", json::object());
                json area;
                area["name"] = props.value("TITLE", std::string("Temporary Flight Restriction"));
                area["source"] = "faa_tfr";
                area["kind"] = "no_fly";
                area["ceiling_ft"] = json(nullptr); // not provided by this feed
                area["notam_key"] = props.value("NOTAM_KEY", std::string());
                area["legal_type"] = props.value("LEGAL", std::string()); // e.g. "SECURITY", "HAZARDS"
                area["polygon"] = latlon_ring;
                areas.push_back(area);
            }
            return areas;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    json faa_facility_map_ = json::array();
    json faa_security_ = json::array();
    json faa_tfr_ = json::array();
    json openaip_ = json::array();
    std::optional<double> last_fetch_;
    std::optional<std::string> last_error_;
    std::string cached_json_ =
        R"({"faa_uas_facility_map":[],"faa_national_security_restrictions":[],"faa_tfr":[],"openaip_restrictions":[],"last_fetch":null,"error":null})";
};
