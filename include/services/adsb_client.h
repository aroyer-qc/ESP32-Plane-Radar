#pragma once

#include <cstddef>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /** ICAO address; stable identity across polls (feed order is not). */
  char hex[8];
  char callsign[9];
  char type[5];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Starts the background fetch task. Call once after boot. */
void begin();
/** Queues a fetch of the aircraft within fetch_radius_km; false if one is in flight. */
bool requestUpdate(double center_lat, double center_lon, float fetch_radius_km);
/** Publishes a finished fetch; true when the aircraft list changed. */
bool consumeUpdate();

}  // namespace services::adsb
