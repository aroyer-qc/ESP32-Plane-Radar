/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/time_sync.h"
#include "services/wifi_setup.h"
#include "ui/night_mode.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

/** Night mode flips on a minute boundary; no need to poll it every loop. */
constexpr unsigned long kNightCheckIntervalMs = 1000;

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_night_check_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void serviceAircraftFeed() {
  if (services::adsb::consumeUpdate()) {
    ui::radarDisplayRefreshAircraft();
  }
  if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
    g_last_adsb_fetch_ms = millis();
    services::adsb::requestUpdate(services::location::lat(),
                                  services::location::lon(),
                                  ui::radar::fetchRadiusKm());
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  ui::radarDisplayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::timesync::init();
  ui::radar::nightInit();
  services::adsb::begin();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (millis() - g_last_night_check_ms >= kNightCheckIntervalMs) {
    g_last_night_check_ms = millis();
    if (g_radar_visible && ui::radarDisplayNightStyleChanged()) {
      ui::radarDisplayDraw();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else {
      serviceAircraftFeed();
      ui::radarDisplayAnimate();
    }
  }

  delay(10);
}
