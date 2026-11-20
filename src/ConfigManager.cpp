/**
 * ConfigManager.cpp
 *
 * Implementation of web-based configuration using WiFiManager and Preferences.
 */

#include "ConfigManager.h"

// Static pointer for callback (WiFiManager doesn't support member function
// callbacks)
static ConfigManager *configManagerInstance = nullptr;
static WiFiManager *currentWM = nullptr;

ConfigManager::ConfigManager()
    : configButtonPin(-1), paramLatitude(nullptr), paramLongitude(nullptr),
      paramLocationName(nullptr), paramCalendarUrl(nullptr),
      paramRefreshMinutes(nullptr), paramHourInterval(nullptr) {

  // Set default values
  config.latitude = "";
  config.longitude = "";
  config.locationName = "";
  config.calendarUrl = DEFAULT_CALENDAR_URL;
  config.refreshMinutes = DEFAULT_REFRESH_MINUTES;
  config.lowPowerMode = DEFAULT_LOW_POWER_MODE;
  config.hourInterval = DEFAULT_HOUR_INTERVAL;
  config.timeFormat = "24h";
  config.alertRegion = "auto";
  config.geoLatitude = "";
  config.geoLongitude = "";
  config.geoCity = "";
  config.geoCountryCode = "";
  config.geoSsid = "";

  configManagerInstance = this;
}

bool ConfigManager::begin(int buttonPin) {
  configButtonPin = buttonPin;

  // Setup button pin with internal pullup
  if (configButtonPin >= 0) {
    pinMode(configButtonPin, INPUT_PULLUP);
  }

  // Load existing configuration from NVS
  loadConfig();

  Serial.println("[ConfigManager] Configuration loaded");
  Serial.print("[ConfigManager] Location: ");
  Serial.println(config.locationName);

  // Check if config button is held - force config portal
  if (isConfigButtonHeld()) {
    Serial.println(
        "[ConfigManager] Config button held - starting config portal");
    return startConfigPortal("Manual Setup");
  }

  // Try to connect with saved credentials
  WiFiManager wm;
  wm.setDebugOutput(true);

  // Set timeout for connection attempts
  wm.setConnectTimeout(30);

  // Don't show config portal on connection failure (we'll handle it)
  wm.setEnableConfigPortal(false);

  // Try to connect
  bool connected = wm.autoConnect();

  if (!connected) {
    Serial.println("[ConfigManager] WiFi connection failed");

    // Start config portal when WiFi fails OR no configuration exists
    Serial.println("[ConfigManager] Starting config portal for setup...");

    // Include the SSID that was attempted in the reason
    String ssid = WiFi.SSID();
    String reason = "Opps! WiFi connection failed. Restarting router may help.";
    if (ssid.length() > 0) {
      reason = "WiFi connection failed: " + ssid;
    }
    return startConfigPortal(reason.c_str());
  }

  if (!isConfigured()) {
    Serial.println(
        "[ConfigManager] No manual or cached coordinates yet - will resolve via GeoJS");
  }

  Serial.println("[ConfigManager] WiFi connected!");
  Serial.print("[ConfigManager] SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("[ConfigManager] IP: ");
  Serial.println(WiFi.localIP());

  return true;
}

bool ConfigManager::startConfigPortal(const char *reason) {
  // Stack-allocated custom parameters (outlive WiFiManager wm below, avoiding
  // heap allocation and -Wdelete-non-virtual-dtor warnings).
  String geoHintHtml;
  if (config.hasGeoCache()) {
    geoHintHtml = "<p><small>GeoIP cached: ";
    if (config.geoCity.length() > 0) {
      geoHintHtml += config.geoCity;
      if (config.geoCountryCode.length() > 0) {
        geoHintHtml += ", " + config.geoCountryCode;
      }
      geoHintHtml += " ";
    }
    geoHintHtml += "(" + config.geoLatitude + ", " + config.geoLongitude +
                   "). Leave Lat/Lon blank to use GeoIP.</small></p>";
  } else {
    geoHintHtml =
        "<p><small>Leave Lat/Lon blank to auto-detect location via "
        "GeoJS.</small></p>";
  }
  WiFiManagerParameter geoHintParam(geoHintHtml.c_str());

  WiFiManagerParameter latitudeParam(
      "latitude", "Latitude (blank = auto GeoIP)", config.latitude.c_str(), 20);
  WiFiManagerParameter longitudeParam(
      "longitude", "Longitude (blank = auto GeoIP)", config.longitude.c_str(),
      20);
  WiFiManagerParameter locationNameParam(
      "location", "Location Name (blank = auto GeoIP)",
      config.locationName.c_str(), 32);
  WiFiManagerParameter calendarUrlParam(
      "calendar_url", "Google Script Calendar URL (optional)",
      config.calendarUrl.c_str(), 200);

  char refreshStr[8];
  snprintf(refreshStr, sizeof(refreshStr), "%d", config.refreshMinutes);
  WiFiManagerParameter refreshMinutesParam("refresh", "Refresh Minutes (1-60)",
                                           refreshStr, 4,
                                           "type='number' min='1' max='60'");

  char hourStr[4];
  snprintf(hourStr, sizeof(hourStr), "%d", config.hourInterval);
  WiFiManagerParameter hourIntervalParam(
      "hour_interval", "Forecast Hour Interval (1-6)", hourStr, 2,
      "type='number' min='1' max='6'");

  WiFiManagerParameter lowPowerModeParam(
      config.lowPowerMode
          ? "<br><label><input type='checkbox' name='low_power' value='1' "
            "checked> Low Power Mode</label>"
          : "<br><label><input type='checkbox' name='low_power' value='1'> Low "
            "Power Mode</label>");

  const char *unitsMetricSelected =
      config.units == "metric"
          ? "<br><label>Units</label><select name='units'><option "
            "value='metric' selected>Celsius</option><option "
            "value='imperial'>Fahrenheit</option></select>"
          : "<br><label>Units</label><select name='units'><option "
            "value='metric'>Celsius</option><option value='imperial' "
            "selected>Fahrenheit</option></select>";
  WiFiManagerParameter unitsParam(unitsMetricSelected);

  const char *timeFormatSelected =
      config.timeFormat == "12h"
          ? "<br><label>Time Format</label><select name='time_format'><option "
            "value='24h'>24H</option><option value='12h' selected>12H "
            "(AM/PM)</option></select>"
          : "<br><label>Time Format</label><select name='time_format'><option "
            "value='24h' selected>24H</option><option value='12h'>12H "
            "(AM/PM)</option></select>";
  WiFiManagerParameter timeFormatParam(timeFormatSelected);

  WiFiManagerParameter enableAlertsParam(
      config.enableAlertDisplay
          ? "<br><label><input type='checkbox' name='alerts' value='1' "
            "checked> Show Weather Alerts</label>"
          : "<br><label><input type='checkbox' name='alerts' value='1'> Show "
            "Weather Alerts</label>");

  String alertRegionHtml =
      "<br><label>Alert Source</label><select name='alert_region'>";
  const char *regionVals[] = {"auto", "US", "CA", "NO", "DE"};
  const char *regionLabels[] = {"Auto (by location)", "US (NWS)",
                                "Canada (ECCC)", "Norway (MET Norway)",
                                "Germany (Bright Sky / DWD)"};
  for (int i = 0; i < 5; i++) {
    alertRegionHtml += "<option value='";
    alertRegionHtml += regionVals[i];
    alertRegionHtml += "'";
    if (config.alertRegion == regionVals[i]) {
      alertRegionHtml += " selected";
    }
    alertRegionHtml += ">";
    alertRegionHtml += regionLabels[i];
    alertRegionHtml += "</option>";
  }
  alertRegionHtml += "</select>";
  WiFiManagerParameter alertRegionParam(alertRegionHtml.c_str());

  // Expose non-owning pointers for saveParamsCallback / readWiFiManagerParams
  paramLatitude = &latitudeParam;
  paramLongitude = &longitudeParam;
  paramLocationName = &locationNameParam;
  paramCalendarUrl = &calendarUrlParam;
  paramRefreshMinutes = &refreshMinutesParam;
  paramHourInterval = &hourIntervalParam;

  WiFiManager wm;
  currentWM = &wm; // Store pointer for callback access

  wm.addParameter(&geoHintParam);
  wm.addParameter(&latitudeParam);
  wm.addParameter(&longitudeParam);
  wm.addParameter(&locationNameParam);
  wm.addParameter(&calendarUrlParam);
  wm.addParameter(&refreshMinutesParam);
  wm.addParameter(&hourIntervalParam);
  wm.addParameter(&unitsParam);
  wm.addParameter(&timeFormatParam);
  wm.addParameter(&enableAlertsParam);
  wm.addParameter(&alertRegionParam);
  wm.addParameter(&lowPowerModeParam);

  wm.setSaveParamsCallback([]() {
    if (configManagerInstance && currentWM) {
      configManagerInstance->saveParamsCallback(*currentWM);
    }
  });

  // Configure portal
  wm.setDebugOutput(true);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
  wm.setBreakAfterConfig(true); // Exit after config even if no connection

  // Custom portal title
  wm.setTitle("Weather Crow Configuration");

  // Custom message after save - modify the "Saved" text
  wm.setCustomHeadElement("<script>"
                          "window.onload=function(){"
                          "var m=document.querySelector('.msg');"
                          "if(m && m.textContent=='Saved'){"
                          "m.innerHTML='Saved! Device is restarting...';"
                          "}"
                          "};"
                          "</script>");

  // Start the config portal (blocking)
  Serial.println("[ConfigManager] Starting config portal...");
  Serial.println("[ConfigManager] Connect to WiFi: 'WeatherCrow-Config'");
  Serial.println("[ConfigManager] Then navigate to: http://192.168.4.1");

  // Call the portal start callback to display on e-paper
  if (portalStartCallback) {
    portalStartCallback("WeatherCrow-Config", "192.168.4.1", reason);
  }

  bool result = wm.startConfigPortal("WeatherCrow-Config");

  if (result) {
    Serial.println("[ConfigManager] Portal completed - WiFi connected");

    // Read and save parameters
    readWiFiManagerParams(wm);
    saveConfig();

    Serial.println("[ConfigManager] Configuration saved");
  } else {
    Serial.println("[ConfigManager] Portal timeout or cancelled");
  }

  currentWM = nullptr;
  paramLatitude = nullptr;
  paramLongitude = nullptr;
  paramLocationName = nullptr;
  paramCalendarUrl = nullptr;
  paramRefreshMinutes = nullptr;
  paramHourInterval = nullptr;

  return result;
}

void ConfigManager::saveParamsCallback(WiFiManager &wm) {
  Serial.println("[ConfigManager] Save params callback triggered");
  readWiFiManagerParams(wm);
  saveConfig();
}

void ConfigManager::readWiFiManagerParams(WiFiManager &wm) {
  if (paramLatitude) {
    config.latitude = String(paramLatitude->getValue());
    config.latitude.trim();
  }

  if (paramLongitude) {
    config.longitude = String(paramLongitude->getValue());
    config.longitude.trim();
  }

  if (paramLocationName) {
    config.locationName = String(paramLocationName->getValue());
    config.locationName.trim();
  }

  if (paramCalendarUrl) {
    config.calendarUrl = String(paramCalendarUrl->getValue());
    config.calendarUrl.trim();
  }

  if (paramRefreshMinutes) {
    int val = String(paramRefreshMinutes->getValue()).toInt();
    config.refreshMinutes = constrain(val, 1, 60);
    if (val == 0)
      config.refreshMinutes = DEFAULT_REFRESH_MINUTES;
  }

  if (paramHourInterval) {
    int val = String(paramHourInterval->getValue()).toInt();
    config.hourInterval = constrain(val, 1, 6);
    if (val == 0)
      config.hourInterval = DEFAULT_HOUR_INTERVAL;
  }

  // Read custom HTML parameters via server args
  if (wm.server) {
    // Units dropdown
    if (wm.server->hasArg("units")) {
      String units = wm.server->arg("units");
      if (units == "metric" || units == "imperial") {
        config.units = units;
      }
    }

    // Time Format dropdown
    if (wm.server->hasArg("time_format")) {
      String fmt = wm.server->arg("time_format");
      if (fmt == "12h" || fmt == "24h") {
        config.timeFormat = fmt;
      }
    }

    // Low Power Mode checkbox
    config.lowPowerMode = wm.server->hasArg("low_power");

    // Enable Alerts checkbox
    config.enableAlertDisplay = wm.server->hasArg("alerts");

    // Regional Alert Source dropdown
    if (wm.server->hasArg("alert_region")) {
      String region = wm.server->arg("alert_region");
      if (region == "auto" || region == "US" || region == "CA" ||
          region == "NO" || region == "DE") {
        config.alertRegion = region;
      }
    }
  }

  Serial.println("[ConfigManager] Parameters read from portal:");
  Serial.print("  Latitude (manual): ");
  Serial.println(config.latitude.length() > 0 ? config.latitude : "(auto)");
  Serial.print("  Longitude (manual): ");
  Serial.println(config.longitude.length() > 0 ? config.longitude : "(auto)");
  Serial.print("  Location (manual): ");
  Serial.println(config.locationName.length() > 0 ? config.locationName
                                                  : "(auto)");
  Serial.print("  Calendar URL: ");
  Serial.println(config.calendarUrl.length() > 0 ? "(set)" : "(empty)");
  Serial.print("  Refresh: ");
  Serial.println(config.refreshMinutes);
  Serial.print("  Hour Interval: ");
  Serial.println(config.hourInterval);
  Serial.print("  Alert Region: ");
  Serial.println(config.alertRegion);
  Serial.print("  Low Power: ");
  Serial.println(config.lowPowerMode ? "true" : "false");
}

void ConfigManager::loadConfig() {
  preferences.begin(PREFS_NAMESPACE, true); // Read-only mode

  config.latitude = preferences.getString("latitude", "");
  config.longitude = preferences.getString("longitude", "");
  config.locationName = preferences.getString("location", "");
  config.calendarUrl = preferences.getString("cal_url", DEFAULT_CALENDAR_URL);
  if (config.calendarUrl.length() == 0) {
    config.calendarUrl = DEFAULT_CALENDAR_URL;
  }
  config.refreshMinutes =
      preferences.getInt("refresh", DEFAULT_REFRESH_MINUTES);
  config.lowPowerMode =
      preferences.getBool("low_power", DEFAULT_LOW_POWER_MODE);
  config.hourInterval = preferences.getInt("hour_int", DEFAULT_HOUR_INTERVAL);
  config.units = preferences.getString("units", "metric");
  config.timeFormat = preferences.getString("time_fmt", "24h");
  config.enableAlertDisplay = preferences.getBool("alerts", true);
  config.alertRegion = preferences.getString("alert_region", "auto");

  // Load cached GeoJS results
  config.geoLatitude = preferences.getString("geo_lat", "");
  config.geoLongitude = preferences.getString("geo_lon", "");
  config.geoCity = preferences.getString("geo_city", "");
  config.geoCountryCode = preferences.getString("geo_cc", "");
  config.geoSsid = preferences.getString("geo_ssid", "");

  preferences.end();

  Serial.println("[ConfigManager] Loaded config from NVS");
  Serial.print("  Latitude (manual): ");
  Serial.println(config.latitude.length() > 0 ? config.latitude : "(auto)");
  Serial.print("  Longitude (manual): ");
  Serial.println(config.longitude.length() > 0 ? config.longitude : "(auto)");
  Serial.print("  Location: ");
  Serial.println(getLocationName());
  if (config.hasGeoCache()) {
    Serial.print("  GeoIP cache: ");
    Serial.print(config.geoCity);
    Serial.print(", ");
    Serial.print(config.geoCountryCode);
    Serial.print(" (");
    Serial.print(config.geoLatitude);
    Serial.print(", ");
    Serial.print(config.geoLongitude);
    Serial.print(") [SSID: ");
    Serial.print(config.geoSsid);
    Serial.println("]");
  }
  Serial.print("  Calendar URL: ");
  Serial.println(config.calendarUrl.length() > 0 ? "(set)" : "(empty)");
  Serial.print("  Refresh: ");
  Serial.println(config.refreshMinutes);
  Serial.print("  Hour Interval: ");
  Serial.println(config.hourInterval);
  Serial.print("  Alert Region: ");
  Serial.println(config.alertRegion);
  Serial.print("  Low Power: ");
  Serial.println(config.lowPowerMode ? "true" : "false");
}

void ConfigManager::saveConfig() {
  preferences.begin(PREFS_NAMESPACE, false); // Read-write mode

  preferences.putString("latitude", config.latitude);
  preferences.putString("longitude", config.longitude);
  preferences.putString("location", config.locationName);
  preferences.putString("cal_url", config.calendarUrl);
  preferences.putInt("refresh", config.refreshMinutes);
  preferences.putBool("low_power", config.lowPowerMode);
  preferences.putInt("hour_int", config.hourInterval);
  preferences.putString("units", config.units);
  preferences.putString("time_fmt", config.timeFormat);
  preferences.putBool("alerts", config.enableAlertDisplay);
  preferences.putString("alert_region", config.alertRegion);

  preferences.end();

  Serial.println("[ConfigManager] Saved config to NVS");
}

void ConfigManager::saveGeoIpCache(const String &lat, const String &lon,
                                   const String &city,
                                   const String &countryCode,
                                   const String &ssid) {
  config.geoLatitude = lat;
  config.geoLongitude = lon;
  config.geoCity = city;
  config.geoCountryCode = countryCode;
  config.geoSsid = ssid;

  preferences.begin(PREFS_NAMESPACE, false); // Read-write mode
  preferences.putString("geo_lat", config.geoLatitude);
  preferences.putString("geo_lon", config.geoLongitude);
  preferences.putString("geo_city", config.geoCity);
  preferences.putString("geo_cc", config.geoCountryCode);
  preferences.putString("geo_ssid", config.geoSsid);
  preferences.end();

  Serial.println("[ConfigManager] Saved GeoJS cache to NVS");
}

void ConfigManager::resetConfig() {
  // Clear NVS
  preferences.begin(PREFS_NAMESPACE, false);
  preferences.clear();
  preferences.end();

  // Clear WiFi credentials
  WiFiManager wm;
  wm.resetSettings();

  // Reset to defaults
  config.latitude = "";
  config.longitude = "";
  config.locationName = "";
  config.calendarUrl = "";
  config.refreshMinutes = DEFAULT_REFRESH_MINUTES;
  config.lowPowerMode = DEFAULT_LOW_POWER_MODE;
  config.hourInterval = DEFAULT_HOUR_INTERVAL;
  config.units = "metric";
  config.timeFormat = "24h";
  config.enableAlertDisplay = true;
  config.alertRegion = "auto";
  config.geoLatitude = "";
  config.geoLongitude = "";
  config.geoCity = "";
  config.geoCountryCode = "";
  config.geoSsid = "";

  Serial.println("[ConfigManager] Configuration reset to defaults");
}

bool ConfigManager::isConfigured() const { return config.isValid(); }

bool ConfigManager::isConfigButtonHeld() {
  if (configButtonPin < 0) {
    return false;
  }

  // Check if button is pressed (active LOW with pullup)
  if (digitalRead(configButtonPin) != LOW) {
    return false;
  }

  Serial.println(
      "[ConfigManager] Config button pressed, checking hold duration...");

  // Wait and check if still held
  unsigned long startTime = millis();
  while (millis() - startTime < CONFIG_BUTTON_HOLD_TIME) {
    if (digitalRead(configButtonPin) != LOW) {
      Serial.println("[ConfigManager] Button released before timeout");
      return false;
    }
    delay(50);
  }

  Serial.println("[ConfigManager] Button held for required duration");
  return true;
}
