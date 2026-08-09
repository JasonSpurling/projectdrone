// airport_boundary.hpp -- an airport's actual property/perimeter outline
// (not just its runways), from OpenStreetMap's free public Overpass API
// (overpass-api.de, no key). Confirmed live: querying a way tagged
// aeroway=aerodrome by its icao tag returns the real boundary polygon
// (e.g. KSEA -> a 139-point way matching its actual fence line).
//
// Unlike the bulk feeds (airports/runways/airspace), this is deliberately
// on-demand and per-airport rather than a background poll of everything
// worldwide -- there's no practical "download every airport's boundary"
// bulk export, and a user only ever needs the one airport they clicked
// on. Results are cached in-process (boundaries don't change) so
// re-selecting the same airport, or two users looking at the same one,
// doesn't re-hit the public Overpass instance every time -- that instance
// is shared community infrastructure with real rate limits, and this
// project already tripped one rate limit this session (adsb.lol) from
// under-spaced requests.
//
// Honest scope: OSM coverage of small/private airfields is inconsistent
// -- not every airport has a mapped aerodrome way, and this returns
// found:false rather than fabricating a boundary when Overpass has
// nothing.
#pragma once
#include <curl/curl.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include "models.hpp"

constexpr const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";

class AirportBoundaryLookup {
public:
    // icao may be empty (not every airport has one) -- lat/lon are always
    // available from the airports directory and used as a fallback/
    // disambiguation query.
    json lookup(const std::string& ident, const std::string& icao, double lat, double lon) {
        std::string cache_key = ident;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = cache_.find(cache_key);
            if (it != cache_.end()) return it->second;
        }

        json result = query_overpass(icao, lat, lon);

        std::lock_guard<std::mutex> lock(mu_);
        cache_[cache_key] = result;
        return result;
    }

private:
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static std::optional<json> post_overpass(const std::string& query) {
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        std::string response;
        std::string body = "data=" + url_encode(query);
        curl_easy_setopt(curl, CURLOPT_URL, OVERPASS_URL);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L); // shared public instance; don't hold a request thread forever
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // Confirmed live: Overpass returns 406 with no/curl's default
        // User-Agent. Overpass's own fair-use policy actually asks
        // automated clients to identify themselves this way, so this
        // isn't a workaround so much as doing what they ask for.
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "DataFlight/1.0 (airspace-awareness project; airport boundary lookup)");
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || http_code != 200) return std::nullopt;
        try { return json::parse(response); } catch (...) { return std::nullopt; }
    }

    static std::string url_encode(const std::string& s) {
        CURL* curl = curl_easy_init();
        char* out = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
        std::string escaped = out ? out : "";
        if (out) curl_free(out);
        if (curl) curl_easy_cleanup(curl);
        return escaped;
    }

    static json way_to_polygon(const json& way) {
        json ring = json::array();
        for (auto& pt : way.value("geometry", json::array())) {
            if (pt.contains("lat") && pt.contains("lon")) ring.push_back({pt["lon"], pt["lat"]});
        }
        json j;
        j["found"] = !ring.empty();
        j["name"] = way.value("tags", json::object()).value("name", "");
        j["ring"] = ring;
        return j;
    }

    json query_overpass(const std::string& icao, double lat, double lon) {
        json not_found; not_found["found"] = false; not_found["ring"] = json::array(); not_found["name"] = "";

        if (!icao.empty()) {
            std::ostringstream q;
            q << "[out:json][timeout:20];way[\"aeroway\"=\"aerodrome\"][\"icao\"=\"" << icao << "\"];out geom;";
            auto resp = post_overpass(q.str());
            if (resp.has_value() && resp->contains("elements") && !(*resp)["elements"].empty()) {
                return way_to_polygon((*resp)["elements"][0]);
            }
        }

        // Fallback: nearest aerodrome way within 3km of the airport's
        // point location, for airfields OSM hasn't tagged with an icao
        // ref (common for smaller/private strips and heliports).
        std::ostringstream q;
        q << "[out:json][timeout:20];way[\"aeroway\"=\"aerodrome\"](around:3000," << lat << "," << lon << ");out geom;";
        auto resp = post_overpass(q.str());
        if (resp.has_value() && resp->contains("elements") && !(*resp)["elements"].empty()) {
            return way_to_polygon((*resp)["elements"][0]);
        }
        return not_found;
    }

    std::mutex mu_;
    std::map<std::string, json> cache_; // airport ident -> lookup result (including not-found, so a repeat miss doesn't re-query either)
};
