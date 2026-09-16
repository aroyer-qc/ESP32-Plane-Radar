#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

enum class NightStyle : uint8_t {
  kNone,  // daytime palette
  kDim,   // daytime hues at 25%
  kRed,   // red matrix palette
};

/** Load the schedule and style from flash. Call once after boot. */
void nightInit();
/** Style for the current local time; kNone while the clock is unset. */
NightStyle nightStyle();
bool nightDimEnabled();
bool nightRedEnabled();
uint16_t nightStartMinutes();
uint16_t nightEndMinutes();
/** "HH:MM", for the portal <input type="time"> fields. */
void formatNightTime(char* buf, size_t len, uint16_t minutes);
void saveNightFromPortal(const char* start, const char* end, const char* dim_value,
                         const char* red_value);
/** Back to defaults (e.g. with a WiFi credential wipe). */
void nightReset();

}  // namespace ui::radar
