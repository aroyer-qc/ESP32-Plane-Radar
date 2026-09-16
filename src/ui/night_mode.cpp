#include "ui/night_mode.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstdio>

#include "services/time_sync.h"
#include "ui/radar_range.h"

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "night";
constexpr char kPrefsStartKey[] = "start";
constexpr char kPrefsEndKey[] = "end";
constexpr char kPrefsDimKey[] = "dim";
constexpr char kPrefsRedKey[] = "red";

constexpr uint16_t kMinutesPerDay = 24 * 60;
constexpr uint16_t kDefaultStartMinutes = 22 * 60;
constexpr uint16_t kDefaultEndMinutes = 7 * 60;

uint16_t s_start_minutes = kDefaultStartMinutes;
uint16_t s_end_minutes = kDefaultEndMinutes;
bool s_dim = false;
bool s_red = false;

bool parseHhMm(const char* text, uint16_t* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  int hours = 0;
  int minutes = 0;
  const int fields = sscanf(text, "%d:%d", &hours, &minutes);
  if (fields < 1 || hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
    return false;
  }
  *out = static_cast<uint16_t>(hours * 60 + (fields >= 2 ? minutes : 0));
  return true;
}

void saveSettings() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putUShort(kPrefsStartKey, s_start_minutes);
  prefs.putUShort(kPrefsEndKey, s_end_minutes);
  prefs.putBool(kPrefsDimKey, s_dim);
  prefs.putBool(kPrefsRedKey, s_red);
  prefs.end();
}

/** Window may wrap past midnight (e.g. 22:00 → 07:00). */
bool withinNightWindow() {
  struct tm local = {};
  if (!services::timesync::localTime(&local)) {
    return false;
  }
  if (s_start_minutes == s_end_minutes) {
    return false;
  }
  const uint16_t now =
      static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min) % kMinutesPerDay;
  if (s_start_minutes < s_end_minutes) {
    return now >= s_start_minutes && now < s_end_minutes;
  }
  return now >= s_start_minutes || now < s_end_minutes;
}

}  // namespace

void nightInit() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  s_start_minutes = prefs.getUShort(kPrefsStartKey, kDefaultStartMinutes) % kMinutesPerDay;
  s_end_minutes = prefs.getUShort(kPrefsEndKey, kDefaultEndMinutes) % kMinutesPerDay;
  s_dim = prefs.getBool(kPrefsDimKey, false);
  s_red = prefs.getBool(kPrefsRedKey, false);
  prefs.end();
}

NightStyle nightStyle() {
  if (!s_dim && !s_red) {
    return NightStyle::kNone;
  }
  if (!withinNightWindow()) {
    return NightStyle::kNone;
  }
  return s_red ? NightStyle::kRed : NightStyle::kDim;
}

bool nightDimEnabled() { return s_dim; }

bool nightRedEnabled() { return s_red; }

uint16_t nightStartMinutes() { return s_start_minutes; }

uint16_t nightEndMinutes() { return s_end_minutes; }

void formatNightTime(char* buf, size_t len, uint16_t minutes) {
  if (buf == nullptr || len == 0) {
    return;
  }
  const uint16_t clamped = minutes % kMinutesPerDay;
  snprintf(buf, len, "%02u:%02u", clamped / 60, clamped % 60);
}

void saveNightFromPortal(const char* start, const char* end, const char* dim_value,
                         const char* red_value) {
  uint16_t parsed = 0;
  if (parseHhMm(start, &parsed)) {
    s_start_minutes = parsed;
  }
  if (parseHhMm(end, &parsed)) {
    s_end_minutes = parsed;
  }
  s_dim = portalCheckboxChecked(dim_value);
  s_red = portalCheckboxChecked(red_value);
  if (s_red) {
    s_dim = false;  // the two styles are mutually exclusive
  }
  saveSettings();

  char start_buf[6];
  char end_buf[6];
  formatNightTime(start_buf, sizeof(start_buf), s_start_minutes);
  formatNightTime(end_buf, sizeof(end_buf), s_end_minutes);
  Serial.printf("Night mode: %s-%s dim=%s red=%s\n", start_buf, end_buf,
                s_dim ? "on" : "off", s_red ? "on" : "off");
}

void nightReset() {
  s_start_minutes = kDefaultStartMinutes;
  s_end_minutes = kDefaultEndMinutes;
  s_dim = false;
  s_red = false;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

}  // namespace ui::radar
