// airports_feed.hpp -- worldwide airport/heliport directory, from
// OurAirports.com's free public-domain CSV (no API key, ~80k facilities).
// This dataset changes far less often than live traffic or even restricted
// airspace, so it's refreshed on a much longer cycle.
#pragma once
#include <curl/curl.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>
#include <atomic>
#include <sstream>
#include "models.hpp"

constexpr double AIRPORTS_REFRESH_SECONDS = 7.0 * 24.0 * 3600.0; // airport locations barely ever change
constexpr const char* AIRPORTS_CSV_URL = "https://davidmegginson.github.io/ourairports-data/airports.csv";

class AirportsFeed {
public:
    void start() {
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                refresh();
                std::this_thread::sleep_for(std::chrono::duration<double>(AIRPORTS_REFRESH_SECONDS));
            }
        });
        worker_.detach();
    }

    // Fetch immediately (also called once at startup by main.cpp, so the
    // map has airports right away rather than waiting a full refresh cycle).
    void refresh() {
        auto body = http_get(AIRPORTS_CSV_URL);

        std::lock_guard<std::mutex> lock(mu_);
        last_fetch_ = now_seconds();
        if (!body.has_value()) {
            last_error_ = "airports fetch failed";
            rebuild_cache();
            return;
        }
        try {
            airports_ = parse_csv(*body);
            last_error_ = std::nullopt;
        } catch (const std::exception& e) {
            last_error_ = std::string("airports parse error: ") + e.what();
        }
        rebuild_cache();
    }

    std::string get_airports_json() {
        std::lock_guard<std::mutex> lock(mu_);
        return cached_json_;
    }

private:
    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    static std::optional<std::string> http_get(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || http_code != 200) return std::nullopt;
        return response;
    }

    // Minimal RFC4180 CSV field splitter: handles double-quoted fields that
    // may themselves contain commas, and "" as an escaped quote.
    static std::vector<std::string> split_csv_line(const std::string& line) {
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

    static json parse_csv(const std::string& csv) {
        json out = json::array();
        std::istringstream stream(csv);
        std::string line;
        bool header_seen = false;
        std::map<std::string, size_t> col;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto fields = split_csv_line(line);

            if (!header_seen) {
                for (size_t i = 0; i < fields.size(); ++i) col[fields[i]] = i;
                header_seen = true;
                continue;
            }

            auto get = [&](const char* name) -> std::string {
                auto it = col.find(name);
                if (it == col.end() || it->second >= fields.size()) return "";
                return fields[it->second];
            };

            std::string type = get("type");
            if (type == "closed") continue; // "all airports" means all operational ones
            std::string lat_s = get("latitude_deg"), lon_s = get("longitude_deg");
            if (lat_s.empty() || lon_s.empty()) continue;

            json a;
            try {
                a["lat"] = std::stod(lat_s);
                a["lon"] = std::stod(lon_s);
            } catch (...) { continue; }

            a["ident"] = get("ident");
            a["type"] = type;
            a["name"] = get("name");
            std::string elev = get("elevation_ft");
            if (elev.empty()) a["elevation_ft"] = nullptr;
            else { try { a["elevation_ft"] = std::stod(elev); } catch (...) { a["elevation_ft"] = nullptr; } }
            a["continent"] = get("continent");
            a["iso_country"] = get("iso_country");
            a["iso_region"] = get("iso_region");
            a["municipality"] = get("municipality");
            a["scheduled_service"] = get("scheduled_service") == "yes";
            a["gps_code"] = get("gps_code");
            a["icao_code"] = get("icao_code");
            a["iata_code"] = get("iata_code");
            a["local_code"] = get("local_code");
            out.push_back(a);
        }
        return out;
    }

    // Serialize once per refresh (weekly) rather than re-copying + re-dumping
    // ~80k airport records on every /api/airports request. Caller must hold mu_.
    void rebuild_cache() {
        json out;
        out["airports"] = airports_;
        out["count"] = airports_.size();
        out["last_fetch"] = last_fetch_.has_value() ? json(*last_fetch_) : json(nullptr);
        out["error"] = last_error_.has_value() ? json(*last_error_) : json(nullptr);
        cached_json_ = out.dump();
    }

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    json airports_ = json::array();
    std::optional<double> last_fetch_;
    std::optional<std::string> last_error_;
    std::string cached_json_ = R"({"airports":[],"count":0,"last_fetch":null,"error":null})";
};
