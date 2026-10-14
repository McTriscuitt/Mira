#pragma once
//
// lights.h — the light roster. Index order is used everywhere: light masks, the
// bridge cache, the echo ring, persisted state. UUIDs live in bridge.cpp (from
// secrets.h) in this same order. Pure header — no Arduino dependencies.
//
#include <stdint.h>

enum LightIdx : uint8_t {
    LIGHT_FLOOR = 0,  // Signe gradient floor lamp — leads the wake, last light out
    LIGHT_DRESSER,
    LIGHT_CHEST,
    LIGHT_CEIL_1,
    // LIGHT_CEIL_2,  // second ceiling bulb — unused since 2.x. To re-enable: uncomment
    //                // here and its row in bridge.cpp's LIGHT_UUIDS (roleOf already maps it).
    LIGHT_COUNT
};

// What a light does in the day (MIRA_3.md §2). The timeline speaks in roles, so a
// second ceiling bulb would follow the first with no other changes.
enum class Role : uint8_t { Floor, Dresser, Chest, Ceiling };

typedef uint8_t LightMask;  // one bit per LightIdx

inline LightMask lightBit(int idx) { return (LightMask)(1u << idx); }
const LightMask ALL_LIGHTS = (LightMask)((1u << LIGHT_COUNT) - 1);

inline Role roleOf(int idx) {
    switch (idx) {
        case LIGHT_FLOOR:   return Role::Floor;
        case LIGHT_DRESSER: return Role::Dresser;
        case LIGHT_CHEST:   return Role::Chest;
        default:            return Role::Ceiling;  // LIGHT_CEIL_1 (and _2 if re-enabled)
    }
}

inline const char* lightName(int idx) {
    switch (idx) {
        case LIGHT_FLOOR:   return "Floor";
        case LIGHT_DRESSER: return "Dresser";
        case LIGHT_CHEST:   return "Chest";
        case LIGHT_CEIL_1:  return "Ceiling";
        default:            return "Light?";
    }
}
