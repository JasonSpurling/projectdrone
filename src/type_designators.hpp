// type_designators.hpp -- ICAO aircraft type designator (DOC 8643) lookup,
// e.g. "B738" -> Boeing 737-800, "H60" -> Sikorsky UH-60 Black Hawk.
//
// Honest scope: DOC 8643 has ~3,000 entries; this is a curated subset of
// commercial airliners, common general-aviation types, and common
// helicopters -- the types actually likely to be seen or looked up
// through this project, not an exhaustive reproduction of the full ICAO
// document. Same disclosed-scope pattern as manufacturer_registry.hpp.
// Returns not-found rather than guessing for anything outside this list.
#pragma once
#include <string>
#include <unordered_map>
#include <algorithm>
#include <optional>

struct TypeDesignatorEntry { const char* manufacturer; const char* model; bool is_helicopter; };

inline const std::unordered_map<std::string, TypeDesignatorEntry>& type_designator_table() {
    static const std::unordered_map<std::string, TypeDesignatorEntry> table = {
        // Boeing
        {"B734", {"Boeing", "737-400", false}}, {"B735", {"Boeing", "737-500", false}},
        {"B736", {"Boeing", "737-600", false}}, {"B737", {"Boeing", "737-700", false}},
        {"B738", {"Boeing", "737-800", false}}, {"B739", {"Boeing", "737-900", false}},
        {"B37M", {"Boeing", "737 MAX 7", false}}, {"B38M", {"Boeing", "737 MAX 8", false}},
        {"B39M", {"Boeing", "737 MAX 9", false}}, {"B744", {"Boeing", "747-400", false}},
        {"B748", {"Boeing", "747-8", false}}, {"B752", {"Boeing", "757-200", false}},
        {"B753", {"Boeing", "757-300", false}}, {"B762", {"Boeing", "767-200", false}},
        {"B763", {"Boeing", "767-300", false}}, {"B764", {"Boeing", "767-400", false}},
        {"B772", {"Boeing", "777-200", false}}, {"B773", {"Boeing", "777-300", false}},
        {"B77L", {"Boeing", "777-200LR", false}}, {"B77W", {"Boeing", "777-300ER", false}},
        {"B788", {"Boeing", "787-8", false}}, {"B789", {"Boeing", "787-9", false}},
        {"B78X", {"Boeing", "787-10", false}},
        // Airbus
        {"A318", {"Airbus", "A318", false}}, {"A319", {"Airbus", "A319", false}},
        {"A320", {"Airbus", "A320", false}}, {"A321", {"Airbus", "A321", false}},
        {"A20N", {"Airbus", "A320neo", false}}, {"A21N", {"Airbus", "A321neo", false}},
        {"A332", {"Airbus", "A330-200", false}}, {"A333", {"Airbus", "A330-300", false}},
        {"A339", {"Airbus", "A330-900neo", false}}, {"A342", {"Airbus", "A340-200", false}},
        {"A343", {"Airbus", "A340-300", false}}, {"A345", {"Airbus", "A340-500", false}},
        {"A346", {"Airbus", "A340-600", false}}, {"A359", {"Airbus", "A350-900", false}},
        {"A35K", {"Airbus", "A350-1000", false}}, {"A388", {"Airbus", "A380-800", false}},
        // Regional jets / turboprops
        {"CRJ2", {"Bombardier", "CRJ200", false}}, {"CRJ7", {"Bombardier", "CRJ700", false}},
        {"CRJ9", {"Bombardier", "CRJ900", false}}, {"CRJX", {"Bombardier", "CRJ1000", false}},
        {"E135", {"Embraer", "ERJ135", false}}, {"E145", {"Embraer", "ERJ145", false}},
        {"E170", {"Embraer", "E170", false}}, {"E175", {"Embraer", "E175", false}},
        {"E190", {"Embraer", "E190", false}}, {"E195", {"Embraer", "E195", false}},
        {"DH8D", {"Bombardier", "Dash 8 Q400", false}}, {"AT72", {"ATR", "72", false}},
        {"AT76", {"ATR", "72-600", false}},
        // General aviation (fixed-wing)
        {"C172", {"Cessna", "172 Skyhawk", false}}, {"C182", {"Cessna", "182 Skylane", false}},
        {"C208", {"Cessna", "208 Caravan", false}}, {"C525", {"Cessna", "CitationJet", false}},
        {"C56X", {"Cessna", "Citation Excel", false}}, {"PA28", {"Piper", "PA-28 Cherokee/Warrior/Archer", false}},
        {"PA32", {"Piper", "PA-32 Saratoga/Cherokee Six", false}}, {"SR20", {"Cirrus", "SR20", false}},
        {"SR22", {"Cirrus", "SR22", false}}, {"BE20", {"Beechcraft", "King Air 200", false}},
        {"BE9L", {"Beechcraft", "King Air 90", false}}, {"GLF5", {"Gulfstream", "G550", false}},
        {"GLF6", {"Gulfstream", "G650", false}}, {"G280", {"Gulfstream", "G280", false}},
        {"LJ45", {"Learjet", "45", false}}, {"F900", {"Dassault", "Falcon 900", false}},
        {"DA40", {"Diamond", "DA40", false}}, {"DA42", {"Diamond", "DA42 Twin Star", false}},
        // Helicopters
        {"H60", {"Sikorsky", "UH-60 Black Hawk", true}}, {"S76", {"Sikorsky", "S-76", true}},
        {"S92", {"Sikorsky", "S-92", true}}, {"EC35", {"Airbus (Eurocopter)", "EC135", true}},
        {"EC45", {"Airbus (Eurocopter)", "EC145", true}}, {"EC30", {"Airbus (Eurocopter)", "EC130", true}},
        {"AS50", {"Airbus (Eurocopter)", "AS350 Ecureuil", true}}, {"AS65", {"Airbus (Eurocopter)", "AS365 Dauphin", true}},
        {"A139", {"Leonardo", "AW139", true}}, {"A109", {"Leonardo", "A109", true}},
        {"B06", {"Bell", "206 JetRanger/LongRanger", true}}, {"B407", {"Bell", "407", true}},
        {"B412", {"Bell", "412", true}}, {"B429", {"Bell", "429", true}},
        {"R44", {"Robinson", "R44", true}}, {"R66", {"Robinson", "R66", true}},
        {"R22", {"Robinson", "R22", true}}, {"H500", {"MD Helicopters", "MD 500", true}},
        {"MD90", {"MD Helicopters", "MD 900 Explorer", true}}, {"CH47", {"Boeing", "CH-47 Chinook", true}},
        {"AH64", {"Boeing", "AH-64 Apache", true}}, {"H64", {"Sikorsky", "S-64/CH-54 Skycrane", true}},
    };
    return table;
}

inline std::optional<TypeDesignatorEntry> lookup_type_designator(std::string code) {
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::toupper(c); });
    auto& table = type_designator_table();
    auto it = table.find(code);
    if (it == table.end()) return std::nullopt;
    return it->second;
}
