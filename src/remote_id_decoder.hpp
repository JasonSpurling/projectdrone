// remote_id_decoder.hpp -- ASTM F3411 / ASD-STAN Open Drone ID message-pack
// decoder, implemented from the publicly documented wire format (the same
// byte layout implemented by the open-source opendroneid-core-c reference
// library that most commercial Remote ID stacks interoperate against).
//
// Scope: this decodes the raw ODID message bytes only. Pulling those bytes
// out of a live capture is capture-hardware/library specific --
//   - Bluetooth 4/5 Legacy or Long Range advertising: the ODID payload is
//     the AD Service Data (UUID 0xFFFA) or Manufacturer Specific Data
//     payload -- extract it with a BLE sniffer/HCI tool (e.g. a nRF sniffer
//     capture, or Wireshark's builtin Open Drone ID dissector) first.
//   - Wi-Fi Beacon or NAN Service Discovery: the payload is inside a
//     vendor-specific information element -- extract it from a
//     monitor-mode capture (e.g. via a Wireshark/tshark filter) first.
// Actually parsing 802.11/BLE link-layer framing and pcap files is a much
// larger scope (a full packet-capture + radio-driver stack) that doesn't
// belong in this HTTP service; this decoder starts from the ODID payload
// bytes themselves, which the caller supplies as hex.
//
// This is a best-effort decoder against the public spec, not a certified
// conformance/interop implementation -- always cross-check against a known
// reference (e.g. opendroneid-core-c) before relying on it operationally.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "models.hpp"

namespace remoteid {

constexpr size_t ODID_MESSAGE_SIZE = 25;

inline std::string trim_nulls(const uint8_t* data, size_t len) {
    size_t end = len;
    while (end > 0 && (data[end - 1] == 0 || data[end - 1] == ' ')) --end;
    return std::string(reinterpret_cast<const char*>(data), end);
}

inline int32_t read_i32le(const uint8_t* p) {
    uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return static_cast<int32_t>(u);
}

inline uint16_t read_u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t read_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline const char* id_type_name(int t) {
    switch (t) {
        case 0: return "none";
        case 1: return "serial_number";       // ANSI/CTA-2063-A
        case 2: return "caa_registration_id";
        case 3: return "utm_assigned_id";
        case 4: return "specific_session_id";
        default: return "reserved";
    }
}

inline const char* ua_type_name(int t) {
    static const char* names[] = {
        "none", "aeroplane", "helicopter_multirotor", "gyroplane", "hybrid_lift",
        "ornithopter", "glider", "kite", "free_balloon", "captive_balloon",
        "airship", "free_fall_parachute", "rocket", "tethered_powered_aircraft",
        "ground_obstacle", "other",
    };
    return (t >= 0 && t <= 15) ? names[t] : "unknown";
}

inline const char* operational_status_name(int s) {
    switch (s) {
        case 0: return "undeclared";
        case 1: return "ground";
        case 2: return "airborne";
        case 3: return "emergency";
        case 4: return "remote_id_system_failure";
        default: return "reserved";
    }
}

inline const char* self_id_description_type_name(int t) {
    switch (t) {
        case 0: return "text_description";
        case 1: return "emergency";
        case 2: return "extended_status";
        default: return "reserved";
    }
}

inline const char* operator_location_type_name(int t) {
    switch (t) {
        case 0: return "takeoff_location";
        case 1: return "live_gnss";
        case 2: return "fixed_location";
        default: return "reserved";
    }
}

inline json decode_basic_id(const uint8_t* m) {
    int id_type = (m[1] >> 4) & 0x0F;
    int ua_type = m[1] & 0x0F;
    json j;
    j["type"] = "basic_id";
    j["id_type"] = id_type_name(id_type);
    j["ua_type"] = ua_type_name(ua_type);
    j["uas_id"] = trim_nulls(m + 2, 20);
    return j;
}

inline json decode_location_vector(const uint8_t* m) {
    int status = (m[1] >> 4) & 0x0F;
    int height_type = (m[1] >> 2) & 0x01;
    int ew_direction = (m[1] >> 1) & 0x01;
    int speed_multiplier = m[1] & 0x01;

    int direction_deg = ew_direction ? (m[2] + 180) : m[2];
    double speed_mps = speed_multiplier ? (m[3] * 0.75 + 63.75) : (m[3] * 0.25);

    int8_t vspeed_raw = static_cast<int8_t>(m[4]);

    double lat = read_i32le(m + 5) * 1e-7;
    double lon = read_i32le(m + 9) * 1e-7;

    uint16_t pbaro = read_u16le(m + 13);
    uint16_t pgeo = read_u16le(m + 15);
    uint16_t pheight = read_u16le(m + 17);

    uint16_t ts = read_u16le(m + 21);

    json j;
    j["type"] = "location_vector";
    j["operational_status"] = operational_status_name(status);
    j["height_reference"] = height_type ? "ground" : "takeoff";
    j["direction_deg"] = direction_deg;
    j["speed_mps"] = (m[3] == 255) ? json(nullptr) : json(speed_mps);
    j["vertical_speed_mps"] = (m[4] == 0x7F) ? json(nullptr) : json(vspeed_raw * 0.5);
    j["lat"] = lat;
    j["lon"] = lon;
    j["pressure_altitude_m"] = (pbaro == 0xFFFF) ? json(nullptr) : json(pbaro * 0.5 - 1000.0);
    j["geodetic_altitude_m"] = (pgeo == 0xFFFF) ? json(nullptr) : json(pgeo * 0.5 - 1000.0);
    j["height_m"] = (pheight == 0xFFFF) ? json(nullptr) : json(pheight * 0.5 - 1000.0);
    j["horizontal_accuracy_code"] = m[19] & 0x0F;
    j["vertical_accuracy_code"] = (m[19] >> 4) & 0x0F;
    j["speed_accuracy_code"] = m[20] & 0x0F;
    j["baro_altitude_accuracy_code"] = (m[20] >> 4) & 0x0F;
    j["timestamp_tenths_since_hour"] = (ts == 0xFFFF) ? json(nullptr) : json(ts);
    return j;
}

inline json decode_self_id(const uint8_t* m) {
    json j;
    j["type"] = "self_id";
    j["description_type"] = self_id_description_type_name(m[1]);
    j["description"] = trim_nulls(m + 2, 23);
    return j;
}

inline json decode_system(const uint8_t* m) {
    int operator_location_type = m[1] & 0x03;
    int classification_type = (m[1] >> 2) & 0x07;

    double op_lat = read_i32le(m + 2) * 1e-7;
    double op_lon = read_i32le(m + 6) * 1e-7;
    uint16_t area_count = read_u16le(m + 10);
    uint8_t area_radius_raw = m[12];
    uint16_t area_ceiling = read_u16le(m + 13);
    uint16_t area_floor = read_u16le(m + 15);
    uint16_t op_alt = read_u16le(m + 18);
    uint32_t ts = read_u32le(m + 20); // seconds since 2019-01-01T00:00:00Z per spec

    json j;
    j["type"] = "system";
    j["operator_location_type"] = operator_location_type_name(operator_location_type);
    j["ua_classification_type"] = classification_type;
    j["operator_lat"] = op_lat;
    j["operator_lon"] = op_lon;
    j["area_count"] = area_count;
    j["area_radius_m"] = area_radius_raw * 10.0;
    j["area_ceiling_m"] = (area_ceiling == 0xFFFF) ? json(nullptr) : json(area_ceiling * 0.5 - 1000.0);
    j["area_floor_m"] = (area_floor == 0xFFFF) ? json(nullptr) : json(area_floor * 0.5 - 1000.0);
    j["operator_altitude_geodetic_m"] = (op_alt == 0xFFFF) ? json(nullptr) : json(op_alt * 0.5 - 1000.0);
    j["timestamp_epoch_2019_s"] = ts;
    return j;
}

inline json decode_operator_id(const uint8_t* m) {
    json j;
    j["type"] = "operator_id";
    j["operator_id_type"] = (m[1] == 0) ? "operator_id" : "reserved";
    j["operator_id"] = trim_nulls(m + 2, 20);
    return j;
}

inline json decode_message(const uint8_t* m) {
    int msg_type = (m[0] >> 4) & 0x0F;
    int protocol_version = m[0] & 0x0F;
    json j;
    switch (msg_type) {
        case 0x0: j = decode_basic_id(m); break;
        case 0x1: j = decode_location_vector(m); break;
        case 0x3: j = decode_self_id(m); break;
        case 0x4: j = decode_system(m); break;
        case 0x5: j = decode_operator_id(m); break;
        case 0x2: j = {{"type", "authentication"}, {"note", "auth message present but not decoded (opaque auth payload, not needed for tracking)"}}; break;
        default: j = {{"type", "unknown"}, {"raw_type", msg_type}}; break;
    }
    j["protocol_version"] = protocol_version;
    return j;
}

struct DecodeResult {
    bool ok = false;
    std::string error;
    json messages = json::array();
};

// Accepts either a single 25-byte message or a Message Pack (type 0xF)
// wrapping several. Real-world captures are almost always a Message Pack
// since that's how a drone broadcasts its full state in one advertisement.
inline DecodeResult decode_payload(const std::vector<uint8_t>& bytes) {
    DecodeResult result;
    if (bytes.size() < ODID_MESSAGE_SIZE) {
        result.error = "payload shorter than one ODID message (25 bytes)";
        return result;
    }

    int msg_type = (bytes[0] >> 4) & 0x0F;
    if (msg_type == 0xF) {
        if (bytes.size() < 3) { result.error = "message pack header truncated"; return result; }
        uint8_t msg_size = bytes[1];
        uint8_t msg_count = bytes[2];
        if (msg_size != ODID_MESSAGE_SIZE) {
            result.error = "unexpected message size in pack header: " + std::to_string(msg_size);
            return result;
        }
        size_t needed = 3 + static_cast<size_t>(msg_count) * ODID_MESSAGE_SIZE;
        if (bytes.size() < needed) {
            result.error = "message pack truncated: need " + std::to_string(needed) + " bytes, have " + std::to_string(bytes.size());
            return result;
        }
        for (int i = 0; i < msg_count; ++i) {
            result.messages.push_back(decode_message(bytes.data() + 3 + i * ODID_MESSAGE_SIZE));
        }
    } else {
        result.messages.push_back(decode_message(bytes.data()));
    }
    result.ok = true;
    return result;
}

inline std::vector<uint8_t> hex_decode(const std::string& hex) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t i = 0;
    // Tolerate a couple of common separator styles ("aa:bb:cc", "aa bb cc")
    // so callers don't have to pre-clean output copy-pasted from a sniffer.
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) if (nibble(c) >= 0) clean += c;
    if (clean.size() % 2 != 0) return out; // caller checks out.empty() for "invalid"
    for (i = 0; i < clean.size(); i += 2) {
        int hi = nibble(clean[i]), lo = nibble(clean[i + 1]);
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace remoteid
