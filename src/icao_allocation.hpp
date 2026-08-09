// icao_allocation.hpp -- ICAO 24-bit aircraft address ("Mode S"/icao24)
// national allocation blocks, per ICAO Annex 10 Volume III. This is a
// static table, not a live feed/download -- unlike the FAA registry or
// airports data, national address-block assignments are a fixed part of
// the ICAO standard and essentially never change.
//
// Honest scope: this covers the major aviation nations only (the ones
// this project's live feeds are most likely to actually see traffic
// from), not the complete ICAO allocation table, which has 190+ entries
// including many single-country micro-blocks. Best-effort against public
// references, not independently verified against the current ICAO
// document -- same disclosed-scope pattern as the emitter-category tables
// in community_adsb_feed.hpp/opensky_feed.hpp. Returns "unknown" rather
// than guessing when an icao24 falls outside a covered block.
#pragma once
#include <string>
#include <cstdint>
#include <cctype>
#include <vector>

struct IcaoAllocationBlock { uint32_t start, end; const char* country; };

// Ranges are inclusive, upper bound of each block. Ordered roughly by how
// often this project's North America-centric live feeds would see them.
inline const std::vector<IcaoAllocationBlock>& icao_allocation_table() {
    static const std::vector<IcaoAllocationBlock> table = {
        {0xA00000, 0xAFFFFF, "United States"},
        {0xC00000, 0xC3FFFF, "Canada"},
        {0xC80000, 0xC87FFF, "New Zealand"},
        {0x0D0000, 0x0DFFFF, "Mexico"},
        {0xE00000, 0xE3FFFF, "Brazil"},
        {0x400000, 0x43FFFF, "United Kingdom"},
        {0x380000, 0x3BFFFF, "France"},
        {0x3C0000, 0x3FFFFF, "Germany"},
        {0x300000, 0x33FFFF, "Italy"},
        {0x340000, 0x37FFFF, "Spain"},
        {0x480000, 0x483FFF, "Netherlands"},
        {0x4B0000, 0x4B7FFF, "Switzerland"},
        {0x780000, 0x7BFFFF, "China"},
        {0x840000, 0x87FFFF, "Japan"},
        {0x7C0000, 0x7FFFFF, "Australia"},
        {0x800000, 0x83FFFF, "India"},
    };
    return table;
}

// Best-effort country-of-registration guess from a live icao24 address.
// Returns nullptr if the address doesn't fall in a covered block (see
// file header on scope) or fails to parse as hex.
inline const char* icao_allocation_country(const std::string& icao24_hex) {
    if (icao24_hex.empty() || icao24_hex.size() > 6) return nullptr;
    for (char c : icao24_hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return nullptr;
    }
    uint32_t addr = 0;
    try { addr = static_cast<uint32_t>(std::stoul(icao24_hex, nullptr, 16)); }
    catch (...) { return nullptr; }

    for (auto& block : icao_allocation_table()) {
        if (addr >= block.start && addr <= block.end) return block.country;
    }
    return nullptr;
}
