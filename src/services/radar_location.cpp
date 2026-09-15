#include "services/radar_location.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyLat2[] = "lat2";
constexpr char kKeyLon2[] = "lon2";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
double s_lat2 = config::kDefaultRadarLat;
double s_lon2 = config::kDefaultRadarLon;
bool s_has_secondary = false;
bool s_use_secondary = false;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void persist(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.end();
  s_lat = lat;
  s_lon = lon;
}

void persistSecondary(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat2, lat);
  prefs.putDouble(kKeyLon2, lon);
  prefs.end();
  s_lat2 = lat;
  s_lon2 = lon;
  s_has_secondary = true;
}

void forgetSecondary() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat2);
  prefs.remove(kKeyLon2);
  prefs.end();
  s_lat2 = config::kDefaultRadarLat;
  s_lon2 = config::kDefaultRadarLon;
  s_has_secondary = false;
}

bool blank(const char* text) {
  if (text == nullptr) {
    return true;
  }
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p != ' ' && *p != '\t') {
      return false;
    }
  }
  return true;
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  if (prefs.isKey(kKeyLat2) && prefs.isKey(kKeyLon2)) {
    const double lat = prefs.getDouble(kKeyLat2, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon2, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat2 = lat;
      s_lon2 = lon;
      s_has_secondary = true;
    }
  }
  prefs.end();
}

double lat() { return (s_use_secondary && s_has_secondary) ? s_lat2 : s_lat; }

double lon() { return (s_use_secondary && s_has_secondary) ? s_lon2 : s_lon; }

double primaryLat() { return s_lat; }

double primaryLon() { return s_lon; }

bool hasSecondary() { return s_has_secondary; }

double secondaryLat() { return s_lat2; }

double secondaryLon() { return s_lon2; }

void useSecondary(bool enabled) {
  if (s_use_secondary == enabled) {
    return;
  }
  s_use_secondary = enabled;
  Serial.printf("Radar center: %s (%.6f, %.6f)\n",
                (enabled && s_has_secondary) ? "backup" : "primary", lat(), lon());
}

bool saveSecondaryFromStrings(const char* lat_str, const char* lon_str) {
  if (blank(lat_str) && blank(lon_str)) {
    forgetSecondary();
    return true;
  }
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persistSecondary(lat, lon);
  Serial.printf("Backup radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persist(lat, lon);
  Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyLat2);
  prefs.remove(kKeyLon2);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_lat2 = config::kDefaultRadarLat;
  s_lon2 = config::kDefaultRadarLon;
  s_has_secondary = false;
  s_use_secondary = false;
}

}  // namespace services::location
