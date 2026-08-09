// faa_aircraft_registry.hpp -- FAA Releasable Aircraft Registry (US
// N-number registrations), free and public, no API key -- confirmed live
// at registry.faa.gov/database/ReleasableAircraft.zip. This is the
// authoritative US civil aircraft registration database: every N-number's
// owner/operator, make/model, aircraft type (including whether it's a
// helicopter), and -- critically for this project -- its Mode S / ICAO24
// hex address, which lets a live ADS-B sighting be cross-referenced back
// to who actually owns the aircraft.
//
// The service blocks requests with no/non-browser User-Agent (returns 403
// via Akamai) -- confirmed live, worked once a normal browser UA string
// was set.
//
// This is a big, slow-changing dataset (~315k rows, ~70MB zipped) --
// refreshed weekly, not on a live-feed cadence, and cached to disk (DATA
// dir) so a container restart doesn't force a fresh 70MB download every
// time. Extracted in-memory from the .zip via libzip rather than shelling
// out to `unzip` or writing extracted files to disk.
//
// Two files matter out of the zip's ~8 members:
//   MASTER.txt  -- one row per N-number: registrant, MFR MDL CODE (joins
//                  ACFTREF), TYPE AIRCRAFT (1=glider..6=rotorcraft..),
//                  STATUS CODE, MODE S CODE HEX (icao24, uppercase).
//   ACFTREF.txt -- one row per MFR MDL CODE: manufacturer + model name.
// Both are joined at load time into one lean in-memory record per
// N-number, indexed by N-number and (when present) by icao24.
#pragma once
#include <curl/curl.h>
#include <zip.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <sys/stat.h>
#include "models.hpp"

constexpr double FAA_REGISTRY_REFRESH_SECONDS = 7.0 * 24.0 * 3600.0; // weekly -- this dataset barely changes day to day
constexpr const char* FAA_REGISTRY_URL = "https://registry.faa.gov/database/ReleasableAircraft.zip";

// TYPE AIRCRAFT is a single-character FAA code -- mapped here so the API
// can say "helicopter" instead of "6". Per FAA's published registry
// layout (ardata.pdf, included in the same zip).
inline const char* faa_aircraft_type_name(const std::string& code) {
    if (code == "1") return "glider";
    if (code == "2") return "balloon";
    if (code == "3") return "blimp_dirigible";
    if (code == "4") return "fixed_wing_single_engine";
    if (code == "5") return "fixed_wing_multi_engine";
    if (code == "6") return "rotorcraft"; // helicopter
    if (code == "7") return "weight_shift_control";
    if (code == "8") return "powered_parachute";
    if (code == "9") return "gyroplane";
    if (code == "H") return "hybrid_lift";
    if (code == "O") return "other";
    return "unknown";
}

struct FaaAircraftRecord {
    std::string n_number;
    std::string icao24; // lowercase, matches this app's convention elsewhere; empty if FAA has no Mode S hex on file
    std::string registrant_name;
    std::string city, state;
    std::string manufacturer, model;
    std::string type_aircraft_code; // raw FAA code, see faa_aircraft_type_name()
    std::string year_mfr;
    std::string status_code; // e.g. "V" = valid; FAA's status codes aren't exhaustively decoded here
};

class FaaAircraftRegistry {
public:
    explicit FaaAircraftRegistry(std::string cache_path) : cache_path_(std::move(cache_path)) {}

    void start() {
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                refresh();
                std::this_thread::sleep_for(std::chrono::duration<double>(FAA_REGISTRY_REFRESH_SECONDS));
            }
        });
        worker_.detach();
    }

    // Downloads only if the on-disk cache is missing/stale; otherwise
    // parses straight from the cached zip. Safe to call directly at
    // startup (main.cpp does) so the registry is usable immediately on a
    // warm cache instead of waiting for the first scheduled refresh.
    void refresh() {
        bool need_download = true;
        struct stat st{};
        if (stat(cache_path_.c_str(), &st) == 0) {
            double age = now_seconds() - static_cast<double>(st.st_mtime);
            if (age < FAA_REGISTRY_REFRESH_SECONDS) need_download = false;
        }

        if (need_download) {
            if (!download_to(cache_path_)) {
                std::lock_guard<std::mutex> lock(mu_);
                // Keep serving whatever was already loaded (possibly
                // nothing yet) rather than wiping it over a failed refresh.
                last_error_ = by_n_number_.empty() ? std::string("download failed and no cached data available")
                                                    : std::string("refresh download failed, still serving previous data");
                if (!by_n_number_.empty()) return;
                // No cache and no prior data -- nothing to parse.
                last_load_ = now_seconds();
                return;
            }
        }

        std::string master, acftref;
        std::string extract_err;
        if (!extract_members(cache_path_, master, acftref, extract_err)) {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = "zip extract failed: " + extract_err;
            last_load_ = now_seconds();
            return;
        }

        auto mfr_model = parse_acftref(acftref);
        auto records = parse_master(master, mfr_model);

        std::lock_guard<std::mutex> lock(mu_);
        by_n_number_.clear();
        by_icao24_.clear();
        for (auto& r : records) {
            std::string n = r.n_number;
            std::string icao = r.icao24;
            by_n_number_.emplace(n, std::move(r));
            if (!icao.empty()) by_icao24_[icao] = n; // secondary index, points back into by_n_number_
        }
        last_error_ = std::nullopt;
        last_load_ = now_seconds();
    }

    struct LookupResult {
        bool found = false;
        FaaAircraftRecord record;
    };

    LookupResult lookup_n_number(std::string n_number) const {
        for (auto& c : n_number) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!n_number.empty() && n_number[0] != 'N') n_number = "N" + n_number;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = by_n_number_.find(n_number);
        if (it == by_n_number_.end()) return {};
        return {true, it->second};
    }

    LookupResult lookup_icao24(std::string icao24) const {
        for (auto& c : icao24) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::lock_guard<std::mutex> lock(mu_);
        auto idx = by_icao24_.find(icao24);
        if (idx == by_icao24_.end()) return {};
        auto it = by_n_number_.find(idx->second);
        if (it == by_n_number_.end()) return {};
        return {true, it->second};
    }

    json status_json() const {
        std::lock_guard<std::mutex> lock(mu_);
        json j;
        j["loaded_count"] = by_n_number_.size();
        j["last_load"] = last_load_.has_value() ? json(*last_load_) : json(nullptr);
        j["error"] = last_error_.has_value() ? json(*last_error_) : json(nullptr);
        return j;
    }

    static json record_to_json(const FaaAircraftRecord& r) {
        json j;
        j["n_number"] = r.n_number;
        j["icao24"] = r.icao24.empty() ? json(nullptr) : json(r.icao24);
        j["registrant_name"] = r.registrant_name;
        j["city"] = r.city;
        j["state"] = r.state;
        j["manufacturer"] = r.manufacturer;
        j["model"] = r.model;
        j["type_aircraft"] = faa_aircraft_type_name(r.type_aircraft_code);
        j["is_helicopter"] = r.type_aircraft_code == "6";
        j["year_mfr"] = r.year_mfr.empty() ? json(nullptr) : json(r.year_mfr);
        j["status_code"] = r.status_code;
        return j;
    }

private:
    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* out = static_cast<std::ofstream*>(userp);
        out->write(static_cast<char*>(contents), static_cast<std::streamsize>(size * nmemb));
        return size * nmemb;
    }

    static bool download_to(const std::string& path) {
        std::string tmp_path = path + ".part";
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out.is_open()) return false;

        CURL* curl = curl_easy_init();
        if (!curl) return false;
        curl_easy_setopt(curl, CURLOPT_URL, FAA_REGISTRY_URL);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L); // ~70MB over whatever link the container has
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // Confirmed live: registry.faa.gov (Akamai-fronted) returns 403 to
        // curl's default/no User-Agent. A normal browser UA string works.
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        long http_code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        out.close();

        if (res != CURLE_OK || http_code != 200) { std::remove(tmp_path.c_str()); return false; }
        std::remove(path.c_str());
        return std::rename(tmp_path.c_str(), path.c_str()) == 0;
    }

    // FAA exports both members with a leading UTF-8 BOM (confirmed live:
    // EF BB BF right before "N-NUMBER"/"CODE") -- left in place, it
    // silently glues itself onto the first header column name, so
    // col["N-NUMBER"] never matches and every row gets skipped as
    // "n_number empty" with no error surfaced anywhere.
    static void strip_bom(std::string& s) {
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
            s.erase(0, 3);
        }
    }

    static bool extract_members(const std::string& zip_path, std::string& master_out, std::string& acftref_out, std::string& err_out) {
        int errcode = 0;
        zip_t* archive = zip_open(zip_path.c_str(), ZIP_RDONLY, &errcode);
        if (!archive) { err_out = "zip_open failed, code " + std::to_string(errcode); return false; }

        auto read_member = [&](const char* name, std::string& out) -> bool {
            zip_stat_t st;
            if (zip_stat(archive, name, 0, &st) != 0) { err_out = std::string(name) + " not found in archive"; return false; }
            zip_file_t* f = zip_fopen(archive, name, 0);
            if (!f) { err_out = std::string("failed to open ") + name; return false; }
            out.resize(st.size);
            zip_int64_t read = zip_fread(f, out.data(), st.size);
            zip_fclose(f);
            if (read < 0 || static_cast<zip_uint64_t>(read) != st.size) { err_out = std::string("short read on ") + name; return false; }
            strip_bom(out);
            return true;
        };

        bool ok = read_member("MASTER.txt", master_out) && read_member("ACFTREF.txt", acftref_out);
        zip_close(archive);
        return ok;
    }

    // FAA's CSV isn't quoted/escaped (registrant names etc. can't contain
    // commas in this dataset) -- a plain split is enough, unlike the
    // RFC4180 parser airports_feed.hpp needs for OurAirports.
    static std::vector<std::string> split_line(const std::string& line) {
        std::vector<std::string> out;
        std::string field;
        for (char c : line) {
            if (c == ',') { out.push_back(field); field.clear(); }
            else field += c;
        }
        out.push_back(field);
        return out;
    }

    static std::unordered_map<std::string, std::pair<std::string, std::string>> parse_acftref(const std::string& csv) {
        std::unordered_map<std::string, std::pair<std::string, std::string>> out;
        std::istringstream stream(csv);
        std::string line;
        bool header = true;
        std::map<std::string, size_t> col;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = split_line(line);
            if (header) { for (size_t i = 0; i < f.size(); ++i) col[f[i]] = i; header = false; continue; }
            auto get = [&](const char* name) -> std::string {
                auto it = col.find(name);
                return (it != col.end() && it->second < f.size()) ? trim(f[it->second]) : "";
            };
            std::string code = get("CODE");
            if (code.empty()) continue;
            out[code] = {get("MFR"), get("MODEL")};
        }
        return out;
    }

    static std::vector<FaaAircraftRecord> parse_master(const std::string& csv,
            const std::unordered_map<std::string, std::pair<std::string, std::string>>& mfr_model) {
        std::vector<FaaAircraftRecord> out;
        out.reserve(340000);
        std::istringstream stream(csv);
        std::string line;
        bool header = true;
        std::map<std::string, size_t> col;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = split_line(line);
            if (header) { for (size_t i = 0; i < f.size(); ++i) col[f[i]] = i; header = false; continue; }
            auto get = [&](const char* name) -> std::string {
                auto it = col.find(name);
                return (it != col.end() && it->second < f.size()) ? trim(f[it->second]) : "";
            };

            std::string n = get("N-NUMBER");
            if (n.empty()) continue;

            FaaAircraftRecord r;
            r.n_number = "N" + n; // MASTER.txt stores the number without the leading "N"
            std::string hex = get("MODE S CODE HEX");
            std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return std::tolower(c); });
            r.icao24 = hex;
            r.registrant_name = get("NAME");
            r.city = get("CITY");
            r.state = get("STATE");
            r.type_aircraft_code = get("TYPE AIRCRAFT");
            r.year_mfr = get("YEAR MFR");
            r.status_code = get("STATUS CODE");

            std::string mfr_code = get("MFR MDL CODE");
            auto mit = mfr_model.find(mfr_code);
            if (mit != mfr_model.end()) { r.manufacturer = mit->second.first; r.model = mit->second.second; }

            out.push_back(std::move(r));
        }
        return out;
    }

    std::string cache_path_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, FaaAircraftRecord> by_n_number_;
    std::unordered_map<std::string, std::string> by_icao24_; // icao24 -> n_number
    std::optional<double> last_load_;
    std::optional<std::string> last_error_;
};
