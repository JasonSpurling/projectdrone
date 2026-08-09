// manufacturer_registry.hpp -- structural parsing of ANSI/CTA-2063-A UAS
// serial numbers, plus an optional, user-populated cross-reference table.
//
// Honest scope note: ANSI/CTA-2063-A defines a Remote ID "Serial Number"
// (id_type 1 in the Basic ID message) as a 4-character Manufacturer Code
// Designator (MCD) followed by a manufacturer-assigned serial. The MCD
// registry is administered by CTA (Consumer Technology Association) under
// an application process -- it is not a dataset this program ships with,
// because I don't have a verified, current copy of that assignment list
// and would rather return "unknown" than fabricate manufacturer names
// against codes I'm not certain about. What this file *does* do:
//   - correctly parse the serial's structure (MCD + serial suffix) so the
//     pieces are available even with no name attached,
//   - let you drop a real registry export in as manufacturer_codes.json
//     (a flat {"CODE": "Manufacturer Name"} object) next to the binary,
//     which is loaded at startup if present.
// For id_type 2 (CAA Registration ID -- in the US, an FAA UAS registration
// number): there is no public FAA API to reverse-lookup a registration
// number to an owner. registry.faa.gov's public search is for manned
// aircraft N-numbers only. This is surfaced honestly as "no public lookup
// available" rather than silently returning nothing.
#pragma once
#include <string>
#include <map>
#include <fstream>
#include "models.hpp"

namespace remoteid {

class ManufacturerRegistry {
public:
    // manufacturer_codes.json, if present next to the binary, is a flat
    // {"MCD": "Manufacturer Name"} object. Missing/malformed file just
    // means every lookup returns "unknown" -- never fatal.
    void load(const std::string& path = "manufacturer_codes.json") {
        std::ifstream f(path);
        if (!f) return;
        try {
            json j; f >> j;
            if (j.is_object()) {
                for (auto& [code, name] : j.items()) {
                    if (name.is_string()) codes_[code] = name.get<std::string>();
                }
            }
        } catch (...) { /* leave codes_ as whatever loaded before the error */ }
    }

    size_t loaded_count() const { return codes_.size(); }

    json list() const {
        json j = json::object();
        for (auto& [code, name] : codes_) j[code] = name;
        return j;
    }

    // Splits a Basic ID "serial_number"-type UAS ID into its
    // Manufacturer Code Designator (first 4 chars) and serial suffix, and
    // attaches a manufacturer name only if the optional table has it.
    json lookup(const std::string& uas_id) const {
        json j;
        if (uas_id.size() < 5) {
            j["manufacturer_code"] = nullptr;
            j["serial_suffix"] = uas_id;
            j["manufacturer_name"] = nullptr;
            j["note"] = "too short to contain a 4-char manufacturer code designator";
            return j;
        }
        std::string code = uas_id.substr(0, 4);
        std::string suffix = uas_id.substr(4);
        j["manufacturer_code"] = code;
        j["serial_suffix"] = suffix;
        auto it = codes_.find(code);
        if (it != codes_.end()) {
            j["manufacturer_name"] = it->second;
        } else {
            j["manufacturer_name"] = nullptr;
            j["note"] = "manufacturer code not in local table (see manufacturer_codes.json)";
        }
        return j;
    }

    static json registration_id_note() {
        return json{
            {"lookup_available", false},
            {"note", "No public API exists to reverse-lookup an FAA UAS Remote ID "
                     "registration number to an owner; registry.faa.gov's public "
                     "search covers manned-aircraft N-numbers only."},
        };
    }

private:
    std::map<std::string, std::string> codes_;
};

} // namespace remoteid
