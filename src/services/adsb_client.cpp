#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/time_sync.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr float kMetersPerFoot = 0.3048f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

// Allocated on first use so the display frame buffer gets the large contiguous
// DRAM block first. The task only ever writes s_staging, the UI only ever reads
// s_aircraft, and the two are swapped by consumeUpdate() between frames.
Aircraft* s_aircraft = nullptr;
Aircraft* s_staging = nullptr;
size_t s_aircraft_count = 0;
size_t s_staging_count = 0;

enum class FetchState : uint8_t { kIdle, kBusy, kReady };

volatile FetchState s_state = FetchState::kIdle;
volatile bool s_fetch_ok = false;
TaskHandle_t s_task = nullptr;
double s_request_lat = 0.0;
double s_request_lon = 0.0;
float s_request_radius_km = 0.0f;

constexpr uint32_t kTaskStackWords = 12288;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;  // core 1 runs loop() and the display SPI

bool ensureAircraftTables() {
  if (s_aircraft == nullptr) {
    s_aircraft = static_cast<Aircraft*>(calloc(kMaxAircraft, sizeof(Aircraft)));
  }
  if (s_staging == nullptr) {
    s_staging = static_cast<Aircraft*>(calloc(kMaxAircraft, sizeof(Aircraft)));
  }
  return s_aircraft != nullptr && s_staging != nullptr;
}

int performGet(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBody(HTTPClient& http, String& payload) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    if (ui::radar::altitudeMeters()) {
      snprintf(out, out_len, "%d m", static_cast<int>(lroundf(alt * kMetersPerFoot)));
    } else {
      snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
    }
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));

  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

bool runFetch(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.useHTTP10(true);
  http.setTimeout(kRequestTimeoutMs);
  const int code = performGet(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readResponseBody(http, payload)) {
    Serial.println("adsb: empty response");
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  // The snapshot carries the server time, which is the only clock this device has.
  const double feed_now = doc["now"].as<double>();
  if (feed_now > 0.0) {
    services::timesync::setFromFeedTimestamp(feed_now);
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_staging_count = 0;
    return true;
  }

  if (!ensureAircraftTables()) {
    Serial.println("adsb: aircraft table alloc failed");
    return false;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_staging[n].lat = plane["lat"].as<float>();
    s_staging[n].lon = plane["lon"].as<float>();
    s_staging[n].nose_deg = pickNoseHeading(plane);
    s_staging[n].track_deg = pickTrackHeading(plane);
    s_staging[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_staging[n], plane);
    ++n;
  }

  s_staging_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

void fetchTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_fetch_ok = runFetch(s_request_lat, s_request_lon, s_request_radius_km);
    s_state = FetchState::kReady;
  }
}

}  // namespace

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

void begin() {
  if (s_task != nullptr || !ensureAircraftTables()) {
    return;
  }
  // Pinned to core 0: the TLS handshake blocks for a while and loop() must keep
  // animating on core 1.
  xTaskCreatePinnedToCore(fetchTask, "adsb", kTaskStackWords, nullptr, kTaskPriority,
                          &s_task, kTaskCore);
}

bool requestUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  if (s_task == nullptr || s_state != FetchState::kIdle) {
    return false;
  }
  s_request_lat = center_lat;
  s_request_lon = center_lon;
  s_request_radius_km = fetch_radius_km;
  s_state = FetchState::kBusy;
  xTaskNotifyGive(s_task);
  return true;
}

bool consumeUpdate() {
  if (s_state != FetchState::kReady) {
    return false;
  }
  const bool ok = s_fetch_ok;
  if (ok) {
    Aircraft* const published = s_aircraft;
    s_aircraft = s_staging;
    s_aircraft_count = s_staging_count;
    s_staging = published;
  }
  s_state = FetchState::kIdle;
  return ok;
}

}  // namespace services::adsb
