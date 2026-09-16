#include "services/time_sync.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace services::timesync {

namespace {

constexpr char kPrefsNamespace[] = "clock";
constexpr char kPrefsTzKey[] = "tz";
/** Eastern time with the US/Canada DST rules. */
constexpr char kDefaultTz[] = "EST5EDT,M3.2.0,M11.1.0";
/** Feed timestamps older than 2020-09-13 are bogus. */
constexpr double kMinValidEpoch = 1600000000.0;
/** Above this the feed reports milliseconds, not seconds. */
constexpr double kMillisecondThreshold = 1.0e12;
/** Only re-arm the clock once the drift gets this large. */
constexpr double kMaxDriftSec = 2.0;

char s_tz[64] = "";
bool s_have_time = false;

void applyTz() {
  setenv("TZ", s_tz, 1);
  tzset();
}

void setTz(const char* tz) {
  const char* value = (tz != nullptr && tz[0] != '\0') ? tz : kDefaultTz;
  strncpy(s_tz, value, sizeof(s_tz) - 1);
  s_tz[sizeof(s_tz) - 1] = '\0';
  applyTz();
}

}  // namespace

void init() {
  Preferences prefs;
  String tz(kDefaultTz);
  if (prefs.begin(kPrefsNamespace, true)) {
    tz = prefs.getString(kPrefsTzKey, kDefaultTz);
    prefs.end();
  }
  setTz(tz.c_str());
}

bool hasTime() { return s_have_time; }

void setFromFeedTimestamp(double value) {
  const double seconds =
      (value >= kMillisecondThreshold) ? (value / 1000.0) : value;
  if (seconds < kMinValidEpoch) {
    return;
  }
  const double current = static_cast<double>(time(nullptr));
  if (s_have_time && fabs(current - seconds) < kMaxDriftSec) {
    return;
  }

  timeval tv = {};
  tv.tv_sec = static_cast<time_t>(seconds);
  tv.tv_usec = static_cast<suseconds_t>((seconds - floor(seconds)) * 1.0e6);
  settimeofday(&tv, nullptr);

  const bool first = !s_have_time;
  s_have_time = true;
  if (first) {
    struct tm local = {};
    if (localTime(&local)) {
      Serial.printf("Clock set from ADS-B feed: %04d-%02d-%02d %02d:%02d:%02d %s\n",
                    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                    local.tm_hour, local.tm_min, local.tm_sec, s_tz);
    }
  }
}

bool localTime(struct tm* out) {
  if (out == nullptr || !s_have_time) {
    return false;
  }
  const time_t now = time(nullptr);
  localtime_r(&now, out);
  return true;
}

const char* posixTz() { return s_tz; }

void saveTimezoneFromPortal(const char* tz) {
  setTz(tz);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsTzKey, s_tz);
    prefs.end();
  }
  Serial.printf("Timezone: %s\n", s_tz);
}

void reset() {
  setTz(kDefaultTz);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kPrefsTzKey);
    prefs.end();
  }
}

}  // namespace services::timesync
