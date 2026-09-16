#pragma once

#include <ctime>

namespace services::timesync {

/** Loads the saved POSIX TZ string and applies it. Call once at boot. */
void init();
/** True once the clock has been set from the ADS-B feed. */
bool hasTime();
/** Feeds the "now" field of an ADS-B response (Unix seconds or milliseconds). */
void setFromFeedTimestamp(double value);
/** Local broken-down time; false while the clock is still unset. */
bool localTime(struct tm* out);
const char* posixTz();
void saveTimezoneFromPortal(const char* tz);
/** Restore the default timezone (e.g. with a WiFi credential wipe). */
void reset();

}  // namespace services::timesync
