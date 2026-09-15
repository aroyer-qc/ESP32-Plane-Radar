#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";
constexpr char kPrefsBackupSsidKey[] = "ssid2";
constexpr char kPrefsBackupPassKey[] = "pass2";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

constexpr int kSsidParamLen = 32;
constexpr int kPassParamLen = 64;
constexpr char kBackupSsidAttrs[] = " maxlength=\"32\"";
constexpr char kBackupPassAttrs[] =
    " type=\"password\" maxlength=\"63\" placeholder=\"unchanged\"";

WiFiManagerParameter s_param_backup_ssid("wifi2_ssid", "Backup WiFi SSID (optional)",
                                         "", kSsidParamLen, kBackupSsidAttrs);
WiFiManagerParameter s_param_backup_pass("wifi2_pass", "Backup WiFi password", "",
                                         kPassParamLen, kBackupPassAttrs);
WiFiManagerParameter s_param_backup_lat("radar_lat2", "Backup latitude (deg)", "",
                                        kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_backup_lon("radar_lon2", "Backup longitude (deg)", "",
                                        kCoordParamLen, kCoordInputAttrs);

// Hooks the scan-list click handler so picking a network asks which slot it goes to.
constexpr char kBackupSectionHtml[] =
    "<hr><h3>Backup network</h3>"
    "<p>Used when the main network is unreachable.</p>"
    "<script>(function(){var o=window.c;window.c=function(l){"
    "var n=l.getAttribute('data-ssid')||l.innerText||l.textContent;"
    "var a=document.querySelector(\"input[name='wifi2_ssid']\");"
    "if(a&&confirm('Use '+n+' as the BACKUP network?\\n\\nOK = backup   Cancel = main')){"
    "a.value=n;var s=document.getElementById('s');if(s)s.value='';"
    "var p=document.getElementById('p');if(p){p.value='';p.disabled=true;}"
    "var b=document.querySelector(\"input[name='wifi2_pass']\");"
    "if(b){b.value='';b.focus();}return;}"
    "if(o)o(l);};})();</script>";
constexpr char kDisplaySectionHtml[] = "<hr><h3>Display</h3>";

WiFiManagerParameter s_param_backup_header(kBackupSectionHtml);
WiFiManagerParameter s_param_display_header(kDisplaySectionHtml);

/** True while the radar is connected through the backup network. */
bool s_on_backup_network = false;

String backupSsid() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return String();
  }
  const String ssid = prefs.getString(kPrefsBackupSsidKey, "");
  prefs.end();
  return ssid;
}

String backupPass() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return String();
  }
  const String pass = prefs.getString(kPrefsBackupPassKey, "");
  prefs.end();
  return pass;
}

void saveBackupCredentials(const String& ssid, const String& pass) {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsBackupSsidKey, ssid);
  prefs.putString(kPrefsBackupPassKey, pass);
  prefs.end();
}

void clearBackupCredentials() {
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.remove(kPrefsBackupSsidKey);
  prefs.remove(kPrefsBackupPassKey);
  prefs.end();
}

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::primaryLat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::primaryLon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
  const String ssid2 = backupSsid();
  s_param_backup_ssid.setValue(ssid2.c_str(), kSsidParamLen);
  // Never echo the stored password back to the page.
  s_param_backup_pass.setValue("", kPassParamLen);
  if (services::location::hasSecondary()) {
    snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::secondaryLat());
    snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::secondaryLon());
    s_param_backup_lat.setValue(lat_buf, kCoordParamLen);
    s_param_backup_lon.setValue(lon_buf, kCoordParamLen);
  } else {
    s_param_backup_lat.setValue("", kCoordParamLen);
    s_param_backup_lon.setValue("", kCoordParamLen);
  }
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());

  const char* ssid2 = s_param_backup_ssid.getValue();
  const char* pass2 = s_param_backup_pass.getValue();
  if (ssid2 == nullptr || ssid2[0] == '\0') {
    clearBackupCredentials();
    Serial.println("Backup WiFi cleared");
  } else {
    // An empty password field means "keep the stored one".
    const String pass = (pass2 != nullptr && pass2[0] != '\0') ? String(pass2)
                                                              : backupPass();
    saveBackupCredentials(String(ssid2), pass);
    Serial.printf("Backup WiFi saved: %s\n", ssid2);
  }

  if (!services::location::saveSecondaryFromStrings(s_param_backup_lat.getValue(),
                                                    s_param_backup_lon.getValue())) {
    Serial.println("Invalid backup lat/lon in portal — keeping previous location");
  }
  services::location::useSecondary(s_on_backup_network);
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_backup_header);
  wm.addParameter(&s_param_backup_ssid);
  wm.addParameter(&s_param_backup_pass);
  wm.addParameter(&s_param_backup_lat);
  wm.addParameter(&s_param_backup_lon);
  wm.addParameter(&s_param_display_header);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  clearBackupCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass, bool persist_credentials) {
  prepareSta();
  WiFi.persistent(persist_credentials);
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
  WiFi.persistent(true);
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui,
                      bool persist_credentials) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass, persist_credentials);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  return tryConnectWithUi(ssid, pass, show_ui, true);
}

bool connectBackupNetwork(bool show_ui) {
  const String ssid = backupSsid();
  if (ssid.length() == 0) {
    return false;
  }
  Serial.printf("Primary WiFi failed — trying backup: %s\n", ssid.c_str());
  // Keep the primary credentials in NVS: do not persist the backup ones.
  if (!tryConnectWithUi(ssid, backupPass(), show_ui, false)) {
    return false;
  }
  s_on_backup_network = true;
  services::location::useSecondary(true);
  return true;
}

bool connectKnownNetworks(bool show_ui) {
  if (connectSavedNetwork(show_ui)) {
    s_on_backup_network = false;
    services::location::useSecondary(false);
    return true;
  }
  return connectBackupNetwork(show_ui);
}

bool hasAnyCredentials() { return storedWifiCredentials() || backupSsid().length() > 0; }

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectKnownNetworks(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (hasAnyCredentials() && connectKnownNetworks(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (hasAnyCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
