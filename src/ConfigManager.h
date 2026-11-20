/**
 * ConfigManager.h
 *
 * Web-based configuration management using WiFiManager and Preferences.
 * Provides a captive portal for configuring WiFi and custom parameters.
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <WiFiManager.h>

// Default configuration values
#define DEFAULT_REFRESH_MINUTES 30
#define DEFAULT_HOUR_INTERVAL 3
#define DEFAULT_LOW_POWER_MODE true
#define DEFAULT_CALENDAR_URL ""

// Button hold duration to trigger config portal (milliseconds)
#define CONFIG_BUTTON_HOLD_TIME 3000

// WiFiManager portal timeout (seconds) - auto-closes if no activity
#define CONFIG_PORTAL_TIMEOUT 300

// Preferences namespace
#define PREFS_NAMESPACE "weather-crow"

/**
 * Configuration structure holding all user-configurable parameters
 */
struct AppConfig {
  String latitude;
  String longitude;
  String locationName;
  String calendarUrl;
  int refreshMinutes;
  bool lowPowerMode;
  int hourInterval;
  String units;      // "metric" or "imperial"
  String timeFormat; // "12h" or "24h"
  bool enableAlertDisplay;
  String alertRegion; // "auto", "US", "CA", "NO", "DE"

  // Cached GeoJS lookup results (used when manual latitude/longitude are blank)
  String geoLatitude;
  String geoLongitude;
  String geoCity;
  String geoCountryCode;
  String geoSsid;

  // Default constructor with sensible defaults
  AppConfig()
      : refreshMinutes(15), lowPowerMode(false), hourInterval(1),
        units("metric"), timeFormat("24h"), enableAlertDisplay(true),
        alertRegion("auto") {}

  // Check if manual coordinates were entered by the user
  bool hasManualCoordinates() const {
    return latitude.length() > 0 && longitude.length() > 0;
  }

  // Check if cached GeoJS coordinates are available
  bool hasGeoCache() const {
    return geoLatitude.length() > 0 && geoLongitude.length() > 0;
  }

  // Check if any usable location exists (manual or cached GeoIP)
  bool isValid() const { return hasManualCoordinates() || hasGeoCache(); }
};

/**
 * ConfigManager - Handles WiFi configuration and persistent storage
 *
 * Usage:
 *   ConfigManager configManager;
 *   configManager.begin(CONFIG_BUTTON_PIN);
 *
 *   // Access configuration
 *   String lat = configManager.getLatitude();
 */
class ConfigManager {
public:
  ConfigManager();

  /**
   * Initialize ConfigManager with button pin for manual config trigger
   * @param buttonPin GPIO pin connected to config button (active LOW)
   * @return true if connected to WiFi, false if in config portal mode
   */
  bool begin(int buttonPin);

  /**
   * Force start the configuration portal
   * Blocks until configuration is complete or timeout
   * @return true if configuration was saved
   */
  bool startConfigPortal(const char *reason);

  /**
   * Load configuration from Preferences (NVS)
   */
  void loadConfig();

  /**
   * Save configuration to Preferences (NVS)
   */
  void saveConfig();

  /**
   * Reset configuration to defaults and clear stored WiFi credentials
   */
  void resetConfig();

  /**
   * Persist cached GeoJS lookup results to Preferences (NVS) without modifying
   * user-entered manual override values.
   */
  void saveGeoIpCache(const String &lat, const String &lon, const String &city,
                      const String &countryCode, const String &ssid);

  /**
   * Check if valid configuration exists
   */
  bool isConfigured() const;

  // Effective location getters (manual override takes precedence over GeoJS cache)
  String getLatitude() const {
    return config.hasManualCoordinates() ? config.latitude : config.geoLatitude;
  }
  String getLongitude() const {
    return config.hasManualCoordinates() ? config.longitude
                                         : config.geoLongitude;
  }
  String getLocationName() const {
    return config.locationName.length() > 0 ? config.locationName
                                            : config.geoCity;
  }

  // Manual vs cached GeoJS getters
  bool hasManualCoordinates() const { return config.hasManualCoordinates(); }
  bool hasGeoCache() const { return config.hasGeoCache(); }
  String getManualLatitude() const { return config.latitude; }
  String getManualLongitude() const { return config.longitude; }
  String getGeoLatitude() const { return config.geoLatitude; }
  String getGeoLongitude() const { return config.geoLongitude; }
  String getGeoCity() const { return config.geoCity; }
  String getGeoCountryCode() const { return config.geoCountryCode; }
  String getGeoSsid() const { return config.geoSsid; }

  // Configuration getters
  String getCalendarUrl() const { return config.calendarUrl; }
  int getRefreshMinutes() const { return config.refreshMinutes; }
  bool getLowPowerMode() const { return config.lowPowerMode; }
  int getHourInterval() const { return config.hourInterval; }
  String getUnits() const { return config.units; }
  String getTimeFormat() const { return config.timeFormat; }
  bool getEnableAlertDisplay() const { return config.enableAlertDisplay; }
  String getAlertRegion() const { return config.alertRegion; }

  // Get the full config struct
  const AppConfig &getConfig() const { return config; }

  /**
   * Set a callback function that will be called before the config portal starts
   * Use this to display instructions on the e-paper screen
   * @param callback Function pointer that takes AP name, IP address, and reason
   */
  void setPortalStartCallback(void (*callback)(const char *apName,
                                               const char *ipAddress,
                                               const char *reason)) {
    portalStartCallback = callback;
  }

private:
  AppConfig config;
  Preferences preferences;
  int configButtonPin;
  void (*portalStartCallback)(const char *, const char *,
                              const char *) = nullptr;

  // Non-owning pointers to active WiFiManager text parameters while portal runs
  WiFiManagerParameter *paramLatitude;
  WiFiManagerParameter *paramLongitude;
  WiFiManagerParameter *paramLocationName;
  WiFiManagerParameter *paramCalendarUrl;
  WiFiManagerParameter *paramRefreshMinutes;
  WiFiManagerParameter *paramHourInterval;

  /**
   * Check if config button is being held during boot
   */
  bool isConfigButtonHeld();

  /**
   * Callback when WiFiManager saves parameters
   */
  void saveParamsCallback(WiFiManager &wm);

  /**
   * Read parameters from WiFiManager after portal closes
   */
  void readWiFiManagerParams(WiFiManager &wm);
};

#endif // CONFIG_MANAGER_H
