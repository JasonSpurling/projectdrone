// runways_feed.hpp -- runway layout data, from OurAirports.com's free
// public-domain CSV (same source/no-key pattern as airports_feed.hpp).
// This is what lets the frontend actually draw a selected airport's
// outline: each runway's two endpoint coordinates + width, rather than
// just a single point marker.
//
// Honest scope: only ~1/3 of OurAirports' ~48k runway rows have both
// endpoints' lat/lon populated (confirmed live) -- small unpaved
// airstrips often only have a single point for the whole airport, no
// per-runway geometry. Rows without full endpoint coordinates are
// dropped at load time since there's nothing to draw for them; the
// frontend shows "no runway geometry available" rather than a fabricated
// outline when an airport isn't in this set.
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

constexpr double RUNWAYS_REFRESH_SECONDS = 7.0 * 24.0 * 3600.0; // same slow-changing cadence as airports
constexpr const char* RUNWAYS_CSV_URL = "https://davidmegginson.github.io/ourairports-data/runways.csv";

class RunwaysFeed {
public:
    void start() {
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                refresh();
                std::this_thread::sleep_for(std::chrono::duration<double>(RUNWAYS_REFRESH_SECONDS));
            }
        });
        worker_.detach();
    }

    void refresh() {
        auto body = http_get(RUNWAYS_CSV_URL);

        std::lock_guard<std::mutex> lock(mu_);
        last_fetch_ = now_seconds();
        if (!body.has_value()) {
            last_error_ = "runways fetch failed";
            rebuild_cache();
            return;
        }
        try {
            runways_ = parse_csv(*body);
            last_error_ = std::nullopt;
        } catch (const std::exception& e) {
            last_error_ = std::string("runways parse error: ") + e.what();
        }
        rebuild_cache();
    }

    std::string get_runways_json() {
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

    // Same RFC4180-ish splitter as airports_feed.hpp -- runway surface/
    // ident fields can be quoted.
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

            if (get("closed") == "1") continue;

            std::string le_lat_s = get("le_latitude_deg"), le_lon_s = get("le_longitude_deg");
            std::string he_lat_s = get("he_latitude_deg"), he_lon_s = get("he_longitude_deg");
            if (le_lat_s.empty() || le_lon_s.empty() || he_lat_s.empty() || he_lon_s.empty()) continue; // see file header

            json r;
            try {
                r["le_lat"] = std::stod(le_lat_s);
                r["le_lon"] = std::stod(le_lon_s);
                r["he_lat"] = std::stod(he_lat_s);
                r["he_lon"] = std::stod(he_lon_s);
            } catch (...) { continue; }

            r["airport_ident"] = get("airport_ident");
            r["le_ident"] = get("le_ident");
            r["he_ident"] = get("he_ident");
            std::string width = get("width_ft");
            r["width_ft"] = width.empty() ? json(nullptr) : json([&] { try { return std::stod(width); } catch (...) { return 0.0; } }());
            std::string length = get("length_ft");
            r["length_ft"] = length.empty() ? json(nullptr) : json([&] { try { return std::stod(length); } catch (...) { return 0.0; } }());
            r["surface"] = get("surface");
            out.push_back(r);
        }
        return out;
    }

    void rebuild_cache() {
        json out;
        out["runways"] = runways_;
        out["count"] = runways_.size();
        out["last_fetch"] = last_fetch_.has_value() ? json(*last_fetch_) : json(nullptr);
        out["error"] = last_error_.has_value() ? json(*last_error_) : json(nullptr);
        cached_json_ = out.dump();
    }

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    json runways_ = json::array();
    std::optional<double> last_fetch_;
    std::optional<std::string> last_error_;
    std::string cached_json_ = R"({"runways":[],"count":0,"last_fetch":null,"error":null})";
};
