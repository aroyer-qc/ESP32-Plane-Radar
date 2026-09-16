#pragma once

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Active center: secondary while the secondary network is in use, primary otherwise. */
double lat();
double lon();

/** Primary center, regardless of the active selection. */
double primaryLat();
double primaryLon();

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Secondary center, used while the Secondary network is connected. */
bool hasSecondary();
double secondaryLat();
double secondaryLon();
/** Empty strings clear the secondary center. */
bool saveSecondaryFromStrings(const char* lat_str, const char* lon_str);
/** Pick which stored center feeds lat()/lon(); runtime only, not persisted. */
void useSecondary(bool enabled);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
